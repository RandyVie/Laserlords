"""List the I$SetStt/I$GetStt library wrappers in a module: entry address, status code,
sub-code (D2) and how arguments map to registers. Linear disassembly of a range.
Usage: python3 tools/wrappers.py ext/cdi_launcher 0x1d80 0x2100"""
import sys, struct
import capstone as cs
b = open(sys.argv[1], 'rb').read()
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
md = cs.Cs(cs.CS_ARCH_M68K, cs.CS_MODE_M68K_000)
a = lo
while a < hi:
    if b[a:a+2] == b'\x4e\x40':
        print(f'{a:05x}  trap #0 {struct.unpack(">H", b[a+2:a+4])[0]:#06x}'); a += 4; continue
    ins = list(md.disasm(b[a:a+10], a, 1))
    if not ins: print(f'{a:05x}  dc.w {b[a:a+2].hex()}'); a += 2; continue
    i = ins[0]
    print(f'{a:05x}  {i.mnemonic:8} {i.op_str}'); a += i.size
