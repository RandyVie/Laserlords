/* OS-9/68k kernel subset: modules, processes, memory, signals, alarms, events.
 * Every TRAP #0 is logged to ll_trace. Unknown calls are logged and fail with E$UnkSvc. */
#include "ll.h"
#include "m68k.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <time.h>

/* ------------------------------------------------------------------ */
/* CPU register helpers                                                */

#define REG(r)      m68k_get_reg(NULL, M68K_REG_##r)
#define SETREG(r,v) m68k_set_reg(M68K_REG_##r, (v))

static void set_w(m68k_register_t r, uint32_t v)   /* write low word, keep high word */
{
    m68k_set_reg(r, (m68k_get_reg(NULL, r) & 0xFFFF0000u) | (v & 0xFFFF));
}
/* Touching another process's registers mid-syscall: stash the running CPU state first. */
static void *saved_ctx;
static void cpu_save(void)    { if (!saved_ctx) saved_ctx = malloc(m68k_context_size()); m68k_get_context(saved_ctx); }
static void cpu_restore(void) { m68k_set_context(saved_ctx); }

static void ok(void)        { SETREG(SR, REG(SR) & ~1u); }
static void err(int e)      { SETREG(SR, REG(SR) | 1u); set_w(M68K_REG_D1, e); }

/* ------------------------------------------------------------------ */
/* Modules                                                             */

typedef struct { uint32_t addr; int links; char name[32]; } ModEntry;
static ModEntry mods[64];
static int nmods;

static void mod_name(uint32_t m, char *out) { mem_cstr(m + rd32(m + 0x0C), out, 32); }

static void mod_fix_parity(uint32_t m)
{
    uint32_t p = 0;
    wr16(m + 0x2E, 0);
    for (int i = 0; i < 0x2E; i += 2) p ^= rd16(m + i);
    wr16(m + 0x2E, ~p & 0xFFFF);
}

static int mod_find(const char *name)
{
    for (int i = 0; i < nmods; i++)
        if (mods[i].addr && !strcasecmp(mods[i].name, name)) return i;
    return -1;
}

static int mod_register(uint32_t m)
{
    for (int i = 0; i < 64; i++)
        if (!mods[i].addr) {
            mods[i].addr = m; mods[i].links = 0; mod_name(m, mods[i].name);
            if (i >= nmods) nmods = i + 1;
            ll_log("        module %-16s at %06x size %x type %02x exec %x\n", mods[i].name, m,
                   rd32(m + 4), rd8(m + 0x12), rd32(m + 0x30));
            return i;
        }
    ll_fatal("module directory full");
    return -1;
}

/* Load every module in a disc file. Returns index of the first, or -1 with *e set. */
static int mod_load_file(const char *path, int *e)
{
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    const DiscFile *f = disc_find(path);
    if (!f) f = disc_find(base);
    if (!f) { *e = E_PNNF; return -1; }
    uint8_t *buf = disc_read_file(f);
    int first = -1;
    for (uint32_t off = 0; off + 0x30 <= f->size;) {
        if (buf[off] != 0x4A || buf[off + 1] != 0xFC) break;
        uint32_t size = (uint32_t)buf[off + 4] << 24 | buf[off + 5] << 16 | buf[off + 6] << 8 | buf[off + 7];
        if (size < 0x30 || off + size > f->size) break;
        char name[32];
        uint32_t no = (uint32_t)buf[off + 12] << 24 | buf[off + 13] << 16 | buf[off + 14] << 8 | buf[off + 15];
        snprintf(name, sizeof name, "%s", (char *)buf + off + no);
        int idx = mod_find(name);
        if (idx < 0) {
            uint32_t m = modarea_alloc(size);
            if (!m) { free(buf); *e = E_MEMFUL; return -1; }
            memcpy(host(m), buf + off, size);
            idx = mod_register(m);
        }
        if (first < 0) first = idx;
        mods[idx].links++;
        off += (size + 1) & ~1u;
    }
    free(buf);
    if (first < 0) *e = E_BMODE;
    return first;
}

static bool mod_in_use(int idx);

/* Drop one link; at zero (and not the code of a live process) the module leaves memory. */
static void mod_unlink(int i)
{
    if (i < 0 || !mods[i].addr) return;
    if (mods[i].links > 0) mods[i].links--;
    if (mods[i].links > 0 || mod_in_use(i)) return;
    ll_log("        module %s removed from memory\n", mods[i].name);
    modarea_free(mods[i].addr, rd32(mods[i].addr + 4));
    mods[i].addr = 0;
}

/* ------------------------------------------------------------------ */
/* Processes                                                           */

enum { P_FREE, P_READY, P_WAIT, P_SLEEP, P_EVWAIT, P_DEAD };
#define MAXPROC 16
#define NPATHS  32
#define MEM_SLACK 0x40000u   /* reserved above each process area so F$Mem can grow it */

typedef struct {
    int      state, id, parent, prio;
    char     name[32];
    void    *ctx;
    int      mod;                    /* module directory index */
    uint32_t mem, memsize, memmax;   /* data area */
    int      paths[NPATHS];          /* local -> system path, -1 = closed */
    uint32_t icpt_pc, icpt_a6;
    void    *sig_ctx;                /* context saved while in the intercept routine */
    bool     in_icpt;
    uint16_t sigq[32]; int nsig;
    uint64_t wake_tick;              /* P_SLEEP: 0 = until signal */
    int      exit_status;
    /* event wait */
    int ev_id; int32_t ev_min, ev_max;
} Proc;

static Proc procs[MAXPROC];
static int vbl_sig[MAXPROC];         /* per-process vertical blank signal, 0 = none */
static void *clean_ctx;              /* CPU state right after reset: template for new processes */
static Proc *cur;
static uint64_t ticks;               /* virtual clock */
#define TICKS_PER_SEC 100            /* (verify) CD-RTOS tick rate */
uint64_t ll_cpu_hz = 15000000;       /* emulated 68070 clock (Musashi 68000 cycle timing) */
#define CYCLES_PER_TICK (ll_cpu_hz / TICKS_PER_SEC)
static uint64_t total_cycles;
uint32_t ll_field_hz = 50;           /* display fields per second (PAL) */
static uint64_t next_field_cycle;
static int last_exit = -1;

bool os9_process_alive(const char *name)
{
    for (int i = 0; i < MAXPROC; i++)
        if (procs[i].state != P_FREE && procs[i].state != P_DEAD && !strcmp(procs[i].name, name)) return true;
    return false;
}

static bool mod_in_use(int idx)
{
    for (int i = 0; i < MAXPROC; i++)
        if (procs[i].state != P_FREE && procs[i].state != P_DEAD && procs[i].mod == idx) return true;
    return false;
}

