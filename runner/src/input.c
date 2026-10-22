/* Pointer device (UCM pt_ calls: Set/GetStt 0x59 on the video or pointer path).
 *   GetStt sub 0 (pt_coord): D0.w = buttons (bit 0 = button 1, bit 1 = button 2), D1 = x << 16 | y
 *   SetStt sub 3 (pt_pos):   D3 = x << 16 | y
 * Coordinates are in double-resolution units (768 x 560 for PAL): cdi_space sets the pointer to
 * twice its own screen coordinates. The host moves the pointer relatively (mouse, stick, keys),
 * so the game can reposition it at will.
 *
 * cdi_tribes uses the pointer as a joystick: it re-centres it to (360, 225) on every read and
 * turns an offset of more than ~20 units into a direction. So sticks and keys also add a
 * deflection offset (up to DEFLECT units) to the reported position, and mouse motion leaves a
 * short-lived offset, so holding a direction keeps registering. Buttons carry bit 15
 * (cdi_tribes tests for 0x8001 / 0x8002).
 *
 * Skipping cutscenes: the game reads the pointer where it lets the player skip (the logo and
 * title). The intro slideshow and other cutscenes don't read it at all, so a button press while
 * a real-time play runs and the pointer hasn't been read for a second ends that play early.
 * Only in cdi_space (intro, warps, video messages): on the planets cdi_tribes streams scenery and
 * conversations from the disc, and ending those plays would leave it waiting forever. */
#include "ll.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG(r)      m68k_get_reg(NULL, M68K_REG_##r)
#define SETREG(r,v) m68k_set_reg(M68K_REG_##r, (v))

#define PTR_W 768
#define PTR_H 560

#define DEFLECT 64.0

static double ptr_x = PTR_W / 2, ptr_y = PTR_H / 2;
static double off_x, off_y;          /* stick / key deflection */
static double moff_x, moff_y;        /* decaying mouse offset */
static int ptr_buttons;
static uint64_t last_read;           /* emulated cycles at the last pt_coord */

uint64_t os9_cycles(void);

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

void input_move_pointer(double dx, double dy)
{
    ptr_x = clampd(ptr_x + dx, 0, PTR_W - 1);
    ptr_y = clampd(ptr_y + dy, 0, PTR_H - 1);
}
void input_set_buttons(int buttons)
{
    if ((buttons & ~ptr_buttons) && cdfm_skippable() && os9_cycles() - last_read > ll_cpu_hz &&
        os9_process_alive("cdi_space") && !os9_process_alive("cdi_tribes"))
        cdfm_skip();
    ptr_buttons = buttons;
}

/* sx, sy in -1..1 (stick or keys), set every host frame. */
void input_set_deflection(double sx, double sy) { off_x = sx * DEFLECT; off_y = sy * DEFLECT; }

/* Mouse motion: moves the pointer and leaves an offset that fades within ~0.15 s. */
void input_mouse_motion(double dx, double dy)
{
    input_move_pointer(dx, dy);
    moff_x = clampd(moff_x + dx * 4, -DEFLECT, DEFLECT);
    moff_y = clampd(moff_y + dy * 4, -DEFLECT, DEFLECT);
}

void input_decay(double dt)
{
    double k = dt > 0.15 ? 0 : 1 - dt / 0.15;
    moff_x *= k; moff_y *= k;
}
void input_set_pointer(int x, int y, int buttons) { ptr_x = x; ptr_y = y; ptr_buttons = buttons; }
void input_get_pointer(int *x, int *y, int *buttons) { *x = (int)ptr_x; *y = (int)ptr_y; *buttons = ptr_buttons; }

int input_getstt(int sub)
{
    os9_charge(ll_ucm_base);
    if (sub == 0) {
        last_read = os9_cycles();
        int b = ptr_buttons & 3;
        int x = (int)clampd(ptr_x + off_x + moff_x, 0, PTR_W - 1);
        int y = (int)clampd(ptr_y + off_y + moff_y, 0, PTR_H - 1);
        SETREG(D0, (REG(D0) & 0xFFFF0000u) | (b ? 0x8000u | b : 0));
        SETREG(D1, (uint32_t)(x & 0xFFFF) << 16 | (y & 0xFFFF));
        return 0;
    }
    ll_log("        pointer: GetStt sub %d not implemented\n", sub);
    return 0;
}

int input_setstt(int sub)
{
    os9_charge(ll_ucm_base);
    if (sub == 3) {
        ptr_x = clampd((int16_t)(REG(D3) >> 16), 0, PTR_W - 1);
        ptr_y = clampd((int16_t)REG(D3), 0, PTR_H - 1);
        return 0;
    }
    ll_log("        pointer: SetStt sub %d (d3=%x) ignored\n", sub, REG(D3));
    return 0;
}

/* ---- scripted input for headless runs: "T:b1" press button 1 at T seconds (0.2 s),
 *      "T:b2", "T:b3" (both), "T:m:DX,DY" move the pointer ---- */
typedef struct { double t; int kind, buttons; double dx, dy, dur; bool done; } Ev;
static Ev script[128];
static int nscript;

