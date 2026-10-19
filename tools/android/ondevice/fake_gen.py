# tools/android/ondevice/fake_gen.py: a stand-in for tools/gen.py in the on-phone test of runtime/gamebuild.c
# (phone_gamebuild_test.sh) - no game files: it writes a few synthetic chunks in gen.py's layout and progress protocol
# (<name>_NNN.c, <name>_table.c, "<name>: progress <i>/<n> files <k>" on stderr), one of them a split-dispatch file,
# and checks the command line runtime/gamebuild.c gives it.
import os, sys, time

SPLIT_MARK = '/* lasgen: split-dispatch */'
name, nso, base, out = sys.argv[1:5]
rest = sys.argv[5:]
assert os.path.isfile(nso), nso
assert '--shift' in rest and '--split-dispatch' in rest, rest
assert (name == 'sdk') == ('--hle' in rest), rest
os.makedirs(out, exist_ok=True)
base = int(base, 16)
n, per = 6, 40
fns = []
for k in range(n):
    body = [SPLIT_MARK + '\n' if k == 2 else '', '#include "cpu.h"\n']
    for j in range(per):
        a = base + (k * per + j) * 0x10
        fns.append(a)
        body.append(f'void f_{a:010x}(Cpu *CPU_RESTRICT c) {{\n    c->x[0] = c->x[1] * {j + 3} + 0x{a:x}ull;\n'
                    + ('    for (int i = 0; i < 8; i++) c->x[2] += c->x[i];\n' if k == 2 else '') + '}\n')
    with open(os.path.join(out, f'{name}_{k:03d}.c'), 'w') as f:
        f.write(''.join(body))
    print(f'{name}: progress {len(fns)}/{n * per} files {k + 1}', file=sys.stderr, flush=True)
    time.sleep(0.3)
with open(os.path.join(out, f'{name}_table.c'), 'w') as f:
    f.write('#include "cpu.h"\n' + ''.join(f'void f_{a:010x}(Cpu *c);\n' for a in fns))
    f.write(f'const uint64_t {name}_fn_addrs[] = {{' + ','.join(f'0x{a:x}' for a in fns) + '};\n')
    f.write(f'const GuestFn {name}_fn_ptrs[] = {{' + ','.join(f'f_{a:010x}' for a in fns) + '};\n')
    f.write(f'const uint64_t {name}_fn_count = {len(fns)};\n')
print(f'{name}: wrote {n} chunks (1 split dispatch), 0s total', file=sys.stderr)