static Proc *proc_by_id(int id)
{
    for (int i = 0; i < MAXPROC; i++)
        if (procs[i].state != P_FREE && procs[i].id == id) return &procs[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Process creation                                                    */

static void proc_setup_regs(Proc *p, uint32_t module, uint32_t params, uint32_t plen, int npaths)
{
    uint32_t staticsz = rd32(module + 0x38);
    uint32_t stack = rd32(module + 0x3C);
    uint32_t total = ((staticsz + 0xF) & ~0xFu) + stack + ((plen + 0xF) & ~0xFu);
    p->memsize = total;
    p->memmax = total + MEM_SLACK;
    p->mem = heap_alloc(p->memmax);
    if (!p->mem) ll_fatal("out of memory forking %s", p->name);
    uint32_t a6 = p->mem + 0x8000;
    uint32_t top = p->mem + total;
    uint32_t pstart = top - ((plen + 0xF) & ~0xFu);
    for (uint32_t i = 0; i < plen; i++) wr8(pstart + i, rd8(params + i));

    /* Initialised data (M$IData) and relocations (M$IRefs): done by the kernel on fork. */
    uint32_t id = rd32(module + 0x40);
    if (id) {
        uint32_t off = rd32(module + id), size = rd32(module + id + 4);
        memcpy(host(p->mem + off), host(module + id + 8), size);
    }
    uint32_t ir = rd32(module + 0x44);
    if (ir) {
        uint32_t a = module + ir;
        for (int table = 0; table < 2; table++) {
            uint32_t bias = table == 0 ? module : p->mem;
            for (;;) {
                uint32_t hi = rd16(a), n = rd16(a + 2); a += 4;
                if (!hi && !n) break;
                for (uint32_t k = 0; k < n; k++, a += 2) {
                    uint32_t at = p->mem + (hi << 16 | rd16(a));
                    wr32(at, rd32(at) + bias);
                }
            }
        }
    }

    m68k_set_context(p->ctx);
    for (int r = M68K_REG_D0; r <= M68K_REG_A7; r++) m68k_set_reg(r, 0);
    SETREG(D0, p->id);
    SETREG(D1, 0);               /* group.user = 0.0 (super user) */
    SETREG(D2, p->prio);
    SETREG(D3, npaths);
    SETREG(D5, plen);
    SETREG(D6, total);
    SETREG(A1, top);
    SETREG(A3, module);
    SETREG(A5, pstart);
    SETREG(A6, a6);
    SETREG(A7, pstart);
    SETREG(SP, pstart);
    SETREG(PC, module + rd32(module + 0x30));
    SETREG(SR, 0x2000);          /* supervisor mode, flat memory: no difference for the game */
    m68k_get_context(p->ctx);
    ll_log("        %s: data %06x-%06x a6=%06x stack top %06x params %u bytes\n",
           p->name, p->mem, top, a6, pstart, plen);
}

static void pchist_reset(void);

static int proc_create(int modidx, int parent, uint32_t params, uint32_t plen, int npaths, int prio)
{
    pchist_reset();   /* the hot-PC report then describes the newest program */
    static int next_id = 1;
    Proc *p = NULL;
    for (int i = 0; i < MAXPROC; i++) if (procs[i].state == P_FREE) { p = &procs[i]; break; }
    if (!p) return -1;
    void *ctx = p->ctx ? p->ctx : malloc(m68k_context_size());
    void *sctx = p->sig_ctx ? p->sig_ctx : malloc(m68k_context_size());
    memcpy(ctx, clean_ctx, m68k_context_size());
    memset(p, 0, sizeof *p);
    p->ctx = ctx; p->sig_ctx = sctx;
    vbl_sig[p - procs] = 0;
    p->id = next_id++;
    p->parent = parent;
    p->prio = prio ? prio : 128;
    p->mod = modidx;
    snprintf(p->name, sizeof p->name, "%s", mods[modidx].name);
    for (int i = 0; i < NPATHS; i++) p->paths[i] = -1;
    Proc *par = proc_by_id(parent);
    for (int i = 0; i < npaths && i < NPATHS; i++)
        if (par && par->paths[i] >= 0) io_dup(par->paths[i], &p->paths[i]);
    if (!par) for (int i = 0; i < 3; i++) p->paths[i] = i;   /* console for the first process */
    proc_setup_regs(p, mods[modidx].addr, params, plen, npaths);
    p->state = P_READY;
    return p->id;
}

static void release_proc_resources(Proc *p);

static void proc_kill(Proc *p, int status)
{
    ll_log("        process %d (%s) exits, status %d\n", p->id, p->name, status);
    release_proc_resources(p);
    for (int i = 0; i < NPATHS; i++) if (p->paths[i] >= 0) { io_close(p->paths[i]); p->paths[i] = -1; }
    if (p->mem) heap_free(p->mem, p->memmax);
    p->mem = 0;
    p->exit_status = status;
    p->state = P_DEAD;
    mod_unlink(p->mod);
    p->mod = -1;
    last_exit = status;
    Proc *par = proc_by_id(p->parent);
    if (!par) { p->state = P_FREE; return; }
    if (par->state == P_WAIT) {
        cpu_save();
        m68k_set_context(par->ctx);
        set_w(M68K_REG_D0, p->id); set_w(M68K_REG_D1, status); ok();
        m68k_get_context(par->ctx);
        cpu_restore();
        par->state = P_READY;
        p->state = P_FREE;
    }
}

/* Send a signal (OS-9 semantics):
 *   S$Kill (0)  terminates the process, cannot be intercepted
 *   S$Wake (1)  only wakes a sleeping process, never queued
 *   others      queued; delivered to the intercept routine, or fatal without one
 * A sleeping process wakes with D0 = ticks left; an interrupted event wait returns
 * carry set (verify the error code; the game only checks for -1). */
static void send_signal(Proc *p, int sig)
{
    ll_log("        signal %d -> process %d (%s)\n", sig, p->id, p->name);
    if (p->state == P_SLEEP || p->state == P_EVWAIT) {
        cpu_save();
        m68k_set_context(p->ctx);
        if (p->state == P_EVWAIT) err(0xA6);
        else SETREG(D0, p->wake_tick > ticks ? (uint32_t)(p->wake_tick - ticks) : 0);
        m68k_get_context(p->ctx);
        cpu_restore();
        p->state = P_READY;
    }
    if (sig == 1) return;
    if (p->nsig < 32) p->sigq[p->nsig++] = (uint16_t)sig;
    else ll_log("        signal queue full, dropped\n");
}

/* Called with p's context loaded in the CPU, before running it. Signals that arrive while
 * the intercept routine runs stay queued until it returns with F$RTE. */
static void deliver_signals(Proc *p)
{
    if (!p->nsig) return;
    if (p->sigq[0] != 0 && p->in_icpt) return;
    int sig = p->sigq[0];
    memmove(p->sigq, p->sigq + 1, --p->nsig * sizeof p->sigq[0]);
    if (sig == 0 || !p->icpt_pc) {
        ll_log("        signal %d kills process %d (%s)%s\n", sig, p->id, p->name, p->icpt_pc ? "" : ": no intercept routine");
        proc_kill(p, sig);
        return;
    }
    m68k_get_context(p->sig_ctx);
    p->in_icpt = true;
    SETREG(D1, sig);
    SETREG(A6, p->icpt_a6);
    SETREG(PC, p->icpt_pc);
}

/* ------------------------------------------------------------------ */
/* Alarms and events                                                   */

typedef struct { int id, pid, sig; uint64_t at, period; } Alarm;
static Alarm alarms[32];
static int next_alarm = 1;

typedef struct { int id, links; char name[32]; int32_t value; int16_t winc, sinc; } Event;
static Event events[32];
static int next_event = 0x10000;

/* The video driver's field interrupt: CD-RTOS provides an event named "line_event" that
 * programs wait on (Ev$Wait [1..1]) for display timing. Pulsed once per display interrupt. */
static Event *line_event;

static void ev_wake_waiters(Event *e, bool all);
static void send_signal(Proc *p, int sig);

/* Display control 0x56 sub-code 0x11: signal a process at the next vertical blank. One-shot:
 * cdi_space re-arms it from its intercept routine every frame. */
void os9_vblank_signal(int sig) { vbl_sig[cur - procs] = sig; }

static void field_interrupt(void)
{
    {   /* debug: LL_PCHIST_FROM=N restarts the hot-PC statistics at field N */
        static int from = -2;
        if (from == -2) { const char *e = getenv("LL_PCHIST_FROM"); from = e ? atoi(e) : -1; }
        if (from >= 0 && ll_frame_count == from) pchist_reset();
        /* debug: LL_DUMP_FROM=N dumps the running process's registers every 50 fields from N */
        static int dfrom = -2;
        if (dfrom == -2) { const char *e = getenv("LL_DUMP_FROM"); dfrom = e ? atoi(e) : -1; }
        if (dfrom >= 0 && ll_frame_count >= dfrom && (ll_frame_count - dfrom) % 50 == 0 && cur) {
            void *save = malloc(m68k_context_size());
            m68k_get_context(save);
            m68k_set_context(cur->ctx);
            ll_log("        DUMP field %d %s pc %06x\n", ll_frame_count, cur->name, m68k_get_reg(NULL, M68K_REG_PC));
            for (int i = 0; i < 8; i++) ll_log("          D%d=%08x A%d=%08x\n", i, m68k_get_reg(NULL, M68K_REG_D0 + i), i, m68k_get_reg(NULL, M68K_REG_A0 + i));
            uint32_t a7 = m68k_get_reg(NULL, M68K_REG_A7);
            ll_log("          stack:");
            for (int i = 0; i < 24; i++) ll_log(" %08x", rd32(a7 + 4 * i));
            ll_log("\n");
            const char *pk = getenv("LL_PEEK");   /* comma-separated addresses to show (longs) */
            for (const char *q = pk; q && *q; ) {
                uint32_t ad = (uint32_t)strtoul(q, (char **)&q, 16);
                ll_log("          peek %06x: %08x %08x %08x %08x\n", ad, rd32(ad), rd32(ad + 4), rd32(ad + 8), rd32(ad + 12));
                if (*q == ',') q++; else break;
            }
            m68k_set_context(save);
            free(save);
        }
    }
    for (int i = 0; i < MAXPROC; i++)
        if (vbl_sig[i] && procs[i].state != P_FREE && procs[i].state != P_DEAD) {
            int sig = vbl_sig[i];
            vbl_sig[i] = 0;          /* one-shot: the game re-arms it from its signal handler */
            send_signal(&procs[i], sig);
        }
    audio_tick((long long)(total_cycles * 37800 / ll_cpu_hz));
    /* Render the field. The game puts INTERRUPT instructions in its line control tables; the
     * driver turns each into a line_event signal. Without an active display, one per field. */
    int n = video_field();
    if (!line_event) return;
    /* Pulsed: programs wait for the range [1..1]; the value rests at 0 between interrupts. */
    for (int k = 0; k < (n ? n : 1); k++) {
        int32_t rest = line_event->value;
        line_event->value = 1;
        ev_wake_waiters(line_event, true);
        line_event->value = rest;
    }
}

int  os9_cur_pid(void) { return cur ? cur->id : 0; }

void os9_signal_pid(int pid, int sig)
{
    Proc *p = proc_by_id(pid);
    if (p && p->state != P_DEAD) send_signal(p, sig);
}

static void release_proc_resources(Proc *p)
{
    int idx = (int)(p - procs);
    vbl_sig[idx] = 0;
    for (int i = 0; i < 32; i++) if (alarms[i].id && alarms[i].pid == p->id) alarms[i].id = 0;
    cdfm_abort(p->id);
    p->nsig = 0;
    p->in_icpt = false;
}

static void check_alarms(void)
{
    for (int i = 0; i < 32; i++) {
        Alarm *a = &alarms[i];
        if (!a->id || ticks < a->at) continue;
        Proc *p = proc_by_id(a->pid);
        if (p && p->state != P_DEAD) send_signal(p, a->sig);
        if (a->period) a->at += a->period; else a->id = 0;
    }
}

static Event *ev_by_id(int id)
{
    for (int i = 0; i < 32; i++) if (events[i].id && events[i].id == id) return &events[i];
    return NULL;
}

static void ev_wake_waiters(Event *e, bool all)
{
    cpu_save();
    for (int i = 0; i < MAXPROC; i++) {
        Proc *p = &procs[i];
        if (p->state != P_EVWAIT || p->ev_id != e->id) continue;
        if (e->value < p->ev_min || e->value > p->ev_max) continue;
        m68k_set_context(p->ctx);
        SETREG(D1, e->value); ok();
        m68k_get_context(p->ctx);
        e->value += e->winc;
        p->state = P_READY;
        if (!all) break;
    }
    cpu_restore();
}

/* ------------------------------------------------------------------ */
/* Name parsing helpers                                                */

static bool name_match(const char *pat, const char *s)  /* OS-9 wildcards, case-insensitive */
{
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            for (;; s++) { if (name_match(pat + 1, s)) return true; if (!*s) return false; }
        }
        if (!*s) return false;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return false;
    }
    return !*s;
}

