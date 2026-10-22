"""Recursive-descent 68000 disassembler for OS-9 program modules.
Handles the OS-9 convention that TRAP #n is followed by a 16-bit function code.
Usage: python3 tools/disasm.py ext/cdi_tribes [out.s]"""
import struct, sys, re
import capstone as cs
sys.path.insert(0, __import__('os').path.dirname(__file__))
from os9 import FUNCS

def u16(b, o): return struct.unpack_from('>H', b, o)[0]
def u32(b, o): return struct.unpack_from('>I', b, o)[0]

class Module:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        b = self.b
        self.size = u32(b, 4)
        self.name = b[u32(b, 0xC):].split(b'\0')[0].decode()
        self.exec, self.excpt = u32(b, 0x30), u32(b, 0x34)
        self.idata = u32(b, 0x40)
        self.code_end = self.idata  # code lives between header and initialized data
        self.md = cs.Cs(cs.CS_ARCH_M68K, cs.CS_MODE_M68K_000)
        self.md.detail = True
        self.insns = {}      # addr -> (size, text, insn)
        self.funcs = set()
        self.labels = set()
        self.traps = []      # (addr, trapno, code)

    def one(self, a):
        for i in self.md.disasm(self.b[a:a + 10], a, 1):
            return i
        return None

    def target(self, i):
        for o in i.operands:
            if o.type == cs.m68k.M68K_OP_BR_DISP:
                return i.address + 2 + o.br_disp.disp
            if o.type == cs.m68k.M68K_OP_IMM and i.mnemonic.startswith(('b', 'db')):
                return o.imm
            if o.type == cs.m68k.M68K_OP_MEM and o.address_mode == cs.m68k.M68K_AM_PCI_DISP:
                return i.address + 2 + o.mem.disp
        m = re.search(r'\$([0-9a-f]+)$', i.op_str)
        if m and i.mnemonic.startswith(('b', 'db')):
            return int(m.group(1), 16)
        return None

    def walk(self, entries):
        work = list(entries)
        while work:
            a = work.pop()
            while 0x48 <= a < self.code_end and a not in self.insns:
                w = u16(self.b, a)
                if 0x4E40 <= w <= 0x4E4F:  # TRAP #n + code word
                    code = u16(self.b, a + 2)
                    self.traps.append((a, w & 15, code))
                    fn = FUNCS.get(code, hex(code)) if (w & 15) == 0 else hex(code)
                    self.insns[a] = (4, f'trap     #{w & 15}, {fn}', None)
                    if (w & 15) == 0 and code == 0x06:  # F$Exit never returns
                        break
                    a += 4; continue
                i = self.one(a)
                if i is None:
                    self.insns[a] = (2, f'dc.w     ${w:04x}  ; ???', None); break
                self.insns[a] = (i.size, f'{i.mnemonic:8} {i.op_str}', i)
                m = i.mnemonic
                t = self.target(i)
                if m in ('rts', 'rte', 'rtr', 'illegal'):
                    break
                if m in ('bsr', 'bsr.b', 'bsr.w', 'jsr') or m.startswith('bsr'):
                    if t is not None: self.funcs.add(t); work.append(t)
                elif m.startswith('bra') or m == 'jmp':
                    if t is not None: self.labels.add(t); work.append(t)
                    break
                elif m.startswith(('b', 'db')) and t is not None:
                    self.labels.add(t); work.append(t)
                a += i.size

    def run(self):
        self.funcs |= {self.exec, self.excpt} - {0}
        entries = set(self.funcs)
        # Microware C opens every function with LINK A5,#n (0x4E55); cdi_nvrui was
        # built with a different compiler that uses LINK A6 (0x4E56) instead.
        for a in range(0x48, self.code_end, 2):
            if u16(self.b, a) in (0x4E55, 0x4E56):
                entries.add(a)
        self.walk(entries)
        for a in entries:
            if u16(self.b, a) in (0x4E55, 0x4E56): self.funcs.add(a)

    def listing(self):
        out = [f'; {self.name}  exec=${self.exec:x} excpt=${self.excpt:x} idata=${self.idata:x}']
        a = 0x48
        while a < self.code_end:
            if a in self.insns:
                sz, txt, _ = self.insns[a]
                if a in self.funcs: out.append(f'\nsub_{a:05x}:')
                elif a in self.labels: out.append(f'loc_{a:05x}:')
                raw = self.b[a:a + sz].hex()
                out.append(f'  {a:05x}  {raw:20} {txt}')
                a += sz
            else:
                s = a
                while a < self.code_end and a not in self.insns: a += 2
                out.append(f'  {s:05x}  ; data {a - s} bytes: {self.b[s:min(a, s + 24)].hex()}{"..." if a - s > 24 else ""}')
        return '\n'.join(out)

if __name__ == '__main__':
    m = Module(sys.argv[1]); m.run()
    txt = m.listing()
    if len(sys.argv) > 2: open(sys.argv[2], 'w').write(txt)
    else: print(txt)
    cov = sum(v[0] for v in m.insns.values())
    print(f'{m.name}: {len(m.funcs)} functions, {cov}/{m.code_end - 0x48} code bytes covered, {len(m.traps)} traps', file=sys.stderr)
