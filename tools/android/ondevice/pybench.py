# tools/android/ondevice/pybench.py: a pure-Python workload shaped like the generator's (struct decoding, dict / set
# bookkeeping, f-string C emission, regex passes) to compare this machine's Python speed with another's
# (docs/android-ondevice-build.md: the phone / Mac ratio estimates the generator's time on the phone).
import re, struct, time

def run(n):
    words = struct.pack(f'<{n}I', *[(i * 2654435761) & 0xFFFFFFFF for i in range(n)])
    seen, out, labels = set(), [], {}
    for i, (w,) in enumerate(struct.iter_unpack('<I', words)):
        op, rd, rn, imm = w >> 24, w & 31, (w >> 5) & 31, (w >> 10) & 0xFFF
        if op & 3 == 0:
            labels[i * 4] = f'L_{i * 4:x}'
        key = (op, rd, rn)
        if key not in seen:
            seen.add(key)
        out.append(f'    x[{rd}] = x[{rn}] + 0x{imm:x}ull; /* {op:02x} */\n')
    text = ''.join(out)
    text = re.sub(r'x\[31\]', 'sp', text)
    return len(text) + len(labels) + len(seen)

t0 = time.perf_counter()
r = 0
for _ in range(5):
    r += run(400000)
print(f'pybench {time.perf_counter() - t0:.2f}s ({r})')
