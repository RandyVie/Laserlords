/* CD-i video: the UCM display-control driver (dc_ calls on the video path) and an MCD212
 * renderer. The game builds Field Control Tables (run once per field) and Line Control Tables
 * (8 instructions per display line) through the driver; each field we execute them and decode
 * the pixel data the VSR points at, per plane, then mix the two planes over the backdrop.
 *
 * Instruction and register behaviour follows the MCD212 (Philips VDSC) as documented in the
 * Green Book and implemented in MAME's mcd212.cpp, used here as the hardware reference.
 * Sub-code meanings of SetStt 0x56 are inferred from the game's calls (see NOTES.md). */
#include "ll.h"
#include "m68k.h"
#include <stdlib.h>
#include <string.h>

#define REG(r) m68k_get_reg(NULL, M68K_REG_##r)

/* ------------------------------------------------------------------ */
/* Driver tables                                                       */

#define MAXTAB  64
#define LCT_COLS 8

typedef struct {
    bool     used, lct;
    int      plane;
    int      count;          /* FCT: instructions; LCT: lines */
    uint32_t *ins;           /* FCT: count; LCT: count * LCT_COLS */
    int      link_tab, link_line;      /* FCT -> LCT start (dc_flnk) */
    int16_t  *jump_tab, *jump_line;    /* LCT: per-line redirect (dc_llnk), -1 = none */
} Table;

/* FCTs and LCTs are numbered separately from 0, in creation order: cdi_tribes never creates
 * tables and writes straight into "FCT 0" / "LCT 0", the launcher's plane A tables. */
static Table fcts[MAXTAB], lcts[MAXTAB];
static int exec_fct[2] = { -1, -1 };   /* active FCT per plane (dc_exec) */

static Table *fct(uint32_t id) { int i = id & 0xFFFF; return i < MAXTAB && fcts[i].used ? &fcts[i] : NULL; }
static Table *lct(uint32_t id) { int i = id & 0xFFFF; return i < MAXTAB && lcts[i].used ? &lcts[i] : NULL; }

static int tab_create(bool lct, int plane, int count)
{
    Table *arr = lct ? lcts : fcts;
    for (int i = 0; i < MAXTAB; i++) {
        if (arr[i].used) continue;
        Table *t = &arr[i];
        memset(t, 0, sizeof *t);
        t->used = true; t->lct = lct; t->plane = plane & 1; t->count = count;
        int n = lct ? count * LCT_COLS : count;
        t->ins = malloc(sizeof(uint32_t) * (n ? n : 1));
        for (int k = 0; k < n; k++) t->ins[k] = 0x10000000;   /* NOP */
        t->link_tab = -1;
        if (lct) {
            t->jump_tab = malloc(sizeof(int16_t) * count);
            t->jump_line = malloc(sizeof(int16_t) * count);
            for (int k = 0; k < count; k++) t->jump_tab[k] = t->jump_line[k] = -1;
        }
        return i;
    }
    return -1;
}

static void tab_delete(bool is_lct, uint32_t id)
{
    Table *t = is_lct ? lct(id) : fct(id);
    if (!t) return;
    free(t->ins); free(t->jump_tab); free(t->jump_line);
    memset(t, 0, sizeof *t);
    if (!is_lct) for (int p = 0; p < 2; p++) if (exec_fct[p] == (int)(id & 0xFFFF)) exec_fct[p] = -1;
}

/* ------------------------------------------------------------------ */
/* MCD212 state                                                        */

typedef struct {
    uint32_t clut[256];
    uint32_t icm, tcr, plane_order, backdrop;
    uint32_t clut_bank[2], transparent[2], mask[2], dyuv_start[2], mosaic[2];
    uint32_t matte[8];
    uint8_t  weight[2];
    uint32_t vsr[2];
    uint8_t  ddr[2];          /* display parameters (FT bits 8-9, MT bits 10-11 as value<<8) */
    bool     stopped[2];      /* DCA stopped for the rest of the field */
    uint32_t first_vsr[2];    /* first VSR loaded by a line table this field (crossfade fix) */
    bool     backjump[2];     /* current rows come from below first_vsr (crossfade fix) */
} Mcd;
static Mcd vd;

/* graphics cursor (gc_ calls, SetStt 0x52) */
static struct { bool on; int x, y; uint16_t pat[16]; uint32_t color; } cursor;

