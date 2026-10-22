"""M1: catalogue every TRAP site in each program module, with the setstat/getstat
code (D1) and sub-code (D2) recovered from the preceding instructions, plus how
often each wrapper function is called.
Usage: python3 tools/syscalls.py ext/cdi_launcher ext/cdi_tribes ..."""
import sys, re, collections, os
sys.path.insert(0, os.path.dirname(__file__))
from disasm import Module
from os9 import FUNCS

IMM = re.compile(r'^(moveq|move\.w|move\.l)\s+#\$?(-?[0-9a-f]+), (d[0-7])$')

def regs_before(m, addr, n=10):
    """Walk back through linearly preceding instructions, return {reg: imm}."""
    seq = sorted(a for a in m.insns if a < addr)[-n:]
    vals = {}
    for a in seq:
        _, txt, _ = m.insns[a]
        t = ' '.join(txt.split())
        mm = IMM.match(t)
        if mm:
            v = int(mm.group(2), 16)
            if mm.group(1) == 'moveq' and v > 127: v -= 256
            vals[mm.group(3)] = v
        else:
            for r in re.findall(r'\b(d[0-7])$', t):
                vals.pop(r, None)
            if t.startswith('exg'): vals.clear()
    return vals

def func_of(m, a):
    fs = [f for f in m.funcs if f <= a]
    return max(fs) if fs else None

def callers(m):
    c = collections.Counter()
    for a, (sz, txt, i) in m.insns.items():
        if i is not None and (i.mnemonic.startswith('bsr') or i.mnemonic == 'jsr'):
            t = m.target(i)
            if t is not None: c[t] += 1
    return c

for path in sys.argv[1:]:
    m = Module(path); m.run()
    calls = callers(m)
    print(f'## {m.name}\n')
    print('| site | function | calls to fn | trap | D1 | D2 |')
    print('|---|---|---|---|---|---|')
    for a, n, code in sorted(m.traps):
        r = regs_before(m, a)
        f = func_of(m, a)
        name = FUNCS.get(code, hex(code)) if n == 0 else f'#{n}:{code:#x}'
        d1 = f"{r['d1']:#x}" if 'd1' in r and code in (0x8D, 0x8E, 0x53, 0x56) else ''
        d2 = f"{r['d2']:#x}" if 'd2' in r and code in (0x8D, 0x8E) else ''
        print(f'| {a:05x} | sub_{f:05x} | {calls.get(f, 0)} | {name} | {d1} | {d2} |')
    print()