static bool name_char(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

/* Module names: skip leading '/', stop at first non-name char. */
static uint32_t read_modname(uint32_t a, char *out)
{
    while (rd8(a) == ' ') a++;
    int n = 0;
    uint32_t p = a;
    while (name_char(rd8(p)) || rd8(p) == '/') {
        if (n < 31) out[n++] = (char)rd8(p);
        p++;
    }
    out[n] = 0;
    char *slash = strrchr(out, '/');
    if (slash) memmove(out, slash + 1, strlen(slash));
    while (rd8(p) == ' ') p++;
    return p;
}

uint32_t os9_crc(uint32_t crc, uint32_t a, uint32_t n)  /* OS-9 module CRC-24 */
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = rd8(a + i);
        crc ^= b << 16;
        for (int k = 0; k < 8; k++) {
            crc <<= 1;
            if (crc & 0x1000000) crc ^= 0x800063;
        }
    }
    return crc & 0xFFFFFF;
}

/* ------------------------------------------------------------------ */
/* System call dispatch                                                */

static const char *fname(int c)
{
    static const char *k[] = {
        "F$Link","F$Load","F$UnLink","F$Fork","F$Wait","F$Chain","F$Exit","F$Mem","F$Send",
        "F$Icpt","F$Sleep","F$SSpd","F$ID","F$SPrior","F$STrap","F$PErr","F$PrsNam","F$CmpNam",
        "F$SchBit","F$AllBit","F$DelBit","F$Time","F$STime","F$CRC","F$GPrDsc","F$GBlkMp",
        "F$GModDr","F$CpyMem","F$SUser","F$UnLoad","F$RTE","F$GPrDBT","F$Julian","F$TLink",
        "F$DFork","F$DExec","F$DExit","F$DatMod","F$SetCRC","F$SetSys","F$SRqMem","F$SRtMem"};
    static const char *io[] = {
        "I$Attach","I$Detach","I$Dup","I$Create","I$Open","I$MakDir","I$ChgDir","I$Delete",
        "I$Seek","I$Read","I$Write","I$ReadLn","I$WritLn","I$GetStt","I$SetStt","I$Close"};
    static char buf[16];
    if (c < (int)(sizeof k / sizeof *k)) return k[c];
    if (c >= 0x80 && c <= 0x8F) return io[c - 0x80];
    switch (c) {
    case 0x52: return "F$SysID"; case 0x53: return "F$Event"; case 0x56: return "F$Alarm";
    case 0x57: return "F$SigMask"; case 0x5C: return "F$SRqCMem";
    }
    snprintf(buf, sizeof buf, "F$%02x", c);
    return buf;
}

