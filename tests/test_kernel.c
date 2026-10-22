/* Kernel and file-layer tests for the Laser Lords runner.
 * Small 68000 programs are hand-assembled into OS-9 modules and run through the real
 * scheduler; their F$Exit status is the test result. Disc tests run when LL_BIN points at
 * the extracted ll.bin. */
#include "ll.h"
#include "m68k.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void cpu_init(void);

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- a minimal assembler: emit words, patch PC-relative displacements ---- */
typedef struct { uint8_t b[1024]; int n; } Code;
static void w16(Code *c, int v) { c->b[c->n++] = (uint8_t)(v >> 8); c->b[c->n++] = (uint8_t)v; }
static void w32(Code *c, uint32_t v) { w16(c, v >> 16); w16(c, v & 0xFFFF); }
static void moveq(Code *c, int reg, int v) { w16(c, 0x7000 | reg << 9 | (v & 0xFF)); }
static void sys(Code *c, int fn) { w16(c, 0x4E40); w16(c, fn); }
static int  lea_pc_a0(Code *c) { w16(c, 0x41FA); w16(c, 0); return c->n - 2; }  /* returns patch slot */
static void patch_pc(Code *c, int slot, int target) { int d = target - slot; c->b[slot] = (uint8_t)(d >> 8); c->b[slot + 1] = (uint8_t)d; }
static void str(Code *c, const char *s) { size_t l = strlen(s) + 1; memcpy(c->b + c->n, s, l); c->n += (int)l; if (c->n & 1) c->n++; }

enum { F_LINK = 0, F_FORK = 3, F_WAIT = 4, F_EXIT = 6, F_SEND = 8, F_ICPT = 9, F_SLEEP = 0x0A, F_ID = 0x0C,
       F_RTE = 0x1E, F_DATMOD = 0x25, F_SRQMEM = 0x28, F_SRTMEM = 0x29, F_EVENT = 0x53, F_ALARM = 0x56 };

/* Store D1.w at -$7F00(A6) / load it back: the intercept routine's way to report. */
static void store_d1(Code *c) { w16(c, 0x3D41); w16(c, 0x8100); }
static void load_d1(Code *c)  { w16(c, 0x322E); w16(c, 0x8100); }

/* Wrap code into an OS-9 program module and install it. */
static void install(const char *name, Code *c)
{
    uint8_t m[1200] = { 0 };
    uint32_t code_off = 0x48, name_off = code_off + c->n;
    uint32_t size = (name_off + (uint32_t)strlen(name) + 1 + 3 + 1) & ~1u;
    m[0] = 0x4A; m[1] = 0xFC; m[3] = 1;
    m[4] = size >> 24; m[5] = size >> 16; m[6] = size >> 8; m[7] = (uint8_t)size;
    m[0x0E] = name_off >> 8; m[0x0F] = (uint8_t)name_off;
    m[0x10] = 0x05; m[0x11] = 0x55; m[0x12] = 1; m[0x13] = 1; m[0x14] = 0x80; m[0x15] = 1;
    m[0x33] = (uint8_t)code_off;                       /* M$Exec */
    m[0x3A] = 0x01;                                    /* M$Mem = 0x100 bytes static */
    m[0x3E] = 0x10;                                    /* M$Stack = 0x1000 */
    memcpy(m + code_off, c->b, c->n);
    memcpy(m + name_off, name, strlen(name) + 1);
    uint32_t p = 0;
    for (int i = 0; i < 0x2E; i += 2) p ^= (uint32_t)(m[i] << 8 | m[i + 1]);
    p = ~p & 0xFFFF; m[0x2E] = p >> 8; m[0x2F] = (uint8_t)p;
    if (os9_install_module(m, size) < 0) { printf("install failed\n"); exit(1); }
}

static void reset(void)
{
    if (getenv("LL_TRACE")) ll_trace = stderr;
    mem_init();
    cpu_init();
    io_init();
    os9_init();
}

static int run(const char *boot)
{
    if (!os9_boot(boot)) return -100;
    return os9_run(15000000ull * 5);   /* 5 emulated seconds */
}

