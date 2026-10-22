/* CD-i audio: ADPCM decoding (levels A, B, C; same format as CD-ROM XA) and the output mix.
 * Real-time sectors arrive from the CD file manager; decoded PCM goes into one output buffer at
 * 37,800 Hz stereo, which the front end drains to SDL (or a WAV file when headless).
 *
 * Sector layout (Form 2, 2324 bytes): 18 sound groups of 128 bytes. Each group: 16 header
 * bytes (sound parameters, duplicated) and 112 data bytes. 4-bit: 8 sound units of 28 samples;
 * 8-bit: 4 units. Unit parameter byte: low nibble = shift (range), bits 4-5 = filter. */
#include "ll.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const int K0[4] = { 0, 60, 115, 98 }, K1[4] = { 0, 0, -52, -55 };

void adpcm_reset(Adpcm *d) { memset(d, 0, sizeof *d); }

static int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* Decode `ngroups` 128-byte sound groups (from guest memory or a host buffer). Returns frames
 * written to out (interleaved L,R) at *rate. Mono is duplicated to both channels. */
int adpcm_decode_groups(Adpcm *d, const uint8_t *s, int ngroups, uint8_t coding, int16_t *out, int *rate)
{
    bool stereo = coding & 1, bits8 = (coding >> 4) & 1;
    *rate = (coding >> 2) & 1 ? 18900 : 37800;
    int nunits = bits8 ? 4 : 8;
    int nl = 0, nr = 0, nm = 0;
    for (int g = 0; g < ngroups; g++) {
        const uint8_t *grp = s + g * 128, *hdr = grp, *data = grp + 16;
        for (int u = 0; u < nunits; u++) {
            int p = hdr[4 + u], sh = p & 15, f = (p >> 4) & 3;
            int c = stereo ? (u & 1) : 0;
            for (int i = 0; i < 28; i++) {
                int v;
                if (bits8) {
                    int8_t b = (int8_t)data[i * 4 + u];
                    v = (b * 256) >> sh;
                } else {
                    int b = data[i * 4 + (u >> 1)];
                    int nib = (u & 1) ? b >> 4 : b & 15;
                    if (nib > 7) nib -= 16;
                    v = (nib * 4096) >> sh;
                }
                v += (d->p1[c] * K0[f] + d->p2[c] * K1[f] + 32) >> 6;
                int16_t sv = clamp16(v);
                d->p2[c] = d->p1[c];
                d->p1[c] = sv;
                if (!stereo) { out[2 * nm] = out[2 * nm + 1] = sv; nm++; }
                else if (c == 0) out[2 * nl++] = sv;
                else out[2 * nr++ + 1] = sv;
            }
        }
    }
    return stereo ? (nl < nr ? nl : nr) : nm;
}

/* One audio sector = 18 sound groups. */
int adpcm_decode_sector(Adpcm *d, const uint8_t *s, uint8_t coding, int16_t *out, int *rate)
{
    return adpcm_decode_groups(d, s, 18, coding, out, rate);
}

/* ------------------------------------------------------------------ */
/* Output                                                              */

/* Attenuation (SetStt 0x3d on /audio): D3 = LL.LR.RR.RL bytes, 0 = full level, 0x80+ = off,
 * in 0.5 dB steps (verify step size). Default 0x00FF00FF = plain stereo. */
static float gain_ll = 1, gain_lr = 0, gain_rr = 1, gain_rl = 0;

static float att(int v) { return v >= 0x80 ? 0.f : powf(10.f, -0.5f * v / 20.f); }

void audio_set_attenuation(uint32_t d3)
{
    gain_ll = att(d3 >> 24); gain_lr = att((d3 >> 16) & 255);
    gain_rr = att((d3 >> 8) & 255); gain_rl = att(d3 & 255);
}

/* Output FIFO at 37.8 kHz stereo, drained by the front end. */
#define FIFO_FRAMES (37800 * 2)
static int16_t fifo[FIFO_FRAMES][2];
static int fifo_r, fifo_w;
static long long frames_out;

static void push(int16_t l, int16_t r)
{
    int next = (fifo_w + 1) % FIFO_FRAMES;
    if (next == fifo_r) fifo_r = (fifo_r + 1) % FIFO_FRAMES;   /* overflow: drop the oldest */
    fifo[fifo_w][0] = l; fifo[fifo_w][1] = r;
    fifo_w = next;
}