static int local_path(int lp)
{
    if (lp < 0 || lp >= NPATHS) return -1;
    return cur->paths[lp];
}

static int add_local_path(int sp)
{
    for (int i = 0; i < NPATHS; i++) if (cur->paths[i] < 0) { cur->paths[i] = sp; return i; }
    io_close(sp);
    return -1;
}

static char skip_mods[8][32];
static int nskip;
void os9_skip_module(const char *name) { if (nskip < 8) snprintf(skip_mods[nskip++], 32, "%s", name); }

/* Debug: byte written into cdi_launch.res when a skipped program "exits" (launcher state). */
static int res_off = -1, res_val;
void os9_skip_sets_res(int off, int val) { res_off = off; res_val = val; }

static void sys_fork(bool chain)
{
    char name[32];
    uint32_t end = read_modname(REG(A0), name);
    uint32_t addmem = REG(D1), plen = REG(D2), params = REG(A1);
    int npaths = REG(D3) & 0xFFFF, prio = REG(D4) & 0xFFFF;
    char pstr[128] = "";
    for (uint32_t i = 0; i < plen && i < sizeof pstr - 1; i++) {
        int c = rd8(params + i);
        pstr[i] = (char)(c >= 32 && c < 127 ? c : '.');
        pstr[i + 1] = 0;
    }
    ll_log(" \"%s\" params \"%s\" (%u) addmem %x paths %d prio %d\n", name, pstr, plen, addmem, npaths, prio);
    for (int k = 0; k < nskip; k++)
        if (!strcasecmp(skip_mods[k], name) && !chain) {
            /* Pretend the child ran and exited cleanly: create it, then kill it at once. */
            int i = mod_find(name), e2 = 0;
            if (i < 0) i = mod_load_file(name, &e2);
            if (i < 0) { err(e2); return; }
            cpu_save();
            int id = proc_create(i, cur->id, params, plen, 0, prio);
            cpu_restore();
            Proc *c = proc_by_id(id);
            ll_log("        --skip: %s exits immediately\n", name);
            int r = mod_find("cdi_launch.res");
            if (res_off >= 0 && r >= 0) {
                uint32_t d = mods[r].addr + rd32(mods[r].addr + 0x30);
                ll_log("        --res-byte: cdi_launch.res+%x = %d (was %d)\n", res_off, res_val, rd8(d + res_off));
                wr8(d + res_off, res_val);
            }
            proc_kill(c, 0);
            set_w(M68K_REG_D0, id); SETREG(A0, end); ok();
            return;
        }
    int idx = mod_find(name), e = 0;
    if (idx < 0) idx = mod_load_file(name, &e);
    else mods[idx].links++;
    if (idx < 0) { ll_log("        -> error %02x\n", e); err(e); return; }
    if (chain) {
        /* Replace the current process image, keeping its id and paths. */
        Proc *p = cur;
        int parent = p->parent;
        uint32_t tmp = heap_alloc(plen + 1);
        memcpy(host(tmp), host(params), plen);
        heap_free(p->mem, p->memmax);
        int old = p->mod;
        p->mod = idx;
        mod_unlink(old);
        snprintf(p->name, sizeof p->name, "%s", name);
        p->icpt_pc = 0; p->nsig = 0; p->in_icpt = false;
        p->parent = parent;
        memcpy(p->ctx, clean_ctx, m68k_context_size());
        proc_setup_regs(p, mods[idx].addr, tmp, plen, npaths);
        heap_free(tmp, plen + 1);
        m68k_set_context(p->ctx);
        m68k_end_timeslice();
        return;
    }
    cpu_save();
    int id = proc_create(idx, cur->id, params, plen, npaths, prio);
    cpu_restore();
    if (id < 0) { err(E_MEMFUL); return; }
    set_w(M68K_REG_D0, id);
    SETREG(A0, end);
    ok();
    ll_log("        -> child process %d\n", id);
}

static void sys_event(void)
{
    int fn = REG(D1) & 0xFFFF;
    int f = fn & 0x7FFF;
    char name[32];
    Event *e;
    switch (f) {
    case 0: case 2: {   /* Ev$Link, Ev$Creat */
        uint32_t end = read_modname(REG(A0), name);
        for (int i = 0; i < 32; i++)
            if (events[i].id && !strcasecmp(events[i].name, name)) {
                if (f == 2) { ll_log(" Ev$Creat \"%s\" exists\n", name); err(0xA4 /* E$EvntID? (verify) */); return; }
                events[i].links++;
                SETREG(D0, events[i].id); SETREG(A0, end); ok();
                ll_log(" Ev$Link \"%s\" -> %x\n", name, events[i].id);
                return;
            }
        if (f == 0) { ll_log(" Ev$Link \"%s\" not found\n", name); err(E_EVNF); return; }
        for (int i = 0; i < 32; i++)
            if (!events[i].id) {
                e = &events[i];
                e->id = next_event++; e->links = 1; snprintf(e->name, 32, "%s", name);
                e->value = (int32_t)REG(D0); e->winc = (int16_t)REG(D2); e->sinc = (int16_t)REG(D3);
                ll_log(" Ev$Creat \"%s\" value %d winc %d sinc %d -> %x\n", name, e->value, e->winc, e->sinc, e->id);
                SETREG(D0, e->id); SETREG(A0, end); ok();
                return;
            }
        err(E_MEMFUL); return;
    }
    }
    e = ev_by_id(REG(D0));
    if (!e) { ll_log(" Ev fn %d: bad id %x\n", fn, REG(D0)); err(E_EVNF); return; }
    switch (f) {
    case 1: ll_log(" Ev$UnLnk %s\n", e->name); e->links--; ok(); return;
    case 3: ll_log(" Ev$Delet %s\n", e->name); memset(e, 0, sizeof *e); ok(); return;
    case 4: case 5: {   /* Ev$Wait, Ev$WaitR */
        int32_t lo = (int32_t)REG(D2), hi = (int32_t)REG(D3);
        if (f == 5) { lo += e->value; hi += e->value; }
        ll_log(" Ev$Wait%s %s [%d..%d] value %d\n", f == 5 ? "R" : "", e->name, lo, hi, e->value);
        if (e->value >= lo && e->value <= hi) { SETREG(D1, e->value); e->value += e->winc; ok(); return; }
        cur->state = P_EVWAIT; cur->ev_id = e->id; cur->ev_min = lo; cur->ev_max = hi;
        m68k_end_timeslice();
        return;
    }
    case 6: ll_log(" Ev$Read %s = %d\n", e->name, e->value); SETREG(D1, e->value); ok(); return;
    case 7: ll_log(" Ev$Info %s\n", e->name); ok(); return;
    case 8: e->value += e->sinc; ll_log(" Ev$Signl%s %s -> %d\n", fn & 0x8000 ? "(all)" : "", e->name, e->value);
            ev_wake_waiters(e, fn & 0x8000); ok(); return;
    case 9: { int32_t old = e->value; e->value = (int32_t)REG(D2);
            ll_log(" Ev$Pulse %s %d\n", e->name, e->value);
            ev_wake_waiters(e, fn & 0x8000); e->value = old; ok(); return; }
    case 10: case 11:
        e->value = f == 10 ? (int32_t)REG(D2) : e->value + (int32_t)REG(D2);
        ll_log(" Ev$Set%s %s -> %d\n", f == 11 ? "R" : "", e->name, e->value);
        SETREG(D1, e->value);
        ev_wake_waiters(e, fn & 0x8000); ok(); return;
    }
    ll_log(" Ev fn %d unknown\n", fn);
    err(E_UNKSVC);
}