/* ---- tests ---- */

static void test_fork_wait_exit(void)
{
    reset();
    Code kid = { 0 };
    moveq(&kid, 1, 7); sys(&kid, F_EXIT);
    install("kid", &kid);

    Code par = { 0 };
    int slot = lea_pc_a0(&par);
    moveq(&par, 0, 0); moveq(&par, 1, 0); moveq(&par, 2, 0); moveq(&par, 3, 0); moveq(&par, 4, 0);
    sys(&par, F_FORK);
    sys(&par, F_WAIT);          /* d1 = child's status */
    sys(&par, F_EXIT);
    patch_pc(&par, slot, par.n); str(&par, "kid");
    install("par", &par);
    CHECK(run("par") == 7, "parent should exit with the child's status 7");
}

static void test_wait_no_children(void)
{
    reset();
    Code c = { 0 };
    sys(&c, F_WAIT);            /* no children: carry set, d1 = E$NoChld */
    sys(&c, F_EXIT);
    install("lonely", &c);
    CHECK(run("lonely") == E_NOCHLD, "F$Wait without children returns E$NoChld");
}

static void test_signal_intercept(void)
{
    reset();
    Code c = { 0 };
    int slot = lea_pc_a0(&c);
    sys(&c, F_ICPT);
    sys(&c, F_ID);                              /* d0 = own pid */
    w16(&c, 0x323C); w16(&c, 300);              /* move.w #300,d1 */
    sys(&c, F_SEND);                            /* handled on the way out of F$Send */
    load_d1(&c); sys(&c, F_EXIT);
    patch_pc(&c, slot, c.n);
    store_d1(&c); sys(&c, F_RTE);               /* handler */
    install("sig", &c);
    CHECK(run("sig") == 300, "intercept routine sees signal 300 and F$RTE resumes");
}

static void test_signal_without_intercept_kills(void)
{
    reset();
    Code c = { 0 };
    sys(&c, F_ID);
    w16(&c, 0x323C); w16(&c, 123);
    sys(&c, F_SEND);
    moveq(&c, 0, 0); sys(&c, F_SLEEP);
    moveq(&c, 1, 99); sys(&c, F_EXIT);          /* must not get here */
    install("victim", &c);
    CHECK(run("victim") == 123, "an unhandled signal kills the process with that status");
}

static void test_alarm(void)
{
    reset();
    Code c = { 0 };
    int slot = lea_pc_a0(&c);
    sys(&c, F_ICPT);
    moveq(&c, 0, 0); moveq(&c, 1, 1); moveq(&c, 2, 77); moveq(&c, 3, 10);   /* A$Set, sig 77, 10 ticks */
    sys(&c, F_ALARM);
    moveq(&c, 0, 0); sys(&c, F_SLEEP);
    load_d1(&c); sys(&c, F_EXIT);
    patch_pc(&c, slot, c.n);
    store_d1(&c); sys(&c, F_RTE);
    install("alarm", &c);
    CHECK(run("alarm") == 77, "A$Set alarm delivers its signal");
}

static void test_sleep_timed(void)
{
    reset();
    Code c = { 0 };
    moveq(&c, 0, 20); sys(&c, F_SLEEP);         /* 20 ticks, no signal: returns d0 = 0 */
    w16(&c, 0x2200);                            /* move.l d0,d1 */
    sys(&c, F_EXIT);
    install("nap", &c);
    CHECK(run("nap") == 0, "timed sleep runs out with no ticks left");
}

static void test_event_local(void)
{
    reset();
    Code c = { 0 };
    int slot = lea_pc_a0(&c);
    moveq(&c, 0, 0); moveq(&c, 1, 2); moveq(&c, 2, 0); moveq(&c, 3, 1);     /* Ev$Creat, sinc 1 */
    sys(&c, F_EVENT);
    w16(&c, 0x2E00);                            /* move.l d0,d7 */
    moveq(&c, 1, 8); sys(&c, F_EVENT);          /* Ev$Signl -> 1 */
    moveq(&c, 1, 8); sys(&c, F_EVENT);          /* Ev$Signl -> 2 */
    w16(&c, 0x2007);                            /* move.l d7,d0 */
    moveq(&c, 1, 6); sys(&c, F_EVENT);          /* Ev$Read -> d1 */
    sys(&c, F_EXIT);
    patch_pc(&c, slot, c.n); str(&c, "testev");
    install("ev", &c);
    CHECK(run("ev") == 2, "Ev$Signl adds the signal increment");
}

