# tools/android/ondevice/selftest.py: the generator's Python modules on the phone, each actually used once
# (docs/android-ondevice-build.md): capstone disassembles, unicorn runs ARM64 code, lz4 round-trips a block.
import struct, sys, time

t0 = time.time()
import capstone
import unicorn as uc
from unicorn import arm64_const as A
import lz4.block

print('python', sys.version.split()[0], sys.platform, 'imports %.2fs' % (time.time() - t0))
code = struct.pack('<3I', 0x8B020020, 0xD2800541, 0xD65F03C0)  # add x0,x1,x2 ; mov x1,#42 ; ret
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
print('capstone', capstone.__version__, [f'{i.mnemonic} {i.op_str}' for i in md.disasm(code, 0x1000)])
mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
mu.mem_map(0x1000, 0x1000)
mu.mem_write(0x1000, code)
mu.reg_write(A.UC_ARM64_REG_X1, 40)
mu.reg_write(A.UC_ARM64_REG_X2, 2)
mu.emu_start(0x1000, 0x1008)
print('unicorn', uc.__version__, 'x0 =', mu.reg_read(A.UC_ARM64_REG_X0), 'x1 =', mu.reg_read(A.UC_ARM64_REG_X1))
data = bytes(range(256)) * 4096
t0 = time.time()
c = lz4.block.compress(data, store_size=False)
assert lz4.block.decompress(c, uncompressed_size=len(data)) == data
print('lz4', lz4.__version__, len(data), '->', len(c), 'bytes, round trip %.3fs' % (time.time() - t0))