uint32_t ll_frame[VIDEO_H][VIDEO_W];  /* ARGB, 768 x 280 */
#define  ACTIVE_END 260
int      video_debug_field = -1;
bool     video_trace_fields;           /* debug: mark every field in the trace (LL_TRACE_FIELDS=1) */       /* debug: log per-line state for this field */
int      ll_frame_count;


static const uint8_t DELTAS[16] = { 0, 1, 4, 9, 16, 27, 44, 79, 128, 177, 212, 229, 240, 247, 252, 255 };
static const uint32_t COL4[16] = {
    0x101010, 0x10107a, 0x107a10, 0x107a7a, 0x7a1010, 0x7a107a, 0x7a7a10, 0x7a7a7a,
    0x101010, 0x1010e6, 0x10e610, 0x10e6e6, 0xe61010, 0xe610e6, 0xe6e610, 0xe6e6e6 };
static const uint32_t DISP4[16] = {
    0x000000, 0x00007b, 0x007b00, 0x007b7b, 0x7b0000, 0x7b007b, 0x7b7b00, 0x7b7b7b,
    0x000000, 0x0000f9, 0x00f900, 0x00f9f9, 0xf90000, 0xf900f9, 0xf9f900, 0xf9f9f9 };

static int clampi(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static uint32_t yuv_rgb(int y, int u, int v)
{
    int r = clampi(y + ((351 * (v - 128)) >> 8));
    int g = clampi(y + ((-86 * (u - 128) - 179 * (v - 128)) >> 8));
    int b = clampi(y + ((444 * (u - 128)) >> 8));
    return (uint32_t)(r & 0xfe) << 16 | (uint32_t)(g & 0xfe) << 8 | (uint32_t)(b & 0xfe);
}

static void set_register(int path, int reg, uint32_t v)
{
    if (reg >= 0x80 && reg <= 0xBF) { vd.clut[(vd.clut_bank[path] * 0x40 + (reg - 0x80)) & 255] = v & 0xfcfcfc; return; }
    switch (reg) {
    case 0xC0: if (!path) vd.icm = v; break;
    case 0xC1: if (!path) vd.tcr = v; break;
    case 0xC2: if (!path) vd.plane_order = v & 7; break;
    case 0xC3: vd.clut_bank[path] = path ? (2 | (v & 1)) : (v & 3); break;
    case 0xC4: if (!path) vd.transparent[0] = v & 0xfcfcfc; break;
    case 0xC6: if (path) vd.transparent[1] = v & 0xfcfcfc; break;
    case 0xC7: if (!path) vd.mask[0] = v & 0xfcfcfc; break;
    case 0xC9: if (path) vd.mask[1] = v & 0xfcfcfc; break;
    case 0xCA: if (!path) vd.dyuv_start[0] = v; break;
    case 0xCB: if (path) vd.dyuv_start[1] = v; break;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        vd.matte[reg & 7] = v; break;
    case 0xD8: if (!path) vd.backdrop = v & 15; break;
    case 0xD9: if (!path) vd.mosaic[0] = v; break;
    case 0xDA: if (path) vd.mosaic[1] = v; break;
    case 0xDB: if (!path) vd.weight[0] = v & 0x3f; break;
    case 0xDC: if (path) vd.weight[1] = v & 0x3f; break;
    }
}

static int line_event_requests;   /* INTERRUPT instructions seen this field */

/* Cosmetic workaround (setting "fix_crossfade", on by default; --accurate turns it off).
 * During warp crossfades cdi_space puts both planes in DYUV, but plane A's line table still
 * reloads the VSR to the CLUT8 cockpit panel (stored just before the DYUV image) for the
 * bottom rows, which decodes as colour noise while plane A fades in. With the fix, rows of a
 * DYUV plane whose VSR jumped back below the field's first VSR for that plane are treated as
 * transparent, so the other plane (the space image) shows there. Only while both planes are
 * DYUV (the crossfade mode), so ordinary scenes are untouched. See NOTES.md. */
bool video_fix_crossfade = true;
static bool in_lct;                /* executing line-table (not field-table) instructions */

static void note_vsr(int path, uint32_t v)
{
    if (!in_lct) return;
    if (!vd.first_vsr[path]) vd.first_vsr[path] = v;
    vd.backjump[path] = v < vd.first_vsr[path];
}

/* Execute one display-control instruction. Returns false on STOP. */
static bool exec_ins(int path, uint32_t c)
{
    int op = c >> 24;
    switch (op >> 4) {
    case 0x0: return false;                                   /* STOP */
    case 0x1: return true;                                    /* NOP */
    case 0x2: return true;                                    /* RELOAD DCP: links are the driver's job */
    case 0x3: return false;                                   /* RELOAD DCP and STOP */
    case 0x4: vd.vsr[path] = c & 0xFFFFFF; note_vsr(path, vd.vsr[path]); return true;    /* RELOAD VSR */
    case 0x5: vd.vsr[path] = c & 0xFFFFFF; note_vsr(path, vd.vsr[path]); return false;   /* RELOAD VSR and STOP */
    case 0x6: line_event_requests++; return true;             /* INTERRUPT */
    case 0x7:
        if (op & 8) { vd.ddr[path] = (uint8_t)(c & 0x1f); return true; }   /* display parameters */
        /* fallthrough */
    default:
        set_register(path, op, c & 0xFFFFFF);
        return true;
    }
}

/* Matte (region) control, MCD212: up to 8 registers (0xD0-0xD7), each "at x = X do OP".
 * OP 0 stops processing, 4/6 change the weight of plane A/B, 8/9 reset/set a matte flag,
 * 12-15 do both. With one matte (ICM bit 19 clear) all 8 registers drive flag MF (bit 16); with
 * two, registers 0-3 drive MF0 and 4-7 MF1. Flags and weights restart at x = 0 every line. */
static bool    matte_flag[2][VIDEO_W];
static uint8_t line_weight[2][VIDEO_W];

static void matte_line(void)
{
    int nm = (vd.icm >> 19) & 1 ? 2 : 1;
    bool mf[2] = { false, false };
    uint8_t wf[2] = { vd.weight[0], vd.weight[1] };
    int idx[2] = { 0, 4 };
    for (int x = 0; x < VIDEO_W; x++) {
        for (int m = 0; m < nm; m++) {
            int max = (nm == 2 ? 4 : 8) + (m ? 4 : 0);
            if (idx[m] >= max) continue;
            uint32_t c = vd.matte[idx[m]];
            if ((int)(c & 0x3FF) != x) continue;
            int op = (c >> 20) & 15, flag = nm == 2 ? m : (int)((c >> 16) & 1);
            switch (op) {
            case 0: idx[m] = 8; continue;
            case 4: case 6: wf[(op >> 1) & 1] = (c >> 10) & 0x3F; break;
            case 8: case 9: mf[flag] = op & 1; break;
            case 12: case 13: case 14: case 15: wf[(op >> 1) & 1] = (c >> 10) & 0x3F; mf[flag] = op & 1; break;
            }
            idx[m]++;
        }
        line_weight[0][x] = wf[0]; line_weight[1][x] = wf[1];
        matte_flag[0][x] = mf[0]; matte_flag[1][x] = mf[1];
    }
}

static int icm_of(int path) { return path ? (vd.icm >> 8) & 15 : vd.icm & 15; }

static uint8_t clut_index(int path, int icm, uint8_t b)
{
    switch (icm) {
    case 1: return b;                                         /* CLUT8 */
    case 3: return (uint8_t)((path ? 0x80 : 0) | (b & 0x7f)); /* CLUT7 */
    case 4: return path ? 0 : (uint8_t)(((vd.icm >> 22) & 1 ? 0x80 : 0) | (b & 0x7f));
    case 11: return (uint8_t)((path ? 0x80 : 0) | (b & 0x0f)); /* CLUT4 */
    }
    return 0;
}

/* Decode one 768-unit line for a plane; advances the VSR. */
static void decode_line(int path, uint32_t *px, bool *tr)
{
    int icm = icm_of(path);
    int tc = (vd.tcr >> (path ? 8 : 0)) & 15;
    int tct = tc & 7;
    bool parity = !(tc & 8);
    bool always = tct == 0 && parity;
    uint32_t vsr = vd.vsr[path];
    if (!icm || !vsr || vd.stopped[path] || vsr >= MEM_SIZE) {
        for (int x = 0; x < VIDEO_W; x++) { px[x] = COL4[0]; tr[x] = tct == 0; }
        return;
    }
    int ft = vd.ddr[path] & 3;            /* 0/1 bitmap, 2 run-length, 3 mosaic */
    uint32_t maskbits = ~vd.mask[path] & 0xfcfcfc, key = vd.transparent[path] & maskbits;
    bool dyuv = icm == 5, rgb = icm == 1 && path == 1;
    bool use_key = !dyuv && !rgb && (tct == 1 || tct == 5 || tct == 6);

    if (dyuv) {
        uint32_t s = vd.dyuv_start[path];
        int y = (s >> 16) & 255, u = (s >> 8) & 255, v = s & 255;
        int b0 = rd8(vsr++), b1 = rd8(vsr++);
        u = (u + DELTAS[b0 >> 4]) & 255; v = (v + DELTAS[b1 >> 4]) & 255;
        for (int i = 0; i < VIDEO_W / 4; i++) {
            int y2 = (y + DELTAS[b0 & 15]) & 255;
            y = (y2 + DELTAS[b1 & 15]) & 255;
            int un = u, vn = v;
            if (i + 1 < VIDEO_W / 4) {
                b0 = rd8(vsr++); b1 = rd8(vsr++);
                un = (u + DELTAS[b0 >> 4]) & 255; vn = (v + DELTAS[b1 >> 4]) & 255;
            }
            uint32_t c0 = yuv_rgb(y2, u, v), c1 = yuv_rgb(y, (u + un) >> 1, (v + vn) >> 1);
            px[i * 4] = px[i * 4 + 1] = c0;
            px[i * 4 + 2] = px[i * 4 + 3] = c1;
            u = un; v = vn;
        }
        bool use_matte = tct >= 3 && tct <= 6;
        const bool *mfl = matte_flag[(~tct) & 1];
        bool hide = video_fix_crossfade && vd.backjump[path] && icm_of(0) == 5 && icm_of(1) == 5;
        for (int x = 0; x < VIDEO_W; x++) tr[x] = hide || (use_matte && !always ? mfl[x] == parity : always);
        vd.vsr[path] = vsr;
        return;
    }

    bool use_matte = tct >= 3 && tct <= 6;
    const bool *mfl = matte_flag[(~tct) & 1];
    uint32_t vsr2 = vd.vsr[!path];
    int mosaic_factor = 1 << (((vd.ddr[path] >> 2) & 3) + 1);
    for (int x = 0; x < VIDEO_W;) {
        uint8_t b = (uint8_t)rd8(vsr++);
        uint32_t c0, c1;
        bool rgbt = false;
        if (rgb) {
            uint8_t b1 = (uint8_t)rd8(vsr2++);
            uint32_t bl = (b & 31) << 3, gr = ((b & 0xE0) >> 2) + ((b1 & 3) << 6), rd = (b1 & 0x7C) << 1;
            c0 = c1 = rd << 16 | gr << 8 | bl;
            rgbt = tct == 2 && ((b1 >> 7) & 1) == parity;
        } else if (icm == 11) {
            uint8_t m = ft == 2 ? 7 : 15;
            c0 = vd.clut[clut_index(path, icm, m & (b >> 4))];
            c1 = vd.clut[clut_index(path, icm, m & b)];
        } else {
            c0 = c1 = vd.clut[clut_index(path, icm, b)];
        }
        int len = ft == 3 ? mosaic_factor * 2 : 2;
        if (ft == 2) {
            int n = (b & 0x80) ? rd8(vsr++) : 1;
            len = n ? n * 2 : VIDEO_W;
        }
        bool k0 = use_key && (((maskbits & c0) == key) == parity);
        bool k1 = use_key && (((maskbits & c1) == key) == parity);
        int end = x + len < VIDEO_W ? x + len : VIDEO_W;
        for (; x < end; x += 2) {
            px[x] = c0; px[x + 1] = c1;
            tr[x] = always || rgbt || k0 || (use_matte && mfl[x] == parity);
            tr[x + 1] = always || rgbt || k1 || (use_matte && mfl[x + 1] == parity);
        }
    }
    vd.vsr[path] = vsr;
    if (rgb) vd.vsr[!path] = vsr2;
}

static int lim(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void mix_line(const uint32_t *a, const bool *ta, const uint32_t *b, const bool *tb, uint32_t *out)
{
    bool ab = !(vd.plane_order & 1);
    bool nomix = vd.tcr & 0x800000;
    static int only = -2;   /* debug: LL_PLANE_ONLY=a|b shows one plane */
    if (only == -2) { const char *e = getenv("LL_PLANE_ONLY"); only = e ? (*e == 'a' ? 0 : 1) : -1; }
    if (only >= 0) {
        for (int x = 0; x < VIDEO_W; x++)
            out[x] = only == 0 ? (ta[x] ? 0xFFFF00FFu : 0xFF000000u | a[x]) : (tb[x] ? 0xFFFF00FFu : 0xFF000000u | b[x]);
        return;
    }
    for (int x = 0; x < VIDEO_W; x++) {
        if (ta[x] && tb[x]) { out[x] = 0xFF000000 | DISP4[vd.backdrop]; continue; }
        uint32_t pa = ta[x] ? COL4[0] : a[x], pb = tb[x] ? COL4[0] : b[x];
        if (!ta[x] && ab && nomix) pb = COL4[0];
        if (!tb[x] && !ab && nomix) pa = COL4[0];
        int ch[3];
        for (int k = 0; k < 3; k++) {
            int ca = (pa >> (16 - 8 * k)) & 255, cb = (pb >> (16 - 8 * k)) & 255;
            int m = lim(((ca - 16) * line_weight[0][x] >> 6) + ((cb - 16) * line_weight[1][x] >> 6) + 16);
            ch[k] = lim(((m - 16) * 298 + 128) >> 8);
        }
        out[x] = 0xFF000000u | (uint32_t)ch[0] << 16 | (uint32_t)ch[1] << 8 | (uint32_t)ch[2];
    }
}

static void draw_cursor(int y, uint32_t *out)
{
    if (!cursor.on) return;
    int r = y - cursor.y;
    if (r < 0 || r >= 16) return;
    for (int x = 0; x < 16; x++)
        if (cursor.pat[r] & (0x8000 >> x)) {
            int px = (cursor.x + x) * 2;
            for (int j = 0; j < 2; j++)
                if (px + j >= 0 && px + j < VIDEO_W) out[px + j] = 0xFF000000 | cursor.color;
        }
}

/* Render one field. Returns the number of INTERRUPT instructions executed (line events). */
int video_field(void)
{
    if (video_debug_field >= 0 && ll_frame_count > video_debug_field - 120 && ll_frame_count < video_debug_field + 20)
        ll_log("        ---- field %d\n", ll_frame_count);
    line_event_requests = 0;
    if (video_trace_fields) ll_log("        -- field %d\n", ll_frame_count);
    int cur[2] = { -1, -1 }, line[2] = { 0, 0 };
    bool ended[2] = { false, false };
    for (int p = 0; p < 2; p++) {
        vd.stopped[p] = false;
        vd.first_vsr[p] = 0;
        vd.backjump[p] = false;
        Table *f = exec_fct[p] >= 0 ? &fcts[exec_fct[p]] : NULL;
        if (!f) continue;
        for (int k = 0; k < f->count; k++)
            if (!exec_ins(p, f->ins[k])) break;
        cur[p] = f->link_tab; line[p] = f->link_line;
        if (ll_frame_count == video_debug_field) {   /* debug: dump the executed FCT */
            ll_log("        fct%d (%d ins, link LCT %d line %d):", exec_fct[p], f->count, f->link_tab, f->link_line);
            for (int k = 0; k < f->count; k++) { if (k % 8 == 0) ll_log("\n          %3d:", k); ll_log(" %08x", f->ins[k]); }
            ll_log("\n");
        }
    }
    uint32_t pa[VIDEO_W], pb[VIDEO_W];
    bool ta[VIDEO_W], tb[VIDEO_W];
    for (int y = 0; y < VIDEO_H; y++) {
        for (int p = 0; p < 2; p++) {
            if (cur[p] < 0) continue;
            Table *l = &lcts[cur[p]];
            if (!l->used || line[p] >= l->count) { cur[p] = -1; ended[p] = true; continue; }
            if (ll_frame_count == video_debug_field) {
                bool any = false;
                for (int c = 0; c < LCT_COLS; c++) any |= (l->ins[line[p] * LCT_COLS + c] >> 28) != 1;
                if (any) {
                    ll_log("        lct%d line %3d (y %3d):", cur[p], line[p], y);
                    for (int c = 0; c < LCT_COLS; c++) ll_log(" %08x", l->ins[line[p] * LCT_COLS + c]);
                    ll_log("\n");
                }
            }
            in_lct = true;
            if (!vd.stopped[p])
                for (int c = 0; c < LCT_COLS; c++)
                    if (!exec_ins(p, l->ins[line[p] * LCT_COLS + c])) { break; }
            in_lct = false;
            int jt = l->jump_tab[line[p]], jl = l->jump_line[line[p]];
            if (jt >= 0 && !(jt == cur[p] && jl == line[p])) { cur[p] = jt; line[p] = jl; }
            else if (jt == cur[p] && jl == line[p]) cur[p] = -1;   /* self link: the line repeats, picture data keeps streaming */
            else line[p]++;
        }
        /* The active picture is 240 lines from row 20 (MAME, real ROM: black from row 260 on every
         * screen); below it the planes would show whatever memory follows their pictures. */
        if (y >= ACTIVE_END) {
            for (int x = 0; x < VIDEO_W; x++) ll_frame[y][x] = 0xFF000000;
            continue;
        }
        if (exec_fct[0] < 0 && exec_fct[1] < 0) {
            for (int x = 0; x < VIDEO_W; x++) ll_frame[y][x] = 0xFF000000;
            continue;
        }
        if (ll_frame_count == video_debug_field && (y % 10 == 0 || y > 200))
            ll_log("        vline %3d: A lct %d/%d vsr %06x icm %x ft %d w %d | B lct %d/%d vsr %06x icm %x ft %d w %d | tcr %06x\n", y,
                   cur[0], line[0], vd.vsr[0], icm_of(0), vd.ddr[0] & 3, vd.weight[0],
                   cur[1], line[1], vd.vsr[1], icm_of(1), vd.ddr[1] & 3, vd.weight[1], vd.tcr);
        matte_line();
        decode_line(0, pa, ta);
        decode_line(1, pb, tb);
        /* past the end of its line table a plane shows nothing (black on the real machine below
         * the planet scenes, instead of whatever memory follows the last picture line) */
        if (ended[0]) for (int x = 0; x < VIDEO_W; x++) ta[x] = true;
        if (ended[1]) for (int x = 0; x < VIDEO_W; x++) tb[x] = true;
        mix_line(pa, ta, pb, tb, ll_frame[y]);
        draw_cursor(y, ll_frame[y]);
    }
    ll_frame_count++;
    return line_event_requests;
}

/* ------------------------------------------------------------------ */
/* Driver entry points (I$SetStt on the video path)                    */

/* Cost of the ROM's UCM display driver on the real 68070, charged to the caller: a fixed cost per
 * call plus a cost per display-table entry written. Calibrated against MAME running the real ROM:
 * the intro's fades (2 single-entry writes per weight step, 64 steps in ~0.4 s; here 24 fields) and
 * the planet's walk (a step every 5 fields, ~75 px/s; here 5 fields, 79 px/s). Without it the game
 * loops run far faster than on the hardware. */
int ll_ucm_base = 50000, ll_ucm_entry = 1100;

/* Returns 0 or an OS-9 error. D0 receives results for create calls. */
int video_setstt(int code, uint32_t *d0)
{
    os9_charge(ll_ucm_base);
    int sub = REG(D2) & 0xFFFF;
    uint32_t d3 = REG(D3), d4 = REG(D4), d5 = REG(D5), d6 = REG(D6), d7 = REG(D7), a0 = REG(A0);
    Table *t;
    if (code == 0x52) {   /* graphics cursor (gc_) (verify sub-codes) */
        switch (sub) {
        case 0: cursor.x = (int16_t)(d3 >> 16); cursor.y = (int16_t)d3; break;   /* position */
        case 2: cursor.on = true; break;
        case 6: cursor.on = false; break;
        case 3: for (int k = 0; k < 16; k++) cursor.pat[k] = (uint16_t)rd16(a0 + 2 * k); break;
        case 5: cursor.color = DISP4[d3 & 15]; break;
        }
        return 0;
    }
    if (code != 0x56) return 0;
    ll_log("        dc %02x: d3=%x d4=%x d5=%x d6=%x d7=%x a0=%06x\n", sub, d3 & 0xFFFF, d4 & 0xFFFF, d5, d6, d7 & 0xFFFF, a0);
    switch (sub) {
    case 0x00: case 0x06: {   /* dc_crfct / dc_crlct (plane, size) -> id */
        /* an LCT's size is in frame lines: two per table row (see the line-number note below) */
        int i = tab_create(sub == 6, d3 & 0xFFFF, sub == 6 ? (d4 & 0xFFFF) / 2 : (d4 & 0xFFFF));
        if (i < 0) return E_MEMFUL;
        *d0 = (uint32_t)i;
        ll_log("        video: create %s plane %d size %d -> id %d\n", sub ? "LCT" : "FCT", d3 & 1, d4 & 0xFFFF, i);
        return 0;
    }
    case 0x03: case 0x09:     /* dc_dlfct / dc_dllct (mirrors create 0 / 6) */
        ll_log("        video: delete %s %d\n", sub == 9 ? "LCT" : "FCT", d3 & 0xFFFF);
        tab_delete(sub == 9, d3);
        return 0;
    case 0x01: case 0x02:     /* dc_rdfct / dc_wrfct (fct, start, count, buf) */
        if (!(t = fct(d3))) return E_BPNUM;
        os9_charge(ll_ucm_entry * (int)(d5 & 0xFFFF));
        for (uint32_t k = 0; k < (d5 & 0xFFFF); k++) {
            uint32_t i = (d4 & 0xFFFF) + k;
            if (i >= (uint32_t)t->count) break;
            if (sub == 2) t->ins[i] = rd32(a0 + 4 * k); else wr32(a0 + 4 * k, t->ins[i]);
        }
        return 0;
    case 0x04:                /* dc_wrfi (fct, index, instruction) */
        if (!(t = fct(d3)) || (d4 & 0xFFFF) >= (uint32_t)t->count) return E_BPNUM;
        os9_charge(ll_ucm_entry);
        t->ins[d4 & 0xFFFF] = d5;
        return 0;
    /* Line numbers: the ROM's UCM has two address helpers (video driver +0x4a82 / +0x4c6c). Block
     * reads and writes (0x1b, 0x1c) count table rows; everything else (0x07, 0x08, 0x0a, 0x0c,
     * 0x0d, 0x10) takes frame lines and uses row = line / 2 (odd lines go to the second field's
     * half of an interlaced table; Laser Lords' tables aren't interlaced). With rows everywhere,
     * the planet's double-buffered scroll (LCT 0 rows 14+ vs LCT 6 linked from line 26) jumped
     * 13 lines every other frame, and LCT 6's link back (line 320 -> LCT 0 line 350) missed the
     * menu bar at row 175. */
    case 0x0A: {              /* dc_wrli (lct, line, column, instruction) */
        int row = (d4 & 0xFFFF) >> 1;
        if (!(t = lct(d3)) || row >= t->count) return E_BPNUM;
        os9_charge(ll_ucm_entry);
        t->ins[row * LCT_COLS + (d5 & 7)] = d6;
        if ((d5 & 7) == 7) t->jump_tab[row] = -1;   /* column 7 held the link */
        return 0;
    }
    case 0x07: case 0x08: case 0x1B: case 0x1C: case 0x10: {
        /* LCT blocks (lct, line, column, lines, columns, buf): 0x1b read, 0x1c write,
         * 0x10 fill with NOP (no buffer). 7/8 unknown read/write variants (verify). */
        if (!(t = lct(d3))) return E_BPNUM;
        int l0 = d4 & 0xFFFF, c0 = d5 & 0xFFFF, nl = d6 & 0xFFFF, nc = d7 & 0xFFFF, k = 0;
        if (sub != 0x1B && sub != 0x1C) l0 >>= 1;   /* starting frame line -> row; the count is rows */
        os9_charge(ll_ucm_entry * nl * nc);
        for (int l = l0; l < l0 + nl && l < t->count; l++)
            for (int c = c0; c < c0 + nc && c < LCT_COLS; c++, k++) {
                uint32_t *slot = &t->ins[l * LCT_COLS + c];
                if (c == 7 && sub != 0x1B && sub != 0x07) t->jump_tab[l] = -1;   /* link lived in column 7 */
                if (sub == 0x10) *slot = 0x10000000;
                else if (sub == 0x1C || sub == 0x08) {
                    *slot = rd32(a0 + 4 * k);
                    if (ll_trace) ll_log("          lct%d[%d][%d] = %08x\n", d3 & 0xFFFF, l, c, *slot);
                }
                else wr32(a0 + 4 * k, *slot);
            }
        return 0;
    }
    case 0x0C:                /* dc_flnk (fct, lct, line) (verify) */
        if (!(t = fct(d3))) return E_BPNUM;
        t->link_tab = lct(d4) ? (int)(d4 & 0xFFFF) : -1;
        t->link_line = (d5 & 0xFFFF) >> 1;
        ll_log("        video: FCT %d -> LCT %d line %d\n", d3 & 0xFFFF, d4 & 0xFFFF, d5 & 0xFFFF);
        return 0;
    case 0x0D:                /* dc_llnk (lct, line, lct2, line2) */
        /* As the ROM's UCM does it: the link is a "reload DCP" instruction written into column 7
         * of the line (replacing whatever was there), so a later write to that column removes it.
         * cdi_tribes relies on this: it rewrites column 7 of lines 14-173 to scroll, which drops the
         * split-screen link, and re-links when it wants it (the conversation menu bar at line 175
         * only shows without the link). */
        {
            int row = (d4 & 0xFFFF) >> 1, row2 = (d6 & 0xFFFF) >> 1;
            if (!(t = lct(d3)) || row >= t->count) return E_BPNUM;
            t->ins[row * LCT_COLS + 7] = 0x10000000;
            t->jump_tab[row] = (int16_t)(lct(d5) ? (int)(d5 & 0xFFFF) : -1);
            t->jump_line[row] = (int16_t)row2;
        }
        return 0;
    case 0x0E:                /* dc_exec (fct plane A, fct plane B) */
        exec_fct[0] = fct(d3) ? (int)(d3 & 0xFFFF) : -1;
        exec_fct[1] = fct(d4) ? (int)(d4 & 0xFFFF) : -1;
        ll_log("        video: exec FCT A=%d B=%d\n", d3 & 0xFFFF, d4 & 0xFFFF);
        return 0;
    }
    (void)d7;
    ll_log("        video: sub-code %02x not implemented (d3=%x d4=%x d5=%x)\n", sub, d3, d4, d5);
    return 0;
}

void video_reset(void)
{
    for (int i = 0; i < MAXTAB; i++) { tab_delete(false, (uint32_t)i); tab_delete(true, (uint32_t)i); }
    memset(&vd, 0, sizeof vd);
    memset(&cursor, 0, sizeof cursor);
    exec_fct[0] = exec_fct[1] = -1;
}

/* ------------------------------------------------------------------ */
/* PNG writer for frame dumps (stored deflate blocks: no zlib needed)   */

static uint32_t crc_tab[256];
static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t r = i; for (int k = 0; k < 8; k++) r = r & 1 ? 0xEDB88320 ^ (r >> 1) : r >> 1; crc_tab[i] = r; }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}

