"""Convert a runner session recording (session.llrec) into timed moves for MAME's replay.lua.

    python3 tools/llrec_to_route.py session.llrec out.route [--cpu-hz 15000000]
Each output line is "t dir button duration" (runner seconds): dir is l/r/u/d combinations or
"-", from the pointer deflection (stick/key offset + mouse offset) beyond the game's
joystick threshold (19 units, cdi_tribes FUN_1016c)."""
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    hz = float(sys.argv[sys.argv.index('--cpu-hz') + 1]) if '--cpu-hz' in sys.argv else 15e6
    lines = open(sys.argv[1]).read().split('\n')
    if not lines[0].startswith('LLREC'):
        sys.exit('not a session recording')
    segs, prev = [], None
    for line in lines[1:]:
        p = line.split()
        if len(p) != 8:
            continue
        v = [float.fromhex(x) for x in p[1:7]]
        dx, dy = v[2] + v[4], v[3] + v[5]
        d = ('r' if dx > 19 else 'l' if dx < -19 else '') + ('d' if dy > 19 else 'u' if dy < -19 else '')
        state = (d or '-', int(p[7]))
        if state != prev:
            segs.append((int(p[0]) / hz,) + state)
            prev = state
    with open(sys.argv[2], 'w') as f:
        for i, (t, d, b) in enumerate(segs):
            t1 = segs[i + 1][0] if i + 1 < len(segs) else t + 0.3
            f.write(f'{t:.3f} {d} {b} {t1 - t:.3f}\n')
    print(f'{len(segs)} segments, last at {segs[-1][0]:.1f} s' if segs else 'no input')


if __name__ == '__main__':
    main()
