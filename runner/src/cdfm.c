/* CD file manager: real-time play (SS_Play) of .rtf files.
 * Sectors are read at the CD-i rate of 75 per second from the play position. Each sector whose
 * channel is selected in the PCB channel mask goes to the PCL chain for that channel (video,
 * audio or data, chosen by the submode), and the PCB / PCL status words and signals report
 * progress the way the game's handlers expect (see NOTES.md, "Real-time play").
 *
 * Play control block (PCB):
 *   +00 w status   +02 w signal   +04 l records left   +08 l channel mask   +0C w audio mask
 *   +0E l -> video PCL table   +12 l -> audio PCL table   +16 l -> data PCL table
 *   each table: 32 longs, PCL pointer per channel
 * Play control list (PCL):
 *   +00 b control  +01 b submode  +02 b coding  +04 w signal  +06 l next PCL
 *   +0A l buffer   +0E l buffer size (sectors)  +12 l error list  +18 l bytes delivered
 *   control byte: 0 while filling, positive when done, negative (bit 7) on error */
#include "ll.h"
#include <string.h>
#include <stdlib.h>

void os9_signal_pid(int pid, int sig);
void cdfm_abort(int pid);

/* PCB status bits, from what the game's handlers test (verify against the Green Book):
 * cdi_bumperanim: 0x01 with records == 0 = finished, 0x10 = trigger, 0x8000 = failed (retry);
 * cdi_space: 0x80 / 0x100 = finished, 0x8000 = error, 0x10 = trigger;
 * cdi_tribes: 0x100 clears its "play busy" flag, tracks 0x01 / 0x10, 0x8000 = error. */
#define ST_EOR     0x0001   /* end of record reached */
#define ST_TRIGGER 0x0010   /* sector with the trigger bit */
#define ST_DONE    0x0100   /* play stopped (records played, aborted, or end of file) */
#define ST_EOF     0x0080   /* end of file reached */

typedef struct {
    bool     active;
    int      pid;
    uint32_t pcb;
    uint32_t lbn, end;      /* next sector, one past the last */
    uint32_t cur[3][32];    /* current PCL per kind and channel (0 = take from the table) */
    uint32_t acc;           /* sector-rate accumulator */
    Adpcm    adpcm;         /* decoder state for real-time audio */
    uint32_t audio_sectors, last_masks, skipped;
    uint32_t fill[3][32];   /* sectors written into the current PCL */
    uint64_t started;       /* emulated cycles at SS_Play */
} Play;

static Play plays[4];

static void signal_pcb(Play *p, uint16_t bits)
{
    wr16(p->pcb, rd16(p->pcb) | bits);
    int sig = rd16(p->pcb + 2);
    if (sig) os9_signal_pid(p->pid, sig);
}

static void stop(Play *p, uint16_t bits)
{
    ll_log("        cdfm: play stops at LBN %u, status |= %04x, %u audio sectors played, %u PCLs skipped by the game\n",
           p->lbn, bits, p->audio_sectors, p->skipped);
    p->active = false;
    signal_pcb(p, bits);
}

int cdfm_play(int pid, uint32_t pcb, uint32_t lbn, uint32_t nsec)
{
    for (int i = 0; i < 4; i++)   /* one play per process: a new play silently replaces the old */
        if (plays[i].active && plays[i].pid == pid) plays[i].active = false;
    Play *p = NULL;
    for (int i = 0; i < 4; i++) if (!plays[i].active) { p = &plays[i]; break; }
    if (!p) return E_MEMFUL;
    memset(p, 0, sizeof *p);
    p->active = true; p->pid = pid; p->pcb = pcb; p->lbn = lbn; p->end = lbn + nsec;
    p->started = os9_cycles();
    p->last_masks = rd32(pcb + 8) ^ (uint32_t)rd16(pcb + 0x0C) << 8;
    wr16(pcb, 0);
    ll_log("        cdfm: play LBN %u..%u chan %08x achan %04x records %d signal %d\n", lbn, lbn + nsec,
           rd32(pcb + 8), rd16(pcb + 0xC), (int)rd32(pcb + 4), rd16(pcb + 2));
    return 0;
}

void cdfm_abort(int pid)
{
    for (int i = 0; i < 4; i++)
        if (plays[i].active && plays[i].pid == pid) {
            ll_log("        cdfm: abort play of process %d\n", pid);
            stop(&plays[i], ST_DONE);   /* the stopped play still reports (cdi_space waits for it) */
        }
}

/* Host "skip": end every play as if its last record had been reached. Used for cutscenes the
 * game offers no way to skip (see input.c). */
static void complete(Play *p, int kind, int ch, uint32_t table, uint32_t pcl);

/* Plays younger than this are never skipped: the title starts with a fade-in during which
 * cdi_space doesn't read the pointer, and ending its play then left the game waiting forever. */
#define SKIP_MIN_SECONDS 3

static bool skippable(const Play *p) { return p->active && os9_cycles() - p->started >= SKIP_MIN_SECONDS * ll_cpu_hz; }

bool cdfm_skippable(void) { for (int i = 0; i < 4; i++) if (skippable(&plays[i])) return true; return false; }

void cdfm_skip(void)
{
    for (int i = 0; i < 4; i++)
        if (skippable(&plays[i])) {
            Play *p = &plays[i];
            ll_log("        cdfm: play of process %d skipped by the user\n", p->pid);
            for (int k = 0; k < 3; k++)        /* hand over half-filled PCLs, as at a record end */
                for (int ch = 0; ch < 32; ch++)
                    if (p->cur[k][ch]) complete(p, k, ch, rd32(p->pcb + 0x0E + 4 * k), p->cur[k][ch]);
            wr32(p->pcb + 4, 0);
            stop(&plays[i], ST_EOR | ST_DONE);
        }
}