static void put32(FILE *f, uint32_t v) { fputc(v >> 24, f); fputc(v >> 16, f); fputc(v >> 8, f); fputc(v, f); }

static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    put32(f, n);
    fwrite(type, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32_upd(0, (const uint8_t *)type, 4);
    c = crc32_upd(c, d, n);
    put32(f, c);
}

/* Save the frame as RGB PNG at 384 x 280 (every other 768-unit column). */
bool video_save_png(const char *path)
{
    const int w = VIDEO_W / 2, h = VIDEO_H, row = 1 + w * 3;
    size_t raw_n = (size_t)row * h;
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = ll_frame[y][x * 2];
            raw[y * row + 1 + x * 3] = c >> 16; raw[y * row + 2 + x * 3] = c >> 8; raw[y * row + 3 + x * 3] = (uint8_t)c;
        }
    }
    size_t blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(z_n), *p = z;
    *p++ = 0x78; *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_n;) {
        size_t n = raw_n - off > 65535 ? 65535 : raw_n - off;
        *p++ = off + n == raw_n;
        *p++ = n & 255; *p++ = n >> 8; *p++ = ~n & 255; *p++ = (~n >> 8) & 255;
        memcpy(p, raw + off, n);
        for (size_t i = 0; i < n; i++) { a = (a + raw[off + i]) % 65521; b = (b + a) % 65521; }
        p += n; off += n;
    }
    uint32_t ad = b << 16 | a;
    *p++ = ad >> 24; *p++ = ad >> 16; *p++ = ad >> 8; *p++ = (uint8_t)ad;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return false; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13] = { 0 };
    ih[0] = w >> 24; ih[1] = w >> 16; ih[2] = w >> 8; ih[3] = (uint8_t)w;
    ih[4] = h >> 24; ih[5] = h >> 16; ih[6] = h >> 8; ih[7] = (uint8_t)h;
    ih[8] = 8; ih[9] = 2;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", z, (uint32_t)(p - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    return true;
}
