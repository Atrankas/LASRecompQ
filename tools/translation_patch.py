#!/usr/bin/env python3
"""tools/translation_patch.py <translation folder> <the game's romfs> <out.lasp>

A fan translation as a patch against the player's own game files, so an APK can offer it without carrying any of
the game's files: for each file of the translation (romfs/... paths, as in translations/<lang>/), the operations that
make it from the game's own file of the same path - copy a range of the original, or add new bytes (the translated
text). The app applies it on the phone to the player's files (android LasActivity.applyTranslationPatch) and writes
translations/<lang>/romfs/..., where the Language setting finds it (runtime/settings.c).

Format (zlib-compressed as a whole): "LASP1", u32 file count; per file: u16 path length, path (UTF-8, under romfs/),
32-byte SHA-256 of the original it applies to, u32 the result's size, u32 op count; per op: u8 kind (1 copy: u32
offset, u32 length; 2 add: u32 length, bytes). Little-endian.
"""
import hashlib
import os
import struct
import sys
import zlib

BLOCK = 32  # (matches of at least this many bytes are copied from the original)


def ops_for(orig, new):
    index = {}
    for o in range(0, len(orig) - BLOCK + 1, 4):
        index.setdefault(orig[o:o + BLOCK], o)
    ops, i, add_from = [], 0, 0
    while i <= len(new) - BLOCK:
        o = index.get(new[i:i + BLOCK])
        if o is None:
            i += 1
            continue
        while o > 0 and i > add_from and orig[o - 1] == new[i - 1]:  # (grow the match backwards)
            o, i = o - 1, i - 1
        n = BLOCK
        while o + n < len(orig) and i + n < len(new) and orig[o + n] == new[i + n]:
            n += 1
        if i > add_from:
            ops.append((2, new[add_from:i]))
        ops.append((1, o, n))
        i += n
        add_from = i
    if add_from < len(new):
        ops.append((2, new[add_from:]))
    return ops


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    tr, romfs, out = sys.argv[1:]
    files = []
    for root, _, names in os.walk(os.path.join(tr, 'romfs')):
        for n in sorted(names):
            p = os.path.join(root, n)
            files.append(os.path.relpath(p, tr).replace(os.sep, '/'))
    body, added, total = bytearray(), 0, 0
    body += struct.pack('<I', len(files))
    for rel in sorted(files):
        new = open(os.path.join(tr, rel), 'rb').read()
        orig = open(os.path.join(romfs, rel[len('romfs/'):]), 'rb').read()
        ops = ops_for(orig, new)
        pb = rel.encode()
        body += struct.pack('<H', len(pb)) + pb + hashlib.sha256(orig).digest() + struct.pack('<II', len(new), len(ops))
        for op in ops:
            if op[0] == 1:
                body += struct.pack('<BII', 1, op[1], op[2])
            else:
                body += struct.pack('<BI', 2, len(op[1])) + op[1]
                added += len(op[1])
        total += len(new)
        # (check: the patch makes the translation's file again)
        made = bytearray()
        for op in ops:
            made += orig[op[1]:op[1] + op[2]] if op[0] == 1 else op[1]
        assert bytes(made) == new, rel
    data = zlib.compress(b'LASP1' + bytes(body), 9)
    open(out, 'wb').write(data)
    print(f'{len(files)} files, {total / 1e6:.2f} MB: {added / 1e6:.2f} MB new bytes, {(total - added) / 1e6:.2f} MB copied from '
          f'the game; patch {len(data) / 1e6:.2f} MB')


if __name__ == '__main__':
    main()
