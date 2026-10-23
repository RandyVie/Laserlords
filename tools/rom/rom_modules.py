"""List (and optionally extract) the OS-9 modules in a CD-i system ROM.

The ROM is the user's own file and is never committed. Usage:
    python3 tools/rom/rom_modules.py cdi220b.rom                 # table of modules
    python3 tools/rom/rom_modules.py cdi220b.rom --extract DIR   # one file per module
Each module starts with the OS-9 sync word 0x4AFC; size at +4, name offset at +0x0C,
type/language at +0x12 (0x0c01 system, 0x0d01 file manager, 0x0e01 driver, 0x0f00 descriptor,
0x0101 program, 0x0400 data)."""
import os
import struct
import sys


def modules(rom):
    i = 0
    while True:
        i = rom.find(b'\x4a\xfc', i)
        if i < 0:
            return
        if i % 2 == 0 and i + 0x30 <= len(rom):
            size = struct.unpack_from('>I', rom, i + 4)[0]
            name_off = struct.unpack_from('>I', rom, i + 0x0C)[0]
            if 0 < size <= len(rom) - i and 0 < name_off < size:
                name = rom[i + name_off:i + name_off + 32].split(b'\0')[0]
                if name and all(32 < c < 127 for c in name):
                    yield i, size, name.decode(), struct.unpack_from('>H', rom, i + 0x12)[0]
        i += 2


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    rom = open(sys.argv[1], 'rb').read()
    out = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == '--extract' else None
    if out:
        os.makedirs(out, exist_ok=True)
    for off, size, name, tl in modules(rom):
        print(f'{off:#08x} {size:#07x} {tl:04x} {name}')
        if out:
            open(os.path.join(out, name), 'wb').write(rom[off:off + size])


if __name__ == '__main__':
    main()