static void test_event_wait_across_processes(void)
{
    reset();
    Code kid = { 0 };
    int ks = lea_pc_a0(&kid);
    moveq(&kid, 1, 0); sys(&kid, F_EVENT);      /* Ev$Link */
    w16(&kid, 0x2E00);
    moveq(&kid, 0, 5); sys(&kid, F_SLEEP);      /* let the parent block first */
    w16(&kid, 0x2007);
    moveq(&kid, 1, 8); sys(&kid, F_EVENT);      /* Ev$Signl wakes the parent */
    moveq(&kid, 1, 0); sys(&kid, F_EXIT);
    patch_pc(&kid, ks, kid.n); str(&kid, "door");
    install("knock", &kid);

    Code par = { 0 };
    int s1 = lea_pc_a0(&par);
    moveq(&par, 0, 0); moveq(&par, 1, 2); moveq(&par, 2, 0); moveq(&par, 3, 1);
    sys(&par, F_EVENT);                         /* Ev$Creat "door" */
    w16(&par, 0x2E00);
    int s2 = lea_pc_a0(&par);
    moveq(&par, 0, 0); moveq(&par, 1, 0); moveq(&par, 2, 0); moveq(&par, 3, 0); moveq(&par, 4, 0);
    sys(&par, F_FORK);
    w16(&par, 0x2007);
    moveq(&par, 1, 4); moveq(&par, 2, 1); moveq(&par, 3, 1);
    sys(&par, F_EVENT);                         /* Ev$Wait [1..1]: blocks until the child signals */
    w16(&par, 0x2C01);                          /* move.l d1,d6 */
    sys(&par, F_WAIT);
    w16(&par, 0x2206);                          /* move.l d6,d1 */
    w16(&par, 0x5281);                          /* addq.l #1,d1 -> exit 2 when value was 1 */
    sys(&par, F_EXIT);
    patch_pc(&par, s1, par.n); str(&par, "door");
    patch_pc(&par, s2, par.n); str(&par, "knock");
    install("wait", &par);
    CHECK(run("wait") == 2, "Ev$Wait blocks until another process signals the event");
}

static void test_memory(void)
{
    reset();
    Code c = { 0 };
    w16(&c, 0x203C); w32(&c, 0x4000);           /* move.l #$4000,d0 */
    sys(&c, F_SRQMEM);
    w16(&c, 0x6500 | 10);                       /* bcs.s fail */
    w16(&c, 0x203C); w32(&c, 0x4000);
    sys(&c, F_SRTMEM);
    moveq(&c, 1, 0); sys(&c, F_EXIT);
    moveq(&c, 1, 1); sys(&c, F_EXIT);           /* fail: */
    install("mem", &c);
    CHECK(run("mem") == 0, "F$SRqMem / F$SRtMem round trip");

    /* allocator: freed blocks are reused and coalesced */
    reset();
    uint32_t a = heap_alloc(0x1000), b = heap_alloc(0x1000), d = heap_alloc(0x1000);
    heap_free(a, 0x1000); heap_free(b, 0x1000);
    uint32_t e = heap_alloc(0x2000);
    CHECK(e == a, "two adjacent freed blocks coalesce (got %06x, want %06x)", e, a);
    heap_free(d, 0x1000); heap_free(e, 0x2000);
    CHECK(cmem_alloc(1, 0x1000) >= CMEM_A_BASE && cmem_alloc(2, 0x1000) >= CMEM_B_BASE, "coloured memory banks");
}