void cdfm_reset(void) { for (int i = 0; i < 4; i++) plays[i].active = false; }

bool cdfm_busy(void) { for (int i = 0; i < 4; i++) if (plays[i].active) return true; return false; }

/* A PCL is done when its buffer is full or a record ends in it: mark it, signal, and move the
 * channel's table entry on to PCL_Nxt. */
static void complete(Play *p, int kind, int ch, uint32_t table, uint32_t pcl)
{
    wr8(pcl, (rd8(pcl) & 0x7F) | 0x01);  /* done; bit 7 would mean error (cdi_space aborts on it) */
    ll_log("        cdfm: PCL %06x done (%s ch %d, %u of %u sectors)\n", pcl,
           kind == 0 ? "video" : kind == 1 ? "audio" : "data", ch, p->fill[kind][ch], rd32(pcl + 0x0E));
    int sig = rd16(pcl + 4);
    if (sig) os9_signal_pid(p->pid, sig);
    wr32(table + 4 * ch, rd32(pcl + 6));
    p->cur[kind][ch] = 0;
}

/* Deliver one sector into a PCL chain. */
static void deliver(Play *p, int kind, int ch, const uint8_t *data, int len, const uint8_t sub[4])
{
    uint32_t table = rd32(p->pcb + 0x0E + 4 * kind);
    if (!table) return;
    /* The table entry is the PCL being filled; when it completes the entry moves on to
     * PCL_Nxt. Games poll the entry (cdi_space re-reads it before every frame). */
    uint32_t pcl = rd32(table + 4 * ch);
    if (!pcl) return;
    if (pcl != p->cur[kind][ch]) {          /* new PCL (ours or installed by the game) */
        p->cur[kind][ch] = pcl;
        p->fill[kind][ch] = 0;
        if (rd8(pcl) != 0) p->skipped++;   /* the game skipped this one: it takes the newest */
        wr8(pcl, 0);
        wr32(pcl + 0x18, 0);
    }
    /* Audio sectors go to memory as their 18 sound groups (2304 bytes), without the 20 spare
     * bytes: the games index sound groups linearly across sectors (sound maps play e.g. groups
     * 882-917 of a 56-sector buffer). With the spare bytes every group after the first sector was
     * shifted and the effects decoded as noise. */
    if (kind == 1 && len > 2304) len = 2304;
    uint32_t cnt = p->fill[kind][ch], size = rd32(pcl + 0x0E), buf = rd32(pcl + 0x0A);
    if (buf) for (int i = 0; i < len; i++) wr8(buf + cnt * (uint32_t)len + i, data[i]);
    wr8(pcl + 1, sub[2]);
    wr8(pcl + 2, sub[3]);
    /* PCL_Cnt counts bytes: cdi_tribes accepts its 3-sector font (all/atnc24cl.ai1, played from
     * luxor.rtf) only if more than 0x17FF bytes arrived; with a sector count the font was never set
     * up and the first line of conversation text took practically forever to draw. */
    wr32(pcl + 0x18, rd32(pcl + 0x18) + (uint32_t)len);
    p->fill[kind][ch] = ++cnt;
    if (cnt >= size || (sub[2] & 0x01)) complete(p, kind, ch, table, pcl);
}

static void step(Play *p)
{
    uint32_t masks = rd32(p->pcb + 8) ^ (uint32_t)rd16(p->pcb + 0x0C) << 8;
    if (masks != p->last_masks) {
        ll_log("        cdfm: LBN %u: game changed masks to chan %08x achan %04x\n", p->lbn, rd32(p->pcb + 8), rd16(p->pcb + 0x0C));
        p->last_masks = masks;
    }
    uint8_t data[2324], sub[4];
    int len = disc_read_sector(p->lbn++, data, sub);
    int ch = sub[1] & 31, sm = sub[2];
    uint32_t mask = rd32(p->pcb + 8);
    if (len && (mask >> ch) & 1) {
        if (sm & 0x02) deliver(p, 0, ch, data, len, sub);
        else if (sm & 0x04) {
            deliver(p, 1, ch, data, len, sub);                       /* to audio memory, if a PCL wants it */
            if (ch < 16 && (rd16(p->pcb + 0x0C) >> ch) & 1) {         /* selected for playback */
                int16_t pcm[4032 * 2];
                int rate, n = adpcm_decode_sector(&p->adpcm, data, sub[3], pcm, &rate);
                if (!p->audio_sectors) ll_log("        cdfm: audio starts, channel %d coding %02x (%d Hz)\n", ch, sub[3], rate);
                p->audio_sectors++;
                audio_output_pcm(pcm, n, rate);
            }
        }
        else if (sm & 0x08) deliver(p, 2, ch, data, len, sub);
        if (sm & 0x10) signal_pcb(p, ST_TRIGGER);
        if (sm & 0x01) {
            uint32_t rec = rd32(p->pcb + 4);
            if (rec) wr32(p->pcb + 4, --rec);
            if (!rec) { stop(p, ST_EOR | ST_DONE); return; }
            signal_pcb(p, ST_EOR);
        }
    }
    if ((sm & 0x80) || p->lbn >= p->end) stop(p, ST_EOR | ST_DONE | ST_EOF);
}

/* Called every kernel tick (TICKS_PER_SEC per second). */
void cdfm_tick(int ticks_per_sec)
{
    for (int i = 0; i < 4; i++) {
        Play *p = &plays[i];
        if (!p->active) continue;
        p->acc += 75;
        while (p->active && p->acc >= (uint32_t)ticks_per_sec) {
            p->acc -= ticks_per_sec;
            step(p);
        }
    }
}
