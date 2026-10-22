"""Generate the app icon (original artwork, no game assets): a laser beam across a planet
in deep space. Writes packaging/icon_1024.png. Usage: python3 tools/make_icon.py"""
import numpy as np, zlib, struct, os
N = 1024
y, x = np.mgrid[0:N, 0:N].astype(np.float32) / N
img = np.zeros((N, N, 3), np.float32)
# rounded-square background: deep blue to black
img[...] = np.stack([0.02 + 0.06 * (1 - y), 0.03 + 0.08 * (1 - y), 0.10 + 0.25 * (1 - y)], -1)
rng = np.random.default_rng(7)
for _ in range(260):                              # stars
    sx, sy, r = rng.random(), rng.random(), rng.random() * 0.004 + 0.001
    d = (x - sx) ** 2 + (y - sy) ** 2
    img += (np.exp(-d / (r * r)) * (0.6 + 0.4 * rng.random()))[..., None]
# golden planet, lower right
cx, cy, R = 0.68, 0.70, 0.30
d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
inside = d < R
shade = np.clip(1.15 - ((x - cx + 0.12) ** 2 + (y - cy + 0.12) ** 2) / (R * R) * 0.9, 0.15, 1)
noise = 0.85 + 0.15 * np.sin(x * 60) * np.sin(y * 47 + x * 13)
planet = np.stack([0.95, 0.70, 0.25]) * (shade * noise)[..., None]
img[inside] = planet[inside]
# red laser beam from upper left, with glow
ax, ay, bx, by = 0.08, 0.18, 0.92, 0.46
t = np.clip(((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / ((bx - ax) ** 2 + (by - ay) ** 2), 0, 1)
dist = np.sqrt((x - (ax + t * (bx - ax))) ** 2 + (y - (ay + t * (by - ay))) ** 2)
core = np.exp(-(dist / 0.008) ** 2)
glow = np.exp(-(dist / 0.04) ** 2) * 0.7
img += np.stack([1.0, 0.15, 0.10]) * glow[..., None] + core[..., None]
# rounded-square mask (macOS style)
m = 0.08
qx, qy = np.maximum(np.abs(x - 0.5) - (0.5 - m - 0.18), 0), np.maximum(np.abs(y - 0.5) - (0.5 - m - 0.18), 0)
mask = (np.sqrt(qx ** 2 + qy ** 2) < 0.18).astype(np.float32)
rgba = np.concatenate([np.clip(img, 0, 1), mask[..., None]], -1)
data = (rgba * 255).astype(np.uint8)
raw = b''.join(b'\0' + data[i].tobytes() for i in range(N))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
os.makedirs('packaging', exist_ok=True)
open('packaging/icon_1024.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', N, N, 8, 6, 0, 0, 0))
                                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
print('packaging/icon_1024.png')