static void sys_alarm(void)
{
    int fn = REG(D1) & 0xFFFF;
    uint32_t t = REG(D3);
    switch (fn) {
    case 0: {  /* A$Delete: d0 = id, 0 = all for this process */
        int id = REG(D0);
        ll_log(" A$Delete %d\n", id);
        for (int i = 0; i < 32; i++)
            if (alarms[i].id && alarms[i].pid == cur->id && (!id || alarms[i].id == id)) alarms[i].id = 0;
        ok(); return;
    }
    case 1: case 2: {  /* A$Set (one shot), A$Cycle */
        uint64_t dt = (t & 0x80000000u) ? (uint64_t)(t & 0x7FFFFFFF) * TICKS_PER_SEC / 256 : t;
        if (!dt) dt = 1;
        for (int i = 0; i < 32; i++)
            if (!alarms[i].id) {
                alarms[i] = (Alarm){ next_alarm++, cur->id, REG(D2) & 0xFFFF, ticks + dt, fn == 2 ? dt : 0 };
                ll_log(" A$%s sig %d every %llu ticks -> id %d\n", fn == 2 ? "Cycle" : "Set",
                       alarms[i].sig, (unsigned long long)dt, alarms[i].id);
                SETREG(D0, alarms[i].id); ok(); return;
            }
        err(E_MEMFUL); return;
    }
    }
    ll_log(" A$ fn %d unsupported\n", fn);
    err(E_UNKSVC);
}

