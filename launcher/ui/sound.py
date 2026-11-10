"""The launcher's sound effects: the game's own interface sounds, the ones its menus make for the cursor, a choice
and going back, read from the player's game (launcher/gamefiles.py extract_sounds decodes them into
data/cache/sounds, as runtime/menu_sound.c plays them in the in-game menu). No sound file ships: until the player's
game has been read, and for a sound it does not hold, short chimes made here from sine tones stand in.

They are off until the player picks a loudness in Customize Launcher (the Sounds row). An effect is a 16-bit WAV in
memory: the game's sound scaled to the loudness (the cursor's a little under the others, as in the game's menu), or a
few plucked notes (a tone with two overtones, a quick attack and an exponential decay). Windows plays it with
PlaySound from that memory; elsewhere the WAV goes to a file in the cache folder and the first of pw-play, paplay
and aplay found plays it. One effect sounds at a time: a new one cuts the last, so the cursor's sound is left out
for a quarter of a second after a choice, which the pointer landing on a row of the next screen would otherwise cut.

    move      the cursor went to another row           the game's cursor sound
    toggle    a switch or a choice changed             the cursor sound
    select    a row was chosen (another screen)        the game's decide sound
    back      back to the screen before                the game's cancel sound
    start     Play                                     the decide sound
    done      an install, update or rebuild finished   the decide sound
    error     something could not be done              the cancel sound
"""
import ctypes
import os
import shutil
import struct
import subprocess
import time

import numpy as np

RATE = 44100
LEVELS = {'off': 0.0, 'low': 0.18, 'medium': 0.4, 'high': 0.75}   # the Sounds choice: the loudest sample's share of full scale
# Which of the game's sounds (gamefiles.SOUNDS) each effect plays, and how loud beside the others.
GAME_SOUND = {'move': ('move', 0.6), 'toggle': ('move', 0.6), 'select': ('select', 1.0), 'start': ('select', 1.0),
              'done': ('select', 1.0), 'back': ('back', 1.0), 'error': ('back', 1.0)}
QUIET_AFTER = 0.25   # seconds after a choice in which the cursor's sound is left out

# Notes as (start in seconds, frequency in Hz, length in seconds, loudness 0..1).
C5, D5, E5, G5, A5, C6, D6, E6, G6, C7 = 523.25, 587.33, 659.25, 783.99, 880.0, 1046.5, 1174.66, 1318.51, 1567.98, 2093.0
EFFECTS = {
    'move': [(0, E6, 0.07, 0.55)],
    'toggle': [(0, G5, 0.10, 0.8), (0.035, D6, 0.10, 0.5)],
    'select': [(0, C6, 0.12, 0.8), (0.07, G6, 0.20, 0.8)],
    'back': [(0, G5, 0.12, 0.8), (0.07, C5, 0.20, 0.7)],
    'start': [(0, C6, 0.14, 0.7), (0.08, E6, 0.14, 0.7), (0.16, G6, 0.14, 0.7), (0.24, C7, 0.45, 0.8)],
    'done': [(0, G5, 0.16, 0.8), (0.12, C6, 0.16, 0.8), (0.24, E6, 0.16, 0.8), (0.36, G6, 0.5, 0.9)],
    'error': [(0, 220.0, 0.16, 0.9), (0.14, 174.61, 0.3, 0.9)],
}


def pluck(freq, length, loudness):
    """One note: the tone and two quieter overtones, in within 4 ms and dying away over its length."""
    n = int(RATE * length)
    t = np.arange(n, dtype=np.float32) / RATE
    wave = np.sin(2 * np.pi * freq * t) + 0.35 * np.sin(2 * np.pi * 2 * freq * t) + 0.12 * np.sin(2 * np.pi * 3 * freq * t)
    env = np.minimum(t / 0.004, 1) * np.exp(-t * 5.5 / length)
    tail = np.minimum((length - t) / 0.01, 1)   # the last 10 ms go to nothing, so the end does not click
    return wave * env * tail * loudness


def synth(name):
    """The effect's samples in -1..1, its loudest at 1."""
    notes = EFFECTS[name]
    total = int(RATE * max(start + length for start, _f, length, _l in notes)) + 1
    out = np.zeros(total, dtype=np.float32)
    for start, freq, length, loudness in notes:
        note = pluck(freq, length, loudness)
        at = int(RATE * start)
        out[at:at + len(note)] += note
    peak = float(np.abs(out).max())
    return out / peak if peak > 0 else out


