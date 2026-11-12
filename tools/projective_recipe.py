"""Opt-in source recipe for locally held modules; this file contains no game shader.

Only the identified input hashes are accepted. Generated SPIR-V is private build
output and must not be attached to an issue or committed to the repository.
The recipe implements the tested coordinate representation, not its recorder.
"""
from pathlib import Path
import argparse
import json
import struct

VS_HASH = 0xF6D753852C47B0DE
FS_HASH = 0xA39C3D09241441FA
FS_SET1_HASH = 0x3D3A316AB4DA3BBA


def fnv(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def string(words):
    return struct.pack('<%dI' % len(words), *words).split(b'\0', 1)[0].decode()


class Module:
    def __init__(self, data):
        if len(data) < 20 or len(data) % 4:
            raise ValueError('Not a complete SPIR-V module')
        words = list(struct.unpack('<%dI' % (len(data) // 4), data))
        if words[0] != 0x07230203:
            raise ValueError('Expected little-endian SPIR-V')
        self.header, self.ops = words[:5], []
        at = 5
        while at < len(words):
            size, op = words[at] >> 16, words[at] & 65535
            if not size or at + size > len(words):
                raise ValueError('Malformed instruction')
            self.ops.append((op, words[at + 1:at + size]))
            at += size
        entries = [args for op, args in self.ops if op == 15]
        if len(entries) != 1 or sum(op == 54 for op, _ in self.ops) != 1:
            raise ValueError('This narrow recipe expects one entry point and function')
        if sum(op == 253 for op, _ in self.ops) != 1:
            raise ValueError('This narrow recipe expects one return')
        self.model = entries[0][0]
        self.types = {a[0]: (op, a[1:]) for op, a in self.ops if 19 <= op <= 33}
        self.variables = {a[1]: (a[0], a[2]) for op, a in self.ops if op == 59}
        self.extra = []

    def new(self):
        result = self.header[3]
        self.header[3] += 1
        return result

    def type(self, op, *args):
        for ident, definition in self.types.items():
            if definition == (op, list(args)):
                return ident
        ident = self.new()
        self.types[ident] = (op, list(args))
        self.extra.append((op, [ident, *args]))
        return ident

    def constant(self, ty, bits):
        for op, args in self.ops + self.extra:
            if op == 43 and args[0] == ty and args[2:] == [bits]:
                return args[1]
        ident = self.new()
        self.extra.append((43, [ty, ident, bits]))
        return ident

    def location0(self, storage):
        ids = [a[0] for op, a in self.ops if op == 71 and a[1:] == [30, 0]
               and a[0] in self.variables and self.variables[a[0]][1] == storage]
        if len(ids) != 1:
            raise ValueError('Expected one location-zero interface variable')
        var = ids[0]
        pointer = self.variables[var][0]
        op, args = self.types[pointer]
        if op != 32 or args[0] != storage:
            raise ValueError('Unexpected interface pointer')
        vec = args[1]
        op, args = self.types[vec]
        if op != 23 or args[1] != 4 or self.types[args[0]] != (22, [32]):
            raise ValueError('Expected a four-component float interface')
        return var, vec, args[0]

    def install_declarations(self):
        at = next(i for i, (op, _) in enumerate(self.ops) if op == 54)
        self.ops[at:at] = self.extra
        self.extra = []

    def bytes(self):
        words = self.header[:]
        for op, args in self.ops:
            words.extend([((len(args) + 1) << 16) | op, *args])
        return struct.pack('<%dI' % len(words), *words)


def vertex(data):
    mod = Module(data)
    if mod.model != 0:
        raise ValueError('Expected vertex stage')
    varying, vec, scalar = mod.location0(3)
    positions = [(a[0], a[1]) for op, a in mod.ops
                 if op == 72 and a[2:] == [11, 0]]
    if len(positions) != 1:
        raise ValueError('Expected one Position structure member')
    structure, member = positions[0]
    candidates = [var for var, (ptr, storage) in mod.variables.items()
                  if storage == 3 and mod.types[ptr] == (32, [3, structure])]
    if len(candidates) != 1 or mod.types[structure][1][member] != vec:
        raise ValueError('Unexpected Position block')
    pointer = mod.type(32, 3, vec)
    integer = mod.type(21, 32, 1)
    member_id = mod.constant(integer, member)
    one = mod.constant(scalar, 0x3F800000)
    ptr, pos, x, y, w, encoded = [mod.new() for _ in range(6)]
    code = [(65, [pointer, ptr, candidates[0], member_id]),
            (61, [vec, pos, ptr]),
            (81, [scalar, x, pos, 0]), (81, [scalar, y, pos, 1]),
            (81, [scalar, w, pos, 3]),
            (80, [vec, encoded, x, y, w, one]), (62, [varying, encoded])]
    at = next(i for i, (op, _) in enumerate(mod.ops) if op == 253)
    mod.ops[at:at] = code
    mod.install_declarations()
    return mod.bytes()


def fragment(data):
    mod = Module(data)
    if mod.model != 4:
        raise ValueError('Expected fragment stage')
    varying, vec, scalar = mod.location0(1)
    if sum(op == 71 and a == [varying, 13] for op, a in mod.ops) != 1:
        raise ValueError('Expected NoPerspective on the original input')
    mod.ops = [(op, a) for op, a in mod.ops if not (op == 71 and a == [varying, 13])]
    imports = [a[0] for op, a in mod.ops if op == 11 and string(a[1:]) == 'GLSL.std.450']
    if len(imports) != 1:
        raise ValueError('Expected GLSL extended instructions')
    local_pointer = mod.type(32, 7, vec)
    local = mod.new()
    # Redirect only original accesses. The raw input load added below must remain an input load.
    count = 0
    for op, args in mod.ops:
        if op in (65, 66) and args[2] == varying:
            old_op, old_args = mod.types[args[0]]
            if old_op != 32 or old_args[0] != 1:
                raise ValueError('Unexpected access pointer')
            args[0] = mod.type(32, 7, old_args[1])
            args[2] = local
            count += 1
        elif op == 61 and args[2] == varying:
            args[2] = local
            count += 1
    if count != 2:
        raise ValueError('Unexpected original varying-access count')
    zero = mod.constant(scalar, 0)
    one = mod.constant(scalar, 0x3F800000)
    half = mod.constant(scalar, 0x3F000000)
    negative_half = mod.constant(scalar, 0xBF000000)
    raw, x, y, w, nx, ny, u, v, decoded = [mod.new() for _ in range(9)]
    code = [(61, [vec, raw, varying]),
            (81, [scalar, x, raw, 0]), (81, [scalar, y, raw, 1]),
            (81, [scalar, w, raw, 2]),
            (136, [scalar, nx, x, w]), (136, [scalar, ny, y, w]),
            (12, [scalar, u, imports[0], 50, nx, half, half]),
            (12, [scalar, v, imports[0], 50, ny, negative_half, half]),
            (80, [vec, decoded, u, v, zero, one]), (62, [local, decoded])]
    at = next(i for i, (op, _) in enumerate(mod.ops) if op == 248) + 1
    while mod.ops[at][0] == 59:
        at += 1
    mod.ops[at:at] = [(59, [local_pointer, local, 7]), *code]
    mod.install_declarations()
    return mod.bytes()


def prepare(vs, fs):
    if fnv(vs) != VS_HASH or fnv(fs) not in (FS_HASH, FS_SET1_HASH):
        raise ValueError('Input hash differs from the narrowly tested shader pair')
    if fnv(fs) == FS_HASH:
        mod = Module(fs)
        for op, args in mod.ops:
            if op == 71 and args[1] == 34:
                if args[2] != 0:
                    raise ValueError('Unexpected descriptor set')
                args[2] = 1
        fs = mod.bytes()
        if fnv(fs) != FS_SET1_HASH:
            raise ValueError('Descriptor remapping did not match the recorded module')
    return vertex(vs), fragment(fs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vertex', required=True, type=Path)
    parser.add_argument('--fragment', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    vs, fs = prepare(args.vertex.read_bytes(), args.fragment.read_bytes())
    args.output.mkdir(parents=True, exist_ok=True)
    files = {f'{VS_HASH:016x}.spv': vs, f'{FS_SET1_HASH:016x}.spv': fs}
    if any((args.output / name).exists() for name in [*files, 'recipe-manifest.json']):
        raise ValueError('Output exists; choose a new ignored build directory')
    for name, data in files.items():
        (args.output / name).write_bytes(data)
    manifest = {'scope': 'Only the identified vertex/fragment pair, changed together',
                'outputs': {name: {'fnv': f'{fnv(data):016x}', 'bytes': len(data)}
                            for name, data in files.items()},
                'warning': 'Generated modules are game-derived private build output; do not upload them'}
    (args.output / 'recipe-manifest.json').write_text(json.dumps(manifest, indent=2))
    print('Created private local modules. Validate them before integration; do not upload them.')


if __name__ == '__main__':
    main()
