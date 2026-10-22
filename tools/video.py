"""Offline CD-i image decoders (validated against disc data before the C port).
   python3 tools/video.py slides <lbn> <sectors> <chan> [width]   -> out/img/*.png
Decoders: DYUV, CLUT8/CLUT7 (with a palette), RL7, CLUT4. Follows the MCD212 rules:
normal resolution = one byte per pixel; DYUV = pairs of bytes (U|Y0, V|Y1)."""
import struct, sys, zlib, os

DELTAS = [0, 1, 4, 9, 16, 27, 44, 79, 128, 177, 212, 229, 240, 247, 252, 255]

def clamp(x): return 0 if x < 0 else 255 if x > 255 else x

def yuv_rgb(y, u, v):
    # MCD212 equations (integer, truncating), then studio -> full range
    r = clamp(y + ((351 * (v - 128)) >> 8))
    g = clamp(y + ((-86 * (u - 128) - 179 * (v - 128)) >> 8))
    b = clamp(y + ((444 * (u - 128)) >> 8))
    return tuple(clamp(((c & 0xfe) - 16) * 298 + 128 >> 8) for c in (r, g, b))

def dyuv_line(data, off, width, start=(16, 128, 128)):
    """One DYUV line of `width` pixels from data[off:]; returns (pixels, bytes used)."""
    y, u, v = start
    out = []
    pairs = width // 2
    b0, b1 = data[off], data[off + 1]
    u = (u + DELTAS[b0 >> 4]) & 255; v = (v + DELTAS[b1 >> 4]) & 255
    p = off + 2
    for i in range(pairs):
        y2 = (y + DELTAS[b0 & 15]) & 255
        y = (y2 + DELTAS[b1 & 15]) & 255
        un, vn = u, v
        if i + 1 < pairs:
            b0, b1 = data[p], data[p + 1]; p += 2
            un = (u + DELTAS[b0 >> 4]) & 255; vn = (v + DELTAS[b1 >> 4]) & 255
        out.append(yuv_rgb(y2, u, v)); out.append(yuv_rgb(y, (u + un) >> 1, (v + vn) >> 1))
        u, v = un, vn
    return out, pairs * 2

def rl7_line(data, off, width, pal):
    """Run-length CLUT7: byte with bit7 clear = 1 pixel; bit7 set = colour, next byte = run (0 = rest of line)."""
    out = []
    p = off
    while len(out) < width:
        b = data[p]; p += 1
        n = 1
        if b & 0x80:
            n = data[p]; p += 1
            if n == 0: n = width - len(out)
        out.extend([pal[b & 0x7f]] * n)
    return out[:width], p - off

def write_png(path, w, h, rows):
    raw = b''.join(b'\x00' + bytes(c for px in row for c in px) for row in rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def video_stream(binpath, lbn, nsec, chan):
    B = open(binpath, 'rb')
    out = bytearray()
    for k in range(nsec):
        B.seek((lbn + k) * 2352); r = B.read(2352)
        fn, ch, sm, ci = r[16:20]
        if sm & 2 and ch == chan:
            out += r[24:24 + (2324 if sm & 0x20 else 2048)]
    return bytes(out)

if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'slides':
        lbn, nsec, chan = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
        width = int(sys.argv[5]) if len(sys.argv) > 5 else 384
        data = video_stream(os.environ.get('LL_BIN', 'data/ll.bin'), lbn, nsec, chan)
        rows, p = [], 0
        while p + width <= len(data):
            px, used = dyuv_line(data, p, width); rows.append(px); p += used
        os.makedirs('out/img', exist_ok=True)
        write_png('out/img/stream.png', width, len(rows), rows)
        print(f'{len(data)} bytes -> {len(rows)} lines of {width}')
