/* Sound maps: sound effects played from memory (SetStt/GetStt 0x3B on /audio).
 *   SetStt sub 0  create: D3 = audio coding (4 = mono 18.9 kHz 4-bit), D4 = size (0x12), A0 = buffer
 *                 -> D0 = map id
 *   GetStt sub 0  D3 = id -> A0 = the map's descriptor, which the game fills in:
 *                 +02 w first sound group  +04 w last sound group  +06 w repeat count
 *                 +0A l buffer  +0E l size (bytes)  +1A w sound groups  +20 l (per-effect pointer)
 *   SetStt sub 1  play: D3 = id, A0 = status word (the game sets it to 0; we write 1 when done)
 *   SetStt sub 2  stop (D3 = id)
 * Layout inferred from cdi_space (FUN_6128 and its set-up code). (verify sub 3/4, GetStt 1) */
#include "ll.h"
#include "m68k.h"
#include <stdlib.h>
#include <string.h>

#define REG(r)      m68k_get_reg(NULL, M68K_REG_##r)
#define SETREG(r,v) m68k_set_reg(M68K_REG_##r, (v))

typedef struct { bool used; uint8_t coding; uint32_t desc, status; } Map;
static Map maps[8];

void soundmap_reset(void) { memset(maps, 0, sizeof maps); }

static Map *map(uint32_t id) { int i = (int)(id & 0xFFFF) - 1; return i >= 0 && i < 8 && maps[i].used ? &maps[i] : NULL; }

static void effect_done(void *arg)
{
    Map *m = arg;
    if (m->status) { wr16(m->status, 1); m->status = 0; }
}

int soundmap_setstt(int sub)
{
    Map *m;
    switch (sub) {
    case 0:
        for (int i = 0; i < 8; i++)
            if (!maps[i].used) {
                maps[i] = (Map){ true, (uint8_t)REG(D3), heap_alloc(0x40), 0 };
                wr32(maps[i].desc + 0x0A, REG(A0));
                wr16(maps[i].desc + 0x1A, REG(D4) & 0xFFFF);
                SETREG(D0, i + 1);
                ll_log("        soundmap: create %d coding %02x buffer %06x -> descriptor %06x\n", i + 1, maps[i].coding, REG(A0), maps[i].desc);
                return 0;
            }
        return E_MEMFUL;
    case 1: {
        if (!(m = map(REG(D3)))) return E_BPNUM;
        uint32_t d = m->desc;
        int first = rd16(d + 2), last = rd16(d + 4), reps = rd16(d + 6);
        uint32_t base = rd32(d + 0x0A);
        int n = last - first + 1;
        if (n <= 0 || n > 4096 || !base) {      /* nothing to play: report it finished at once */
            ll_log("        soundmap: bad play %d..%d base %06x\n", first, last, base);
            if (REG(A0)) wr16(REG(A0), 1);
            return 0;
        }
        if (reps < 1) reps = 1;
        int16_t *pcm = malloc(sizeof(int16_t) * 2 * 224 * n * reps);
        Adpcm st; adpcm_reset(&st);
        int frames = 0, rate = 18900;
        for (int r = 0; r < reps; r++)
            frames += adpcm_decode_groups(&st, host(base + (uint32_t)first * 128), n, m->coding, pcm + 2 * frames, &rate);
        audio_stop_effect();               /* the effect it replaces reports done to its own status word */
        m->status = REG(A0);
        if (m->status) wr16(m->status, 0);
        ll_log("        soundmap: play %d groups %d..%d x%d (%d frames at %d Hz)\n", (int)(m - maps) + 1, first, last, reps, frames, rate);
        audio_play_effect(pcm, frames, rate, effect_done, m);
        free(pcm);
        return 0;
    }
    case 2:
        ll_log("        soundmap: stop\n");
        audio_stop_effect();
        return 0;
    }
    ll_log("        soundmap: SetStt sub %d (d3=%x a0=%06x) not implemented\n", sub, REG(D3), REG(A0));
    return 0;
}

int soundmap_getstt(int sub)
{
    Map *m;
    if (sub == 0) {
        if (!(m = map(REG(D3)))) return E_BPNUM;
        SETREG(A0, m->desc);
        return 0;
    }
    ll_log("        soundmap: GetStt sub %d not implemented\n", sub);
    return 0;
}
