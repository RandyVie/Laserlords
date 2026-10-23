"""RMS level of a WAV file per window, to compare the runner's audio with MAME's.

    python3 tools/audio_rms.py FILE.wav T0 T1 [WINDOW_SECONDS]
Runner: --wav out.wav (37.8 kHz). MAME: -wavwrite out.wav (48 kHz) with MAME_SOUND=1 in
tools/mame/run.sh. MAME's mixer is quieter overall (about 4x for Laser Lords' music), so compare
ratios (effects vs music) rather than absolute levels."""
import math
import struct
import sys
import wave


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    w = wave.open(sys.argv[1])
    t0, t1 = float(sys.argv[2]), float(sys.argv[3])
    win_s = float(sys.argv[4]) if len(sys.argv) > 4 else 0.5
    sr, ch = w.getframerate(), w.getnchannels()
    w.setpos(int(t0 * sr))
    data = w.readframes(int((t1 - t0) * sr))
    a = struct.unpack('<%dh' % (len(data) // 2), data)[::ch]
    win = max(1, int(win_s * sr))
    levels = [int(math.sqrt(sum(x * x for x in a[i:i + win]) / win)) for i in range(0, len(a) - win + 1, win)]
    print(f'{sr} Hz, RMS per {win_s} s:', levels)
    print('overall RMS', int(math.sqrt(sum(x * x for x in a) / max(1, len(a)))))


if __name__ == '__main__':
    main()