bool input_script_add(const char *spec)
{
    if (nscript >= 128) return false;
    Ev e = { 0 };
    char kind[8] = "";
    if (sscanf(spec, "%lf:%7[^:]", &e.t, kind) < 2) return false;
    if (kind[0] == 'b') { e.kind = 1; e.buttons = kind[1] - '0'; }
    else if (kind[0] == 's') {   /* T:s:SX,SY,DUR  hold the stick */
        const char *c = strrchr(spec, ':');
        if (!c || sscanf(c + 1, "%lf,%lf,%lf", &e.dx, &e.dy, &e.dur) != 3) return false;
        e.kind = 3;
    }
    else if (kind[0] == 'm') {
        const char *c = strrchr(spec, ':');
        if (!c || sscanf(c + 1, "%lf,%lf", &e.dx, &e.dy) != 2) return false;
        e.kind = 2;
    } else return false;
    script[nscript++] = e;
    return true;
}

void input_script_tick(double now)
{
    for (int i = 0; i < nscript; i++) {
        Ev *e = &script[i];
        if (e->kind == 1) {
            if (!e->done && now >= e->t) { ll_log("        input: buttons %d\n", e->buttons); input_set_buttons(e->buttons); e->done = true; }
            else if (e->done && e->buttons && now >= e->t + 0.2) { ptr_buttons = 0; e->buttons = 0; }
        } else if (e->kind == 3) {
            if (!e->done && now >= e->t) { input_set_deflection(e->dx, e->dy); e->done = true; }
            else if (e->done && e->dur > 0 && now >= e->t + e->dur) { input_set_deflection(0, 0); e->dur = 0; }
        } else if (e->kind == 2 && !e->done && now >= e->t) {
            input_move_pointer(e->dx, e->dy);
            e->done = true;
        }
    }
}

/* ---- input recording and replay ----
 * The window records the complete pointer state (position, deflections, buttons) every time it
 * changes, stamped with the emulated cycle count. Emulation is deterministic, so a headless
 * --replay of the file runs the same session again: the way to reproduce a player's bug.
 * File: "LLREC 1 speed S" header, then lines "cycle x y ox oy mx my buttons" (hex floats). */
static FILE *rec;
static double rec_last[7] = { -1 };
static int rec_last_b = -1;

bool input_record_open(const char *path, int speed)
{
    rec = fopen(path, "w");
    if (!rec) return false;
    fprintf(rec, "LLREC 1 speed %d\n", speed);
    return true;
}

void input_record_tick(uint64_t cycle)
{
    if (!rec) return;
    double v[7] = { ptr_x, ptr_y, off_x, off_y, moff_x, moff_y, 0 };
    if (!memcmp(v, rec_last, sizeof v) && ptr_buttons == rec_last_b) return;
    memcpy(rec_last, v, sizeof v);
    rec_last_b = ptr_buttons;
    fprintf(rec, "%llu %a %a %a %a %a %a %d\n", (unsigned long long)cycle, v[0], v[1], v[2], v[3], v[4], v[5], ptr_buttons);
}

void input_record_close(void) { if (rec) fclose(rec); rec = NULL; }

typedef struct { uint64_t c; double v[6]; int b; } RecEv;
static RecEv *rp;
static int rp_n, rp_i;

/* Returns the recorded game speed, or -1 if the file can't be read. */
int input_replay_open(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int speed = 100;
    if (fscanf(f, "LLREC 1 speed %d", &speed) != 1) { fclose(f); return -1; }
    int cap = 0;
    unsigned long long c;
    double v[6];
    int b;
    while (fscanf(f, "%llu %la %la %la %la %la %la %d", &c, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &b) == 8) {
        if (rp_n == cap) { cap = cap ? cap * 2 : 1024; rp = realloc(rp, sizeof *rp * cap); }
        rp[rp_n].c = c; memcpy(rp[rp_n].v, v, sizeof v); rp[rp_n].b = b; rp_n++;
    }
    fclose(f);
    /* debug: LL_REPLAY_SHIFT=D (cycles) moves every event at or after LL_REPLAY_SHIFT_FROM, to line
     * an old recording up again after a fix changed the timing */
    const char *sh = getenv("LL_REPLAY_SHIFT"), *from = getenv("LL_REPLAY_SHIFT_FROM");
    if (sh) {
        long long d = atoll(sh); unsigned long long f0 = from ? strtoull(from, NULL, 10) : 0;
        for (int i = 0; i < rp_n; i++) if (rp[i].c >= f0) rp[i].c = (uint64_t)((long long)rp[i].c + d);
    }
    return speed;
}

uint64_t input_replay_next(void) { return rp_i < rp_n ? rp[rp_i].c : UINT64_MAX; }

void input_replay_apply(uint64_t cycle)
{
    while (rp_i < rp_n && rp[rp_i].c <= cycle) {
        RecEv *e = &rp[rp_i++];
        ptr_x = e->v[0]; ptr_y = e->v[1]; off_x = e->v[2]; off_y = e->v[3]; moff_x = e->v[4]; moff_y = e->v[5];
        if (e->b != ptr_buttons) ll_log("        replay: buttons %d\n", e->b);
        input_set_buttons(e->b);
    }
}
