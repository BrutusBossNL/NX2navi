"""Build and run tests/frame_step_check.c: the step runtime/frame_rate.c gives Ocarina, with the rest of the runtime
stubbed. No game files: the module layout the runtime includes is a stand-in written to the test's build folder.

Usage: frame_step_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
sys.path[:0] = [str(ROOT / 'recomp')]
from patches import load  # noqa: E402

PAGE = 0x4ea0000   # the patch page of 1.1.1, main+0x4ea0000
# The sites that call frame_rate.c's hosts, as recomp/patches-v111.txt has them: set, module offset, the original word,
# the words before the bl (the original's own, where the host comes after it), the page offset the bl reaches.
FDIV = 0x1e201900   # fdiv s0, s8, s0
HOST_SITES = [('ostep', 0x3cdfc0, 0x1e380008, [], 0xf60), ('ostep', 0x475f8, 0x1e380009, [], 0xf50),
              ('owater', 0x3876014, FDIV, [FDIV], 0xf20), ('owater', 0x38827f8, FDIV, [FDIV], 0xf20),
              ('owater', 0x632cfc, FDIV, [FDIV], 0xf20),
              ('osound', 0x41531c, 0x910083fd, [0x910083fd], 0xf10), ('osound', 0x415334, 0xbd401100, [0xbd401100], 0xf00)]


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'

LAYOUT = '''// A stand-in for the generated layout.h (tests/frame_step_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
'''


def check_sites():
    """Each host site is its listed words and one bl to its page slot, over its original word."""
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', 0x7000000000, PAGE)])
    bad = 0
    for name, site, original, before, slot in HOST_SITES:
        (key, words, have), = table.sites['main'][site]
        bl = words[-1] if isinstance(words[-1], int) else 0
        target = site + (((bl & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4
        ok = (table.sets[key][0] == name and have == original and words[:-1] == before and bl >> 26 == 0x25 and
              target == PAGE + slot)
        if not ok:
            print(f'FAIL: the site main+{site:#x} of set {name} is {words} over {have:#x}')
            bad += 1
    return bad


def main():
    work = ROOT / 'build' / 'test' / 'frame_step'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    exe = work / ('frame_step_check.exe' if os.name == 'nt' else 'frame_step_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-I', str(ROOT / 'runtime'), '-I', str(work),
                           str(ROOT / 'tests' / 'frame_step_check.c'), '-o', str(exe), '-lm'], env=env)
    rc = subprocess.run([str(exe)], env=env).returncode
    return subprocess.run([str(exe), 'mean'], env=env).returncode or rc or check_sites()


if __name__ == '__main__':
    sys.exit(main())