/* Sound-map effect being played from memory (mixed over the CD audio). */
static struct { int16_t *pcm; int frames, pos, rep; void (*done)(void *); void *arg; } fx;

void audio_play_effect(const int16_t *pcm, int frames, int rate, void (*done)(void *), void *arg)
{
    if (fx.done) fx.done(fx.arg);          /* a new effect replaces the old one */
    free(fx.pcm);
    fx.pcm = malloc(sizeof(int16_t) * 2 * (frames ? frames : 1));
    memcpy(fx.pcm, pcm, sizeof(int16_t) * 2 * frames);
    fx.frames = frames; fx.pos = 0; fx.rep = rate == 18900 ? 2 : 1;
    fx.done = done; fx.arg = arg;
}

void audio_stop_effect(void)
{
    void (*done)(void *) = fx.done;
    void *arg = fx.arg;
    free(fx.pcm);
    memset(&fx, 0, sizeof fx);
    if (done) done(arg);
}

/* Next effect sample (at 37.8 kHz), or silence. */
static void fx_next(int *l, int *r)
{
    *l = *r = 0;
    if (!fx.pcm) return;
    int i = fx.pos / fx.rep;
    *l = fx.pcm[2 * i]; *r = fx.pcm[2 * i + 1];
    if (++fx.pos >= fx.frames * fx.rep) audio_stop_effect();
}

static void emit(float l, float r)
{
    int el, er;
    fx_next(&el, &er);
    l += el; r += er;
    push(clamp16((int)(l * gain_ll + r * gain_rl)), clamp16((int)(r * gain_rr + l * gain_lr)));
    frames_out++;
}

void audio_output_pcm(const int16_t *pcm, int frames, int rate)
{
    int rep = rate == 18900 ? 2 : 1;
    for (int i = 0; i < frames; i++)
        for (int k = 0; k < rep; k++) emit(pcm[2 * i], pcm[2 * i + 1]);
}

/* Keep the output timeline up to `target` frames (emulated time): silence plus any effect. */
void audio_tick(long long target)
{
    if (target - frames_out > 37800) frames_out = target - 37800;   /* never build up more than 1 s */
    while (frames_out < target) emit(0, 0);
}

int audio_drain(int16_t *out, int max_frames)
{
    int n = 0;
    while (n < max_frames && fifo_r != fifo_w) {
        out[2 * n] = fifo[fifo_r][0]; out[2 * n + 1] = fifo[fifo_r][1];
        fifo_r = (fifo_r + 1) % FIFO_FRAMES;
        n++;
    }
    return n;
}

void audio_reset(void)
{
    free(fx.pcm);
    memset(&fx, 0, sizeof fx);
    fifo_r = fifo_w = 0;
    frames_out = 0;
    audio_set_attenuation(0x00FF00FF);
}

/* ---- WAV writer for headless runs ---- */
static FILE *wav;
static uint32_t wav_frames;

static void le32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void le16(FILE *f, uint16_t v) { fputc(v, f); fputc(v >> 8, f); }

bool audio_wav_open(const char *path)
{
    wav = fopen(path, "wb");
    if (!wav) return false;
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, wav);
    le32(wav, 16); le16(wav, 1); le16(wav, 2); le32(wav, 37800); le32(wav, 37800 * 4); le16(wav, 4); le16(wav, 16);
    fwrite("data\0\0\0\0", 1, 8, wav);
    return true;
}

void audio_wav_pump(void)
{
    if (!wav) return;
    int16_t tmp[4096 * 2];
    int n;
    while ((n = audio_drain(tmp, 4096)) > 0) {
        for (int i = 0; i < n * 2; i++) le16(wav, (uint16_t)tmp[i]);
        wav_frames += n;
    }
}

/* Keep real time in the WAV: pad silence up to `frames` total. */
void audio_wav_pad_to(long long frames)
{
    if (!wav) return;
    audio_wav_pump();
    while ((long long)wav_frames < frames) { le32(wav, 0); wav_frames++; }
}

void audio_wav_close(void)
{
    if (!wav) return;
    audio_wav_pump();
    fseek(wav, 4, SEEK_SET); le32(wav, 36 + wav_frames * 4);
    fseek(wav, 40, SEEK_SET); le32(wav, wav_frames * 4);
    fclose(wav);
    wav = NULL;
}
