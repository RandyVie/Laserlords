"""Write the entries file DecompileOS9.java consumes (function starts + TRAP sites).
Usage: python3 tools/ghidra_entries.py ext/cdi_tribes out/ghidra/cdi_tribes.entries"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from disasm import Module
from os9 import FUNCS
m = Module(sys.argv[1]); m.run()
with open(sys.argv[2], 'w') as f:
    for a in sorted(m.funcs): f.write(f'{a:x}\n')
    f.write(f'label {m.exec:x} _cstart\n')
    for a, n, code in m.traps:
        name = FUNCS.get(code, f'0x{code:x}') if n == 0 else f'trap{n}_0x{code:x}'
        f.write(f'trap {a:x} {name}\n')
