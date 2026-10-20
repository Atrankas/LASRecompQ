#!/usr/bin/env python3
"""tools/hot_functions.py <profile.profraw|.profdata>... : tools/hot_functions.txt from play profiles.

The profiles come from an instrumented game library (tools/android/build_game.sh --pgo-gen, played with
LARECOMP_PGO_DUMP_EVERY in env.txt; build/pgo/lasgame.profraw in the app's data folder). Hot: the busiest functions
that together ran SHARE of all the basic blocks the game ran (default 0.99999) - "<module> <hex offset from the
module's base>" lines, which tools/gen.py --hot reads; the rest of the game's functions are compiled unoptimized
(runtime/cpu.h COLD_FN). Measured on one profile: 99.99% of the work is in ~6600 of the 121651 functions, 99.999% in
~10100; a function that did one block in 100000 costs nothing noticeable at -O0. Only addresses: nothing of the game's
code. LLVM_PROFDATA=<llvm-profdata> (default: the Android NDK's).
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

BASES = (('sdk', 0x7004000000), ('main', 0x7000000000))  # (tools/android/build_game.sh: gen.py's base per module)


def profdata_tool():
    if os.environ.get('LLVM_PROFDATA'):
        return os.environ['LLVM_PROFDATA']
    sdk = os.environ.get('ANDROID_HOME') or os.path.expanduser('~/Android/Sdk')
    for p in sorted(glob.glob(os.path.join(sdk, 'ndk', '*', 'toolchains', 'llvm', 'prebuilt', '*', 'bin', 'llvm-profdata'))):
        return p
    return 'llvm-profdata'


def main():
    profiles = sys.argv[1:]
    if not profiles:
        sys.exit(__doc__)
    tool = profdata_tool()
    with tempfile.TemporaryDirectory() as tmp:
        merged = os.path.join(tmp, 'merged.profdata')
        subprocess.run([tool, 'merge', '-o', merged] + profiles, check=True)
        out = subprocess.run([tool, 'show', '--all-functions', '--counts', merged], check=True, capture_output=True,
                             text=True).stdout
    share = float(os.environ.get('SHARE', '0.99999'))
    work, seen, name = {}, 0, None
    for line in out.splitlines():
        m = re.match(r'\s+f_([0-9a-f]+):$', line)
        if m:
            name = int(m.group(1), 16)
            seen += 1
            continue
        m = re.match(r'\s+Block counts: \[(.*)\]', line)  # (IR instrumentation: ran if any of its blocks did)
        if m and name is not None:
            work[name] = sum(int(v) for v in m.group(1).split(',') if v.strip())
            name = None
    total, acc, hot = sum(work.values()), 0, set()
    for a, n in sorted(work.items(), key=lambda kv: -kv[1]):
        if n == 0 or acc >= share * total:
            break
        hot.add(a)
        acc += n
    lines = []
    for a in sorted(hot):
        mod, base = next((n, b) for n, b in BASES if a >= b)
        lines.append(f'{mod} {a - base:x}\n')
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hot_functions.txt')
    with open(path, 'w') as f:
        f.write(''.join(lines))
    ran = sum(1 for n in work.values() if n)
    print(f'{len(hot)} hot functions ({share:g} of the work) of {ran} that ran, {seen} in all: {path}')


if __name__ == '__main__':
    main()