static void test_datmod_and_link(void)
{
    reset();
    Code c = { 0 };
    int s1 = lea_pc_a0(&c);
    w16(&c, 0x203C); w32(&c, 0x100);            /* size */
    w16(&c, 0x223C); w32(&c, 0x8000);           /* move.l #$8000,d1 attr/rev */
    w16(&c, 0x243C); w32(&c, 0x1FF);            /* move.l #$1ff,d2 perms */
    moveq(&c, 3, 0x04); moveq(&c, 4, 0);
    sys(&c, F_DATMOD);
    w16(&c, 0x6500 | 0x1C);                     /* bcs.s fail */
    w16(&c, 0x12BC); w16(&c, 0x002A);           /* move.b #42,(a1): write into the data area */
    int s2 = lea_pc_a0(&c);
    moveq(&c, 0, 0);
    sys(&c, F_LINK);                            /* link it back: a2 = module, a1 = exec entry */
    w16(&c, 0x6500 | 0x0C);
    w16(&c, 0xD5EA); w16(&c, 0x0030);           /* adda.l $30(a2),a2 -> data area */
    moveq(&c, 1, 0);
    w16(&c, 0x1212);                            /* move.b (a2),d1 */
    sys(&c, F_EXIT);
    moveq(&c, 1, 1); sys(&c, F_EXIT);           /* fail: */
    patch_pc(&c, s1, c.n); str(&c, "shared");
    patch_pc(&c, s2, c.n); str(&c, "shared");
    install("dm", &c);
    CHECK(run("dm") == 42, "F$DatMod creates a module F$Link can find, data area shared");
}

static void test_nvram_files(void)
{
    reset();
    const char *dir = "test_nvram.tmp";
    io_set_nvram_dir(dir);
    io_init();
    remove("test_nvram.tmp/save.nvri");
    uint32_t name = heap_alloc(64), buf = heap_alloc(256), end;
    strcpy((char *)host(name), "/nvr/save.nvri");
    int p, q;
    uint32_t done;
    CHECK(io_create(name, 3, 0, &p, &end) == 0, "create NVRAM file");
    CHECK(io_create(name, 3, 0, &q, &end) == E_CEF, "second create reports E$CEF");
    memcpy(host(buf), "LASERLORDS-SAVE", 15);
    CHECK(io_write(p, buf, 15, &done, false) == 0 && done == 15, "write 15 bytes");
    io_seek(p, 5);
    memset(host(buf), 0, 32);
    CHECK(io_read(p, buf, 100, &done, false) == 0 && done == 10 && !memcmp(host(buf), "LORDS-SAVE", 10), "seek + read back");
    CHECK(io_read(p, buf, 100, &done, false) == E_EOF, "read at end is E$EOF");
    io_close(p);

    /* directory listing in RBF format */
    strcpy((char *)host(name), "/nvr");
    CHECK(io_open(name, 0x81, &p, &end) == 0, "open /nvr as a directory");
    bool found = false;
    for (;;) {
        if (io_read(p, buf, 32, &done, false) || done != 32) break;
        char n[32] = { 0 };
        for (int k = 0; k < 28; k++) { n[k] = (char)(rd8(buf + k) & 0x7F); if (rd8(buf + k) & 0x80) break; }
        if (!strcmp(n, "save.nvri")) found = true;
    }
    CHECK(found, "directory lists save.nvri");
    m68k_set_reg(M68K_REG_D2, 0);
    CHECK(io_getstt(p, 0x43) == 0, "free space query");
    uint32_t freeb = m68k_get_reg(NULL, M68K_REG_D2);
    CHECK(freeb > 7000 && freeb < 8192, "free space reflects usage (%u)", freeb);
    m68k_set_reg(M68K_REG_A0, buf);
    io_getstt(p, 0x00);
    CHECK(rd8(buf) == 1, "SS_Opt device type of /nvr is RBF");
    io_close(p);

    strcpy((char *)host(name), "/nvr/save.nvri");
    CHECK(io_open(name, 1, &p, &end) == 0, "reopen file");
    m68k_set_reg(M68K_REG_A0, buf); m68k_set_reg(M68K_REG_D2, 16);
    CHECK(io_getstt(p, 0x0F) == 0 && rd32(buf + 9) == 15, "SS_FD reports size 15");
    io_close(p);

    strcpy((char *)host(name), "/nvr/csd");
    CHECK(io_open(name, 1, &p, &end) == 0, "built-in CSD is readable");
    CHECK(io_read(p, buf, 256, &done, true) == 0 && !memcmp(host(buf), "2:/audio:\r", 10), "ReadLn returns one CSD record");
    io_close(p);

    strcpy((char *)host(name), "/nvr/save.nvri");
    CHECK(io_delete(name) == 0, "delete");
    CHECK(io_open(name, 1, &p, &end) == E_PNNF, "deleted file is gone");
    rmdir(dir);
    io_set_nvram_dir("nvram");
}