bool os9_trap(int trapno)
{
    uint32_t pc = REG(PC);
    int code = rd16(pc);
    SETREG(PC, pc + 2);
    if (trapno != 0) {
        ll_log("[%d] %06x TRAP #%d code %04x: unsupported\n", cur->id, pc - 2, trapno, code);
        ll_fatal("unsupported TRAP #%d at %06x in %s", trapno, pc - 2, cur->name);
    }
    ll_log("[%d %s] %06x %-9s", cur->id, cur->name, (pc - 2 - mods[cur->mod].addr), fname(code));
    char s[256];
    int e;
    switch (code) {
    case 0x00: {  /* F$Link */
        char name[32];
        uint32_t end = read_modname(REG(A0), name);
        int i = mod_find(name);
        ll_log(" \"%s\"", name);
        if (i < 0) { ll_log(" -> not found\n"); err(E_MNF); break; }
        mods[i].links++;
        uint32_t m = mods[i].addr;
        set_w(M68K_REG_D0, rd16(m + 0x12)); set_w(M68K_REG_D1, rd16(m + 0x14));
        SETREG(A0, end); SETREG(A1, m + rd32(m + 0x30)); SETREG(A2, m); ok();
        ll_log(" -> %06x\n", m);
        if (rd8(m + 0x12) == 4) {   /* data module: show the start of its data */
            uint32_t d = m + rd32(m + 0x30);
            ll_log("        data:");
            for (int k = 0; k < 0x48; k += 4) ll_log(" %08x", rd32(d + k));
            ll_log("\n");
        }
        break;
    }
    case 0x01: {  /* F$Load */
        char name[64];
        mem_cstr(REG(A0), name, sizeof name);
        uint32_t end = REG(A0) + mem_strlen(REG(A0), 64);
        ll_log(" \"%s\" mode %x\n", name, REG(D0) & 0xFF);
        int i = mod_load_file(name, &e);
        if (i < 0) { ll_log("        -> error %02x\n", e); err(e); break; }
        uint32_t m = mods[i].addr;
        set_w(M68K_REG_D0, rd16(m + 0x12)); set_w(M68K_REG_D1, rd16(m + 0x14));
        SETREG(A0, end); SETREG(A1, m + rd32(m + 0x30)); SETREG(A2, m); ok();
        break;
    }
    case 0x02: {  /* F$UnLink */
        uint32_t m = REG(A2);
        ll_log(" %06x\n", m);
        int i;
        for (i = 0; i < nmods; i++) if (mods[i].addr && m >= mods[i].addr && m < mods[i].addr + rd32(mods[i].addr + 4)) break;
        if (i == nmods) { ll_log("        -> not a module\n"); err(0xDD); break; }
        mod_unlink(i);
        ok();
        break;
    }
    case 0x1D: {  /* F$UnLoad */
        char name[32];
        read_modname(REG(A0), name);
        int i = mod_find(name);
        ll_log(" \"%s\"", name);
        if (i < 0) { ll_log(" -> not loaded\n"); err(E_MNF); break; }
        ll_log(" (links %d)\n", mods[i].links);
        mod_unlink(i);
        ok();
        break;
    }
    case 0x03: sys_fork(false); break;
    case 0x05: sys_fork(true); break;
    case 0x04: {  /* F$Wait */
        Proc *dead = NULL;
        bool any = false;
        for (int i = 0; i < MAXPROC; i++)
            if (procs[i].state != P_FREE && procs[i].parent == cur->id) {
                any = true;
                if (procs[i].state == P_DEAD) { dead = &procs[i]; break; }
            }
        if (!any) { ll_log(" -> no children\n"); err(E_NOCHLD); break; }
        if (dead) {
            ll_log(" -> child %d status %d\n", dead->id, dead->exit_status);
            set_w(M68K_REG_D0, dead->id); set_w(M68K_REG_D1, dead->exit_status); ok();
            dead->state = P_FREE;
            break;
        }
        ll_log(" (blocking)\n");
        cur->state = P_WAIT;
        m68k_end_timeslice();
        break;
    }
    case 0x06:  /* F$Exit */
        ll_log(" status %d\n", REG(D1) & 0xFFFF);
        m68k_get_context(cur->ctx);
        proc_kill(cur, REG(D1) & 0xFFFF);
        m68k_end_timeslice();
        break;
    case 0x07: {  /* F$Mem */
        uint32_t want = REG(D0);
        ll_log(" %x (have %x)", want, cur->memsize);
        if (want > cur->memmax) { ll_log(" -> no room\n"); err(E_MEMFUL); break; }
        if (want > cur->memsize) cur->memsize = (want + 15) & ~15u;
        SETREG(D0, cur->memsize); SETREG(A1, cur->mem + cur->memsize); ok();
        ll_log(" -> %x end %06x\n", cur->memsize, cur->mem + cur->memsize);
        break;
    }
    case 0x08: {  /* F$Send */
        int id = REG(D0) & 0xFFFF, sig = REG(D1) & 0xFFFF;
        ll_log(" pid %d signal %d\n", id, sig);
        Proc *p = proc_by_id(id);
        if (!p) { err(E_IPRCID); break; }
        send_signal(p, sig);
        ok();
        break;
    }
    case 0x09:  /* F$Icpt */
        cur->icpt_pc = REG(A0); cur->icpt_a6 = REG(A6);   /* a0 = 0 removes the routine */
        ll_log(" handler %06x a6 %06x\n", cur->icpt_pc, cur->icpt_a6);
        ok();
        break;
    case 0x0A: {  /* F$Sleep */
        uint32_t t = REG(D0);
        ll_log(" %u ticks%s\n", t & 0x7FFFFFFF, t ? "" : " (until signal)");
        if (cur->nsig) { SETREG(D0, t); ok(); break; }   /* handled on the way out */
        /* As the ROM kernel does it: 0 = until a signal, 1 = only give up the time slice (the
         * process stays ready), n = sleep n-1 ticks. cdi_space's fades yield with F$Sleep(1). */
        uint32_t n = t & 0x7FFFFFFF;
        if (t & 0x80000000u) n = (uint32_t)((uint64_t)n * TICKS_PER_SEC / 256) + 1;
        if (n == 1) { SETREG(D0, 0); ok(); m68k_end_timeslice(); break; }
        cur->state = P_SLEEP;
        cur->wake_tick = n ? ticks + (n - 1) : 0;
        SETREG(D0, 0); ok();
        m68k_end_timeslice();
        break;
    }
    case 0x27: {  /* F$SetSys: d0 = offset, d1 = size (bit 31 = read), d2 = value */
        uint32_t off = REG(D0), sz = REG(D1) & 0x7FFFFFFF;
        bool rd = REG(D1) & 0x80000000u;
        uint32_t v = 0;
        /* Only offset 0x7C is used (read, 4 bytes, once at launcher start). Serve the tick
         * count: a free-running value is the safe guess (verify which global it is). */
        if (off == 0x7C) v = (uint32_t)ticks;
        ll_log(" %s offset %x size %u -> %x\n", rd ? "read" : "write", off, sz, v);
        if (rd) SETREG(D2, v);
        ok();
        break;
    }
    case 0x0B: case 0x0D: case 0x1B: case 0x26: case 0x1A: case 0x18: case 0x1F:
    case 0x0F: case 0x52:
        /* Rare housekeeping calls: log the registers and succeed. Filled in once seen in a trace. */
        ll_log(" d0=%x d1=%x a0=%06x (stub)\n", REG(D0), REG(D1), REG(A0));
        if (code == 0x0F) { mem_cstr(REG(A0), s, sizeof s); }
        ok();
        break;
    case 0x0C:  /* F$ID */
        set_w(M68K_REG_D0, cur->id); SETREG(D1, 0); set_w(M68K_REG_D2, cur->prio);
        ll_log(" -> %d\n", cur->id); ok();
        break;
    case 0x10: {  /* F$PrsNam */
        uint32_t a = REG(A0);
        if (rd8(a) == '/') a++;
        uint32_t b = a;
        while (name_char(rd8(b))) b++;
        mem_cstr(a, s, (b - a + 1) < sizeof s ? b - a + 1 : sizeof s);
        ll_log(" \"%s\"\n", s);
        if (b == a) { err(E_BPNAM); break; }
        set_w(M68K_REG_D0, rd8(b)); set_w(M68K_REG_D1, b - a); SETREG(A0, a); SETREG(A1, b); ok();
        break;
    }
    case 0x11: {  /* F$CmpNam */
        char pat[64], tgt[64];
        int n = REG(D1) & 0xFFFF;
        for (int i = 0; i < n && i < 63; i++) pat[i] = (char)rd8(REG(A0) + i);
        pat[n < 63 ? n : 63] = 0;
        uint32_t t = REG(A1); int k = 0;
        while (name_char(rd8(t)) && k < 63) tgt[k++] = (char)rd8(t++);
        tgt[k] = 0;
        bool m = name_match(pat, tgt);
        ll_log(" \"%s\" vs \"%s\" -> %s\n", pat, tgt, m ? "match" : "no");
        if (m) ok(); else err(0xEB /* E$Differ */);
        break;
    }
    case 0x15: {  /* F$Time */
        int fmt = REG(D0) & 0xFFFF;
        time_t now = time(NULL);
        struct tm tm; ll_localtime(&now, &tm);
        uint32_t secs = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
        if (fmt & 1) {  /* Julian */
            int y = tm.tm_year + 1900, mth = tm.tm_mon + 1, d = tm.tm_mday;
            int a = (14 - mth) / 12, yy = y + 4800 - a, mm = mth + 12 * a - 3;
            uint32_t jdn = d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
            SETREG(D0, secs); SETREG(D1, jdn);
        } else {
            SETREG(D0, tm.tm_hour << 16 | tm.tm_min << 8 | tm.tm_sec);
            SETREG(D1, (tm.tm_year + 1900) << 16 | (tm.tm_mon + 1) << 8 | tm.tm_mday);
        }
        set_w(M68K_REG_D2, tm.tm_wday);
        SETREG(D3, TICKS_PER_SEC << 16 | (uint32_t)(ticks % TICKS_PER_SEC));
        ll_log(" fmt %d -> d0=%x d1=%x tick %llu\n", fmt, REG(D0), REG(D1), (unsigned long long)ticks);
        ok();
        break;
    }
    case 0x17: {  /* F$CRC */
        uint32_t crc = os9_crc(REG(D1), REG(A0), REG(D0));
        ll_log(" %06x len %x -> %06x\n", REG(A0), REG(D0), crc);
        SETREG(D1, crc); ok();
        break;
    }
    case 0x1E: {  /* F$RTE: return from intercept */
        ll_log("\n");
        if (!cur->in_icpt) { err(E_UNKSVC); break; }
        cur->in_icpt = false;
        m68k_set_context(cur->sig_ctx);
        m68k_end_timeslice();   /* let the scheduler deliver the next queued signal */
        break;
    }
    case 0x25: {  /* F$DatMod */
        char name[32];
        uint32_t end = read_modname(REG(A0), name);
        uint32_t size = REG(D0);
        ll_log(" \"%s\" size %x attr %x perm %x type %x\n", name, size, REG(D1) & 0xFFFF, REG(D2) & 0xFFFF, REG(D3) & 0xFFFF);
        if (mod_find(name) >= 0) { ll_log("        -> exists\n"); err(0xE7 /* E$KwnMod (verify) */); break; }
        uint32_t nl = strlen(name) + 1;
        uint32_t doff = (0x34 + nl + 3) & ~3u;
        uint32_t msize = doff + ((size + 3) & ~3u) + 4;
        uint32_t m = modarea_alloc(msize);
        if (!m) { err(E_MEMFUL); break; }
        wr16(m, 0x4AFC); wr16(m + 2, 1); wr32(m + 4, msize); wr32(m + 0x0C, 0x34);
        wr16(m + 0x10, REG(D2)); wr8(m + 0x12, 4); wr8(m + 0x13, REG(D3));   /* type Data, language from d3 */ wr16(m + 0x14, REG(D1));
        wr32(m + 0x30, doff);
        memcpy(host(m + 0x34), name, nl);
        mod_fix_parity(m);
        int i = mod_register(m);
        mods[i].links = 1;
        set_w(M68K_REG_D0, rd16(m + 0x12)); set_w(M68K_REG_D1, rd16(m + 0x14));
        SETREG(A0, end); SETREG(A1, m + doff); SETREG(A2, m); ok();
        ll_log("        -> module %06x data %06x\n", m, m + doff);
        break;
    }
    case 0x28: {  /* F$SRqMem */
        uint32_t size = REG(D0);
        if (size == 0xFFFFFFFFu) size = 0x100000;
        uint32_t a = heap_alloc(size);
        ll_log(" %x -> %06x\n", size, a);
        if (!a) { err(E_MEMFUL); break; }
        SETREG(D0, (size + 15) & ~15u); SETREG(A2, a); ok();
        break;
    }
    case 0x29:  /* F$SRtMem */
        ll_log(" %06x size %x\n", REG(A2), REG(D0));
        heap_free(REG(A2), REG(D0)); ok();
        break;
    case 0x53: sys_event(); break;
    case 0x56: sys_alarm(); break;
    case 0x5C: {  /* F$SRqCMem */
        uint32_t size = REG(D0), color = REG(D1);
        /* Colour 0x80 / 0x81 = video RAM bank of plane A / B (verify order). 0 = any memory. */
        uint32_t a = cmem_alloc(color == 0x80 ? 1 : color == 0x81 ? 2 : 0, size);
        if (!a && color) { ll_log(" (bank full, using general RAM)"); a = heap_alloc(size); }
        ll_log(" %x colour %d -> %06x\n", size, color, a);
        if (!a) { err(E_MEMFUL); break; }
        SETREG(D0, (size + 15) & ~15u); SETREG(A2, a); ok();
        break;
    }

    /* ---- I/O ---- */
    case 0x82: {  /* I$Dup */
        int sp = local_path(REG(D0) & 0xFFFF), np;
        ll_log(" path %d\n", REG(D0) & 0xFFFF);
        if (sp < 0 || io_dup(sp, &np)) { err(E_BPNUM); break; }
        set_w(M68K_REG_D0, add_local_path(np)); ok();
        break;
    }
    case 0x83: case 0x84: {  /* I$Create, I$Open */
        int sp; uint32_t end;
        mem_cstr(REG(A0), s, 128);
        ll_log(" \"%s\" mode %x", s, REG(D0) & 0xFF);
        e = code == 0x84 ? io_open(REG(A0), REG(D0) & 0xFF, &sp, &end)
                         : io_create(REG(A0), REG(D0) & 0xFF, REG(D1), &sp, &end);
        if (e) { ll_log(" -> error %02x\n", e); err(e); break; }
        int lp = add_local_path(sp);
        set_w(M68K_REG_D0, lp); SETREG(A0, end); ok();
        ll_log(" -> path %d\n", lp);
        break;
    }
    case 0x85: mem_cstr(REG(A0), s, 128); ll_log(" \"%s\"\n", s); e = io_makdir(REG(A0)); if (e) err(e); else ok(); break;
    case 0x87: mem_cstr(REG(A0), s, 128); ll_log(" \"%s\"\n", s); e = io_delete(REG(A0)); if (e) err(e); else ok(); break;
    case 0x86: mem_cstr(REG(A0), s, 128); ll_log(" \"%s\" (ignored)\n", s); ok(); break;
    case 0x88: {  /* I$Seek */
        int lp = REG(D0) & 0xFFFF;
        ll_log(" path %d pos %x\n", lp, REG(D1));
        e = io_seek(local_path(lp), REG(D1));
        if (e) err(e); else ok();
        break;
    }
    case 0x89: case 0x8A: case 0x8B: case 0x8C: {  /* Read, Write, ReadLn, WritLn */
        int lp = REG(D0) & 0xFFFF;
        uint32_t done = 0;
        bool wr = code == 0x8A || code == 0x8C, line = code == 0x8B || code == 0x8C;
        ll_log(" path %d buf %06x count %x", lp, REG(A0), REG(D1));
        int sp = local_path(lp);
        e = sp < 0 ? E_BPNUM : wr ? io_write(sp, REG(A0), REG(D1), &done, line)
                                  : io_read(sp, REG(A0), REG(D1), &done, line);
        if (e) { ll_log(" -> error %02x\n", e); err(e); break; }
        SETREG(D1, done); ok();
        ll_log(" -> %x\n", done);
        break;
    }
    case 0x8D: case 0x8E: {  /* I$GetStt, I$SetStt */
        int lp = REG(D0) & 0xFFFF, st = REG(D1) & 0xFFFF;
        int sp = local_path(lp);
        ll_log(" path %d code %02x d2=%x d3=%x d4=%x a0=%06x a1=%06x\n", lp, st, REG(D2), REG(D3), REG(D4), REG(A0), REG(A1));
        if (sp < 0) { err(E_BPNUM); break; }
        e = code == 0x8D ? io_getstt(sp, st) : io_setstt(sp, st);
        if (e) { ll_log("        -> error %02x\n", e); err(e); } else ok();
        break;
    }
    case 0x8F: {  /* I$Close */
        int lp = REG(D0) & 0xFFFF;
        ll_log(" path %d\n", lp);
        int sp = local_path(lp);
        if (sp < 0) { err(E_BPNUM); break; }
        io_close(sp); cur->paths[lp] = -1; ok();
        break;
    }
    default:
        ll_log(" d0=%x d1=%x d2=%x a0=%06x UNIMPLEMENTED\n", REG(D0), REG(D1), REG(D2), REG(A0));
        err(E_UNKSVC);
        break;
    }
    /* OS-9 processes pending signals on the way out of a system call. */
    if (cur && cur->state == P_READY && cur->nsig && !cur->in_icpt) m68k_end_timeslice();
    return true;
}

