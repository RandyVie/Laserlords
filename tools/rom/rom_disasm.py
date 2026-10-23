"""Linear 68000 disassembly of one module inside a CD-i ROM (or any OS-9 module file).

    python3 tools/rom/rom_disasm.py ROM MODULE [START_HEX]
MODULE is a module name from rom_modules.py (e.g. video, sgstom, kernel, init). Without START
the listing begins at the module's exec offset (+0x30). Offsets are module-relative.
TRAP #0 is followed by a 16-bit OS-9 function code, printed as dc.w.

Finding a driver's SetStat handler: a driver module starts its exec area with a table of
16-bit offsets: init, read, write, getstat, setstat, term, exception. For the CD-i video
driver the SetStat code dispatches 0x51-0x57 through a table; 0x56 (display control) then
indexes a second table by sub-code (see GUIDE_NEW_GAME.md, "Reading the ROM")."""
import struct
import sys

import capstone as cs

sys.path.insert(0, __import__('os').path.dirname(__file__))
from rom_modules import modules  # noqa: E402


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    rom = open(sys.argv[1], 'rb').read()
    found = [m for m in modules(rom) if m[2] == sys.argv[2]]
    if not found:
        sys.exit(f'no module {sys.argv[2]!r} in {sys.argv[1]}')
    off, size, _, _ = found[0]
    m = rom[off:off + size]
    start = int(sys.argv[3], 16) if len(sys.argv) > 3 else struct.unpack_from('>I', m, 0x30)[0]
    md = cs.Cs(cs.CS_ARCH_M68K, cs.CS_MODE_M68K_000)
    i = start
    while i < size:
        ins = next(md.disasm(m[i:i + 10], i, 1), None)
        if ins is None:
            print(f'{i:5x}  dc.w {m[i:i + 2].hex()}')
            i += 2
            continue
        print(f'{i:5x}  {m[i:i + ins.size].hex():<20} {ins.mnemonic} {ins.op_str}')
        i += ins.size
        if ins.mnemonic == 'trap' and i + 2 <= size:
            print(f'{i:5x}  dc.w {struct.unpack_from(">H", m, i)[0]:04x}')
            i += 2


if __name__ == '__main__':
    main()