/* Drive the display driver the way the game does (through the video SetStt entry point). */
static uint32_t vcall(int sub, uint32_t d3, uint32_t d4, uint32_t d5, uint32_t d6, uint32_t d7, uint32_t a0)
{
    m68k_set_reg(M68K_REG_D2, sub); m68k_set_reg(M68K_REG_D3, d3); m68k_set_reg(M68K_REG_D4, d4);
    m68k_set_reg(M68K_REG_D5, d5); m68k_set_reg(M68K_REG_D6, d6); m68k_set_reg(M68K_REG_D7, d7);
    m68k_set_reg(M68K_REG_A0, a0);
    uint32_t d0 = 0;
    CHECK(video_setstt(0x56, &d0) == 0, "video sub-code %02x accepted", sub);
    return d0;
}

static void test_video(void)
{
    reset();
    video_reset();
    /* plane A: CLUT7 bitmap, colour 5 = pure red; plane B: off. Full weight, never transparent. */
    uint32_t pix = cmem_alloc(1, 384 * 4), rl = cmem_alloc(1, 64), buf = heap_alloc(64);
    for (int i = 0; i < 384 * 2; i++) wr8(pix + i, i < 384 ? 5 : 6);
    uint32_t fct = vcall(0, 0, 8, 0, 0, 0, 0), lct = vcall(6, 0, 280, 0, 0, 0, 0);
    CHECK(fct == 0 && lct == 0, "first FCT and first LCT are both id 0 (separate numbering)");
    uint32_t ins[] = { 0xC0000003, 0xC1000808, 0xDB00003F, 0xC3000000, 0x85FC0000, 0x86000000 | 0x00FC00, 0x78000000, 0x10000000 };
    for (int k = 0; k < 8; k++) wr32(buf + 4 * k, ins[k]);
    vcall(2, fct, 0, 8, 0, 0, buf);                      /* dc_wrfct */
    vcall(0x0A, lct, 0, 0, 0x40000000 | pix, 0, 0);      /* dc_wrli line 0: VSR = pixels */
    vcall(0x0C, fct, lct, 0, 0, 0, 0);                   /* dc_flnk */
    vcall(0x0E, fct, 0, 0, 0, 0, 0);                     /* dc_exec plane A only */
    video_field();
    uint32_t c0 = ll_frame[0][100] & 0xFFFFFF, c1 = ll_frame[1][100] & 0xFFFFFF;
    CHECK((c0 >> 16) > 0xF0 && (c0 & 0xFFFF) == 0, "line 0 is CLUT colour 5 (red), got %06x", c0);
    CHECK((c1 >> 8 & 0xFF) > 0xF0 && (c1 >> 16) == 0, "line 1 continues from the VSR: colour 6 (green), got %06x", c1);

    /* run-length on line 0: 100 pixels of colour 6 then colour 5 for the rest */
    uint8_t rle[] = { 0x86, 100, 0x85, 0 };
    for (int i = 0; i < 4; i++) wr8(rl + i, rle[i]);
    vcall(4, fct, 6, 0x78000002, 0, 0, 0);              /* dc_wrfi: display parameters = run-length */
    vcall(0x0A, lct, 0, 0, 0x40000000 | rl, 0, 0);
    video_field();
    uint32_t a = ll_frame[0][2 * 99] & 0xFFFFFF, b = ll_frame[0][2 * 100] & 0xFFFFFF;
    CHECK((a >> 8 & 0xFF) > 0xF0 && (b >> 16) > 0xF0, "RL7 run of 100 green pixels, then red (%06x %06x)", a, b);
}