/* ------------------------------------------------------------------ */
/* Boot and scheduler                                                  */

void os9_init(void)
{
    for (int i = 0; i < MAXPROC; i++) { free(procs[i].ctx); free(procs[i].sig_ctx); }
    memset(procs, 0, sizeof procs);
    memset(mods, 0, sizeof mods); nmods = 0;
    memset(alarms, 0, sizeof alarms);
    memset(events, 0, sizeof events);
    cdfm_reset();
    memset(vbl_sig, 0, sizeof vbl_sig);
    ticks = 0; total_cycles = 0; last_exit = -1; cur = NULL; next_field_cycle = 0;
    clean_ctx = malloc(m68k_context_size());
    m68k_get_context(clean_ctx);
    line_event = &events[0];
    *line_event = (Event){ next_event++, 1, "line_event", 0, 0, 1 };
    /* Vectors: point every exception at a trap that halts with a message (see cpu.c). */
}

int os9_install_module(const uint8_t *img, uint32_t len)
{
    uint32_t m = modarea_alloc(len);
    if (!m) return -1;
    memcpy(host(m), img, len);
    int i = mod_register(m);
    mods[i].links = 1;
    return i;
}

bool os9_boot(const char *module_name)
{
    int e = 0, idx = mod_find(module_name);
    if (idx >= 0) mods[idx].links++;
    else idx = mod_load_file(module_name, &e);
    if (idx < 0) { fprintf(stderr, "cannot load %s: error %02x\n", module_name, e); return false; }
    static const uint8_t none[1] = { 0 };
    (void)none;
    uint32_t params = heap_alloc(16);
    wr8(params, '\r');   /* OS-9 parameter strings end with CR */
    int id = proc_create(idx, 0, params, 1, 3, 128);
    return id > 0;
}

/* PC histogram (sampled after each timeslice) to find busy-wait loops. */
static struct { uint32_t pc; int pid; unsigned n; } pchist[256];
static void pchist_add(uint32_t pc, int pid)
{
    unsigned h = (pc * 2654435761u) >> 24;
    for (unsigned k = 0; k < 256; k++) {
        unsigned i = (h + k) & 255;
        if (!pchist[i].n) { pchist[i].pc = pc; pchist[i].pid = pid; }
        if (pchist[i].pc == pc) { pchist[i].n++; return; }
    }
}
static void pchist_reset(void) { memset(pchist, 0, sizeof pchist); }

