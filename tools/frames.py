"""Measure runner / MAME frame dumps without extra Python packages (plain zlib PNG reader).

    python3 tools/frames.py rows FILE.png [Y0 Y1]       mean brightness per row (find edges)
    python3 tools/frames.py first-diff DIR_A DIR_B      first file name whose pixels differ
    python3 tools/frames.py sprite DIR F0 F1 Y0 Y1      per-frame x extent of whatever moves in
                                                        rows Y0-Y1 versus frame F0 (walk speed)
    python3 tools/frames.py top FILE.png                first row with content (black borders)
Runner dumps (--shots DIR:N) are 384x280 RGB; MAME snapshots are 768x603 with 43 extra rows
on top, so runner row y matches MAME row 2*y + 43.
Contact sheets are easiest with ffmpeg:
    ffmpeg -pattern_type glob -i 'DIR/f0*.png' -vf "scale=192:140,tile=8x6" -frames:v 1 sheet.png"""
import glob
import os
import struct
import sys
import zlib


def rows(path):
    """Decode an 8-bit RGB, non-interlaced PNG into a list of row byte strings."""
    d = open(path, 'rb').read()
    i, idat, w, h = 8, b'', 0, 0
    while i < len(d):
        n, t = struct.unpack('>I4s', d[i:i + 8])
        c = d[i + 8:i + 8 + n]
        if t == b'IHDR':
            w, h = struct.unpack('>II', c[:8])
        elif t == b'IDAT':
            idat += c
        i += 12 + n
    raw, st = zlib.decompress(idat), w * 3
    prev, out = bytearray(st), []
    for y in range(h):
        f = raw[y * (st + 1)]
        r = bytearray(raw[y * (st + 1) + 1:(y + 1) * (st + 1)])
        for x in range(st):
            a = r[x - 3] if x >= 3 else 0
            b = prev[x]
            c = prev[x - 3] if x >= 3 else 0
            if f == 1:
                r[x] = (r[x] + a) & 255
            elif f == 2:
                r[x] = (r[x] + b) & 255
            elif f == 3:
                r[x] = (r[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                r[x] = (r[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out.append(bytes(r))
        prev = r
    return out


def frame_no(path):
    return int(os.path.basename(path)[1:6])


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == 'rows':
        rs = rows(args[0])
        y0, y1 = (int(args[1]), int(args[2])) if len(args) > 2 else (0, len(rs))
        for y in range(y0, y1):
            print(y, sum(rs[y]) // len(rs[y]))
    elif cmd == 'top':
        print(next((y for y, r in enumerate(rows(args[0])) if max(r) > 24), None))
    elif cmd == 'first-diff':
        for f in sorted(os.listdir(args[0])):
            g = os.path.join(args[1], f)
            if os.path.exists(g) and open(os.path.join(args[0], f), 'rb').read() != open(g, 'rb').read():
                print(f)
                return
        print('no difference')
    elif cmd == 'sprite':
        d, f0, f1, y0, y1 = args[0], int(args[1]), int(args[2]), int(args[3]), int(args[4])
        fs = [f for f in sorted(glob.glob(d + '/f*.png')) if f0 <= frame_no(f) <= f1]
        base, prev = rows(fs[0]), None
        for f in fs:
            rs, xs = rows(f), []
            for y in range(y0, y1):
                r, b = rs[y], base[y]
                xs += [x for x in range(len(r) // 3)
                       if abs(r[3 * x] - b[3 * x]) + abs(r[3 * x + 1] - b[3 * x + 1]) + abs(r[3 * x + 2] - b[3 * x + 2]) > 60]
            v = (min(xs), max(xs)) if xs else None
            if v != prev:
                print(frame_no(f), v)
            prev = v
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