def wav_bytes(samples, level, rate=RATE, channels=1):
    """A 16-bit WAV of samples (-1..1; frames by channels when there are two) at level (0..1 of full scale)."""
    pcm = (np.clip(samples * level, -1, 1) * 32767).astype('<i2').tobytes()
    return (b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt '
            + struct.pack('<IHHIIHH', 16, 1, channels, rate, rate * 2 * channels, 2 * channels, 16)
            + b'data' + struct.pack('<I', len(pcm)) + pcm)


def read_wav(data):
    """A 16-bit PCM WAV as written by gamefiles.pcm_to_wav: (rate, channels, samples in -1..1 with its loudest at 1),
    or None when it is another kind of file."""
    if len(data) < 44 or data[:4] != b'RIFF' or data[8:16] != b'WAVEfmt ' or data[36:40] != b'data':
        return None
    kind, channels, rate, _bytes, _align, bits = struct.unpack_from('<HHIIHH', data, 20)
    size = min(struct.unpack_from('<I', data, 40)[0], len(data) - 44)
    if kind != 1 or bits != 16 or channels not in (1, 2) or size < 2 * channels:
        return None
    a = np.frombuffer(data, dtype='<i2', count=size // 2, offset=44).astype(np.float32)
    peak = float(np.abs(a).max())
    return rate, channels, a / peak if peak > 0 else a


class Sounds:
    """Plays the effects at the level of the Sounds choice. Every failure is silent: a PC without a sound device, or
    without a player program, just has a quiet launcher."""

    def __init__(self, cache_dir, level='off', log=None):
        self.cache = cache_dir
        self.log = log or (lambda line: None)
        self.level = None     # set_level below: 'off' unless the player chose a loudness
        self.game = {}        # the game's sounds decoded from the player's files: name -> (rate, channels, samples)
        self.last_choice = 0.0   # when a sound other than the cursor's last started
        self.wavs = {}        # name -> WAV bytes at the current level (Windows plays from this memory: it must stay)
        self.files = {}       # name -> the WAV file elsewhere
        self.player = None    # elsewhere: the program that plays a WAV file
        self.winmm = None
        self.failed = False
        if os.name == 'nt':
            try:
                self.winmm = ctypes.WinDLL('winmm')
                self.winmm.PlaySoundW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
                self.winmm.PlaySoundW.restype = ctypes.c_int
            except (OSError, AttributeError) as e:
                self.log(f'sounds: {e}')
        else:
            self.player = next((p for p in ('pw-play', 'paplay', 'aplay') if shutil.which(p)), None)
        self.set_level(level)
        self.load_game()

    def load_game(self):
        """Takes up the game's sounds decoded into <cache>/sounds (again after gamefiles.extract_sounds wrote them)."""
        game = {}
        for name in {g for g, _gain in GAME_SOUND.values()}:
            try:
                got = read_wav((self.cache / 'sounds' / f'{name}.wav').read_bytes())
            except OSError:
                got = None
            if got:
                game[name] = got
        if set(game) != set(self.game):
            self.stop()
            self.game, self.wavs, self.files = game, {}, {}
        return bool(game)

    def set_level(self, level):
        level = level if level in LEVELS else 'off'
        if level == self.level:
            return
        self.stop()
        self.level = level
        self.wavs, self.files = {}, {}

    def wav(self, name):
        if name not in self.wavs:
            source, gain = GAME_SOUND[name]
            if source in self.game:
                rate, channels, samples = self.game[source]
                self.wavs[name] = wav_bytes(samples, LEVELS[self.level] * gain, rate, channels)
            else:
                self.wavs[name] = wav_bytes(synth(name), LEVELS[self.level])
        return self.wavs[name]

    def stop(self):
        """Ends what is sounding, before the memory it plays from goes."""
        if self.winmm:
            try:
                self.winmm.PlaySoundW(None, None, 0)
            except OSError:
                pass

    def play(self, name):
        if LEVELS.get(self.level, 0) <= 0 or self.failed or name not in EFFECTS:
            return
        now = time.monotonic()
        if GAME_SOUND[name][0] == 'move':
            if now - self.last_choice < QUIET_AFTER:
                return
        else:
            self.last_choice = now
        try:
            if self.winmm:
                # SND_ASYNC | SND_NODEFAULT | SND_MEMORY: PlaySound reads the bytes while it plays, and self.wavs
                # keeps them until the level changes (set_level stops the sound first)
                self.winmm.PlaySoundW(self.wav(name), None, 0x0001 | 0x0002 | 0x0004)
            elif self.player:
                if name not in self.files:
                    path = self.cache / f'sound-{name}-{self.level}.wav'
                    path.write_bytes(self.wav(name))
                    self.files[name] = path
                subprocess.Popen([self.player, str(self.files[name])], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        except (OSError, ValueError) as e:   # no device, no player: stay quiet from here on
            self.failed = True
            self.log(f'sounds: {e}')