static void pchist_dump(void)
{
    for (int r = 0; r < 12; r++) {
        int best = -1;
        for (int i = 0; i < 256; i++) if (pchist[i].n && (best < 0 || pchist[i].n > pchist[best].n)) best = i;
        if (best < 0) break;
        Proc *p = proc_by_id(pchist[best].pid);
        uint32_t base = p && p->mod >= 0 ? mods[p->mod].addr : 0;
        ll_log("    pc %06x (%s+%05x) x%u\n", pchist[best].pc, p ? p->name : "?", pchist[best].pc - base, pchist[best].n);
        pchist[best].n = 0;
    }
}

/* Game speed: while cdi_tribes runs, the kernel clock (alarms, sleeps) can run slower than real
 * time, which slows the game logic without touching disc streaming or the display.
 * ll_game_speed is a percentage. (The real ROM's clock is 100 ticks/s: sgstom sets D_TckSec = 100
 * and tim070driv programs the 68070 timer to ~100 Hz, so 100 % is what the code asks for.)
 * The original pace comes from charging the display driver's cost (video.c, ll_ucm_*); this
 * factor is only an optional extra slowdown. */
int ll_game_speed = 100;
static int gt_speed = 100;
static uint64_t gt_base_cycles, gt_base_ticks, cd_ticks;

static uint64_t game_tick_cycles(void) { return CYCLES_PER_TICK * 100 / (uint64_t)gt_speed; }
static uint64_t tick_at(uint64_t c) { return c < gt_base_cycles ? gt_base_ticks : gt_base_ticks + (c - gt_base_cycles) / game_tick_cycles(); }
static uint64_t cycle_of_tick(uint64_t t) { return t < gt_base_ticks ? gt_base_cycles : gt_base_cycles + (t - gt_base_ticks) * game_tick_cycles(); }

/* Kernel ticks (alarms, sleeps), CD play and display fields run off the cycle counter. */
static void advance_time(void)
{
    /* only the planets (cdi_tribes): cdi_space's intro and cutscenes are paced against the disc */
    int sp = ll_game_speed < 25 ? 25 : ll_game_speed > 100 ? 100 : ll_game_speed;
    if (sp != 100 && !os9_process_alive("cdi_tribes")) sp = 100;
    if (sp != gt_speed) { gt_base_ticks = ticks; gt_base_cycles = total_cycles; gt_speed = sp; }
    uint64_t c = total_cycles / CYCLES_PER_TICK;
    while (cd_ticks < c) { cd_ticks++; cdfm_tick(TICKS_PER_SEC); }
    uint64_t t = tick_at(total_cycles);
    while (ticks < t) { ticks++; check_alarms(); }
    if (!next_field_cycle) next_field_cycle = ll_cpu_hz / ll_field_hz;
    while (total_cycles >= next_field_cycle) {
        field_interrupt();
        next_field_cycle += ll_cpu_hz / ll_field_hz;
    }
}

/* Run the scheduler until the cycle counter reaches `until`. Returns 1 while processes are
 * alive, 0 when all have exited, -2 on deadlock. */
static int rr;
int os9_run_until(uint64_t until)
{
    while (total_cycles < until) {
        /* advance the clock, fire alarms, wake sleepers */
        advance_time();
        Proc *next = NULL;
        bool alive = false, sleeping = false;
        for (int k = 0; k < MAXPROC; k++) {
            Proc *p = &procs[(rr + k) % MAXPROC];
            if (p->state == P_SLEEP && p->wake_tick && p->wake_tick <= ticks) p->state = P_READY;
            if (p->state == P_SLEEP && p->nsig) p->state = P_READY;
            if (p->state != P_FREE && p->state != P_DEAD) alive = true;
            if (p->state == P_SLEEP || p->state == P_EVWAIT) sleeping = true;
            if (!next && p->state == P_READY) { next = p; rr = (int)(p - procs) + 1; }
        }
        if (!alive) return 0;
        if (!next) {
            /* Everyone is blocked: jump the clock to the next timed wakeup or alarm. */
            uint64_t soon = UINT64_MAX;
            for (int k = 0; k < MAXPROC; k++)
                if (procs[k].state == P_SLEEP && procs[k].wake_tick) soon = procs[k].wake_tick < soon ? procs[k].wake_tick : soon;
            for (int k = 0; k < 32; k++) if (alarms[k].id && alarms[k].at < soon) soon = alarms[k].at;
            uint64_t soon_c = soon == UINT64_MAX ? UINT64_MAX : cycle_of_tick(soon);
            if (cdfm_busy() && (cd_ticks + 1) * CYCLES_PER_TICK < soon_c) soon_c = (cd_ticks + 1) * CYCLES_PER_TICK;
            for (int k = 0; k < MAXPROC; k++)
                if ((procs[k].state == P_EVWAIT && line_event && procs[k].ev_id == line_event->id) ||
                    (vbl_sig[k] && procs[k].state == P_SLEEP))
                    if (next_field_cycle < soon_c) soon_c = next_field_cycle;
            if (soon_c == UINT64_MAX) {
                ll_log("*** deadlock: all processes blocked with nothing pending (sleeping=%d)\n", sleeping);
                for (int k = 0; k < MAXPROC; k++)
                    if (procs[k].state != P_FREE)
                        ll_log("    process %d %-14s state %d ev %x\n", procs[k].id, procs[k].name, procs[k].state, procs[k].ev_id);
                return -2;
            }
            total_cycles = soon_c > total_cycles ? soon_c : total_cycles + 1;
            if (total_cycles > until) { total_cycles = until; break; }
            continue;
        }
        cur = next;
        m68k_set_context(cur->ctx);
        deliver_signals(cur);
        if (cur->state != P_READY) continue;
        int ran = m68k_execute(CYCLES_PER_TICK / 4);
        total_cycles += ran > 0 ? (uint64_t)ran : 1;
        if (cur->state != P_FREE && cur->state != P_DEAD) {
            m68k_get_context(cur->ctx);
            pchist_add(REG(PC), cur->id);
        }
        /* free orphaned zombies */
        for (int k = 0; k < MAXPROC; k++)
            if (procs[k].state == P_DEAD && !proc_by_id(procs[k].parent)) procs[k].state = P_FREE;
    }
    advance_time();
    return 1;
}

uint64_t os9_cycles(void) { return total_cycles; }

/* Charge CPU time to the running process for work the ROM would do on the 68070 (system calls
 * we implement natively): the cycles come out of the current Musashi time slice. */
extern int m68ki_remaining_cycles;
void os9_charge(int cycles) { m68ki_remaining_cycles -= cycles; }
int os9_last_exit(void) { return last_exit; }

int os9_run(uint64_t max_cycles)
{
    int r = os9_run_until(max_cycles);
    if (r == 0) return last_exit;
    if (r == -2) return -2;
    {
        ll_log("*** instruction budget exhausted at tick %llu; hottest PCs:\n", (unsigned long long)ticks);
        pchist_dump();
    }
    return last_exit;
}

/* Diagnostics for cpu.c */
const char *os9_cur_name(void) { return cur ? cur->name : "?"; }
uint32_t os9_cur_module(void) { return cur && cur->mod >= 0 ? mods[cur->mod].addr : 0; }