static void test_disc(const char *bin)
{
    reset();
    CHECK(disc_open(bin), "open disc image");
    const DiscFile *f = disc_find("cdi_tribes");
    CHECK(f && f->size == 135168, "cdi_tribes in the directory");
    CHECK(disc_find("/cd/LUXOR.RTF") != NULL, "lookup ignores /cd/ and case");

    /* every program module on the disc carries a valid OS-9 CRC */
    static const char *names[] = { "cdi_launcher", "cdi_tribes", "cdi_space", "cdi_nvrui", "cdi_bumperanim" };
    for (int i = 0; i < 5; i++) {
        const DiscFile *df = disc_find(names[i]);
        uint8_t *data = disc_read_file(df);
        uint32_t size = (uint32_t)data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
        uint32_t a = heap_alloc(size);
        memcpy(host(a), data, size);
        CHECK(os9_crc(0xFFFFFF, a, size) == 0x800FE3, "%s module CRC", names[i]);
        free(data);
    }

    /* ADPCM decoder matches the reference Python decoder (tools/xa.py) bit for bit */
    struct { uint32_t lbn, scan; int ch; long long sum, sumabs; int first0; bool stereo; } ref[] = {
        { 98967, 400, 15, 74508, 78744048, 0, true },        /* themes.rtf music, stereo 37.8 kHz */
        { 276234, 2000, 4, 1436779, 78277611, -5, false },   /* messages.rtf voice, mono 18.9 kHz */
    };
    for (int r = 0; r < 2; r++) {
        Adpcm st; adpcm_reset(&st);
        long long sum = 0, sumabs = 0; int got = 0, n_samples = 0, first = 99999, rate = 0;
        uint8_t data[2324], sub[4];
        static int16_t pcm[4032 * 2];
        for (uint32_t k = 0; k < ref[r].scan && got < 20; k++) {
            disc_read_sector(ref[r].lbn + k, data, sub);
            if (!(sub[2] & 4) || sub[1] != ref[r].ch) continue;
            int n = adpcm_decode_sector(&st, data, sub[3], pcm, &rate);
            for (int i = 0; i < n; i++) {
                int vals = ref[r].stereo ? 2 : 1;
                for (int c = 0; c < vals; c++) {
                    int v = pcm[2 * i + c];
                    if (first == 99999) first = v;
                    sum += v; sumabs += v < 0 ? -v : v; n_samples++;
                }
            }
            got++;
        }
        CHECK(n_samples == 80640 && sum == ref[r].sum && sumabs == ref[r].sumabs && first == ref[r].first0,
              "ADPCM %s: %d samples sum %lld sumabs %lld (want 80640 %lld %lld)", ref[r].stereo ? "stereo" : "mono",
              n_samples, sum, sumabs, ref[r].sum, ref[r].sumabs);
        CHECK(rate == (ref[r].stereo ? 37800 : 18900), "ADPCM rate %d", rate);
    }

    /* the launcher boots and reaches its first fork without an unimplemented call */
    reset();
    disc_open(bin);
    CHECK(os9_boot("cdi_launcher"), "boot cdi_launcher");
    os9_run(15000000ull * 2);
}

int main(void)
{
    test_fork_wait_exit();
    test_wait_no_children();
    test_signal_intercept();
    test_signal_without_intercept_kills();
    test_alarm();
    test_sleep_timed();
    test_event_local();
    test_event_wait_across_processes();
    test_memory();
    test_datmod_and_link();
    test_nvram_files();
    test_video();
    const char *bin = getenv("LL_BIN");
    if (bin) test_disc(bin);
    else printf("LL_BIN not set: skipping disc tests\n");
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
