"""List every OS-9/68k module in the extracted files (ext/) with header fields.
Usage: python3 tools/modinfo.py [dir]"""
import os, struct, sys

TYPES = {1: 'Prgrm', 2: 'Sbrtn', 3: 'Multi', 4: 'Data', 5: 'CSDData', 0xB: 'TrapLib',
         0xC: 'Systm', 0xD: 'FlMgr', 0xE: 'Drivr', 0xF: 'Devic'}
LANGS = {0: 'any', 1: 'Objct', 2: 'ICode', 3: 'PCode', 4: 'CCode', 5: 'CblCode', 6: 'FrtnCode'}

def u16(b, o): return struct.unpack_from('>H', b, o)[0]
def u32(b, o): return struct.unpack_from('>I', b, o)[0]
def cstr(b, o):
    e = b.index(b'\0', o)
    return b[o:e].decode('latin1')

def parse(b, off):
    m = b[off:]
    size = u32(m, 4)
    t, lang = m[0x12], m[0x13]
    h = dict(offset=off, size=size, sysrev=u16(m, 2), owner=u32(m, 8),
             name=cstr(m, u32(m, 0x0C)), accs=u16(m, 0x10),
             type=TYPES.get(t, hex(t)), lang=LANGS.get(lang, hex(lang)),
             attr=m[0x14], revs=m[0x15], edit=u16(m, 0x16),
             usage=u32(m, 0x18), symbol=u32(m, 0x1C), parity=u16(m, 0x2E))
    hdr_ok = 0
    for i in range(0, 0x30, 2): hdr_ok ^= u16(m, i)
    h['hdr_parity_ok'] = (hdr_ok == 0xFFFF)
    if t in (1, 0xB, 3, 2):  # executable types
        h.update(exec=u32(m, 0x30), excpt=u32(m, 0x34), mem=u32(m, 0x38),
                 stack=u32(m, 0x3C), idata=u32(m, 0x40), irefs=u32(m, 0x44))
        if t == 0xB:
            h.update(init=u32(m, 0x48), term=u32(m, 0x4C))
    return h

def scan(path):
    b = open(path, 'rb').read()
    off, out = 0, []
    while off + 0x30 <= len(b):
        if u16(b, off) == 0x4AFC:
            try:
                h = parse(b, off)
                if 0x30 <= h['size'] <= len(b) - off:
                    out.append(h); off += (h['size'] + 1) & ~1; continue
            except (ValueError, struct.error, IndexError):
                pass
        off += 2
    return out

if __name__ == '__main__':
    d = sys.argv[1] if len(sys.argv) > 1 else 'ext'
    for f in sorted(os.listdir(d)):
        for h in scan(os.path.join(d, f)):
            print(f'{f}:')
            for k, v in h.items():
                print(f'    {k:14} {hex(v) if isinstance(v, int) and not isinstance(v, bool) else v}')
