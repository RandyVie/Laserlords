/* I/O layer: system path table and the virtual file systems / devices behind it.
 *   /cd/...  or bare names  -> read-only files on the disc
 *   /nvr/... -> host files in the NVRAM directory (save games, CSD)
 *   /<other> -> stub device: every call is logged and succeeds (filled in as the trace shows) */
#include "ll.h"
#include "m68k.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <time.h>

#define REG(r)      m68k_get_reg(NULL, M68K_REG_##r)
#define SETREG(r,v) m68k_set_reg(M68K_REG_##r, (v))

enum { T_FREE, T_CONSOLE, T_DISC, T_MEM, T_NVR, T_DEVICE };

struct Path {
    int      type, refs;
    char     name[64];
    char     host[256];    /* T_NVR, and T_MEM directory images: host path */
    const DiscFile *df;    /* T_DISC */
    uint8_t *data;         /* T_DISC: file contents (Form 1 files only); T_MEM: contents */
    FILE    *fp;           /* T_NVR */
    uint32_t pos, size;
};

#define NSYSPATHS 64
static Path paths[NSYSPATHS];
static char nvr_dir[512] = "nvram";

/* A CD-i player has 8 KB of NVRAM, shared by every disc. (Kept small on purpose: the
 * launcher offers its "manage saves" screen when space runs out, as on real hardware.) */
#define NVRAM_SIZE 8192
#define NVR_ENTRY  32      /* bookkeeping charged per file (verify against a real player) */

/* OS-9 device types (PD_DTP, first byte of SS_Opt). The C library's readdir parses CD
 * directory records when it sees DT_CDFM, 32-byte RBF records otherwise. */
enum { DT_SCF = 0, DT_RBF = 1, DT_CDFM = 5, DT_UCM = 6 };

void io_set_nvram_dir(const char *dir) { snprintf(nvr_dir, sizeof nvr_dir, "%s", dir); }

/* Built-in Configuration Status Descriptor, served as /nvr/csd unless the NVRAM directory has
 * its own. Format (from the game's csd_ library): one "type:/device:params" record per line.
 * Types seen: 2 = audio, 3 = video (params: "TV" = 384 wide, "625" = PAL 280 lines), 5 = pointer. */
static const char builtin_csd[] =
    "2:/audio:\r"
    "3:/video:TV,625\r"
    "5:/ptr:CL=\"a\"\r";

void io_init(void)
{
    for (int i = 0; i < 3; i++) {
        paths[i].type = T_CONSOLE; paths[i].refs = 1000;
        snprintf(paths[i].name, sizeof paths[i].name, "/term%d", i);
    }
    ll_mkdir(nvr_dir);
}

/* Build an RBF-format directory image of a host directory: 32-byte records, name with the
 * last character's high bit set, 3-byte file descriptor sector number at +0x1D. */
static void build_dir_image(Path *ph, const char *hostdir)
{
    size_t cap = 32 * 66, n = 0;
    uint8_t *img = calloc(1, cap);
    static const char *dots[] = { "..", "." };
    for (int k = 0; k < 2; k++) {
        size_t l = strlen(dots[k]);
        memcpy(img + n, dots[k], l); img[n + l - 1] |= 0x80; img[n + 0x1F] = (uint8_t)(k + 1);
        n += 32;
    }
    DIR *d = opendir(hostdir);
    int lsn = 3;
    for (struct dirent *e; d && (e = readdir(d)) && n + 32 <= cap;) {
        if (e->d_name[0] == '.') continue;
        size_t l = strlen(e->d_name);
        if (l == 0 || l > 28) continue;
        memcpy(img + n, e->d_name, l);
        img[n + l - 1] |= 0x80;
        img[n + 0x1D] = 0; img[n + 0x1E] = (uint8_t)(lsn >> 8); img[n + 0x1F] = (uint8_t)lsn++;
        n += 32;
    }
    if (d) closedir(d);
    ph->type = T_MEM; ph->data = img; ph->size = (uint32_t)n;
    snprintf(ph->host, sizeof ph->host, "%s", hostdir);
}

static uint32_t nvram_used(void)
{
    uint32_t used = 0;
    DIR *d = opendir(nvr_dir);
    char hp[1024];
    struct stat st;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        snprintf(hp, sizeof hp, "%s/%s", nvr_dir, e->d_name);
        if (!stat(hp, &st) && S_ISREG(st.st_mode)) used += (uint32_t)st.st_size + NVR_ENTRY;
    }
    if (d) closedir(d);
    if (!d || !ll_access_ok(nvr_dir)) return 0;
    char csd[600]; snprintf(csd, sizeof csd, "%s/csd", nvr_dir);
    if (stat(csd, &st)) used += (uint32_t)sizeof builtin_csd - 1 + NVR_ENTRY;  /* built-in CSD lives in NVRAM too */
    return used;
}

static int alloc_path(void)
{
    for (int i = 3; i < NSYSPATHS; i++) if (paths[i].type == T_FREE) { memset(&paths[i], 0, sizeof paths[i]); paths[i].refs = 1; return i; }
    return -1;
}

static void host_nvr(const char *name, char *out, size_t n)
{
    const char *p = name + 4;            /* skip "/nvr" */
    while (*p == '/') p++;
    snprintf(out, n, "%s/%s", nvr_dir, *p ? p : ".");
}

static uint32_t parse_name(uint32_t a, char *out, size_t max)
{
    size_t n = 0;
    while (rd8(a) == ' ') a++;
    while (rd8(a) > ' ' && n < max - 1) out[n++] = (char)rd8(a++);
    out[n] = 0;
    return a;
}

int io_open(uint32_t name_addr, int mode, int *out, uint32_t *end)
{
    char name[64];
    *end = parse_name(name_addr, name, sizeof name);
    int i;
    if (!strncasecmp(name, "/nvr", 4) && (name[4] == '/' || !name[4])) {
        char hp[256];
        host_nvr(name, hp, sizeof hp);
        struct stat st;
        if (stat(hp, &st)) {
            if (strcasecmp(name, "/nvr/csd") || (mode & 2)) return E_PNNF;
            if ((i = alloc_path()) < 0) return E_BPNUM;
            paths[i].type = T_MEM;    /* read-only memory file */
            paths[i].size = sizeof builtin_csd - 1;
            paths[i].data = malloc(paths[i].size);
            memcpy(paths[i].data, builtin_csd, paths[i].size);
            snprintf(paths[i].name, sizeof paths[i].name, "%s", name);
            *out = i;
            return 0;
        }
        if ((i = alloc_path()) < 0) return E_BPNUM;
        if (S_ISDIR(st.st_mode)) {
            if (!(mode & 0x80) && (mode & 2)) { paths[i].type = T_FREE; return E_BMODE; }
            build_dir_image(&paths[i], hp);
        } else {
            if (mode & 0x80) { paths[i].type = T_FREE; return E_BMODE; }  /* not a directory */
            paths[i].type = T_NVR;
            paths[i].fp = fopen(hp, (mode & 2) ? "r+b" : "rb");
            if (!paths[i].fp) { paths[i].type = T_FREE; return E_PNNF; }
            paths[i].size = (uint32_t)st.st_size;
            snprintf(paths[i].host, sizeof paths[i].host, "%s", hp);
        }
    } else if (name[0] != '/' || !strncasecmp(name, "/cd/", 4)) {
        const DiscFile *f = disc_find(name);
        if (!f) return E_PNNF;
        if (mode & 2) return E_BMODE;  /* write to CD */
        if ((i = alloc_path()) < 0) return E_BPNUM;
        paths[i].type = T_DISC; paths[i].df = f; paths[i].size = f->size;
    } else {
        if ((i = alloc_path()) < 0) return E_BPNUM;
        paths[i].type = T_DEVICE;
    }
    snprintf(paths[i].name, sizeof paths[i].name, "%s", name);
    *out = i;
    return 0;
}

int io_create(uint32_t name_addr, int mode, uint32_t attrs, int *out, uint32_t *end)
{
    char name[64], hp[256];
    (void)attrs;
    *end = parse_name(name_addr, name, sizeof name);
    if (strncasecmp(name, "/nvr/", 5)) return E_BMODE;
    host_nvr(name, hp, sizeof hp);
    struct stat st;
    if (!stat(hp, &st)) return E_CEF;
    int i = alloc_path();
    if (i < 0) return E_BPNUM;
    paths[i].type = T_NVR;
    if (nvram_used() + NVR_ENTRY > NVRAM_SIZE) { paths[i].type = T_FREE; return 0xF8; /* E$Full */ }
    paths[i].fp = fopen(hp, "w+b");
    if (!paths[i].fp) { paths[i].type = T_FREE; return E_PNNF; }
    snprintf(paths[i].host, sizeof paths[i].host, "%s", hp);
    snprintf(paths[i].name, sizeof paths[i].name, "%s", name);
    (void)mode;
    *out = i;
    return 0;
}

int io_close(int p)
{
    if (p < 0 || p >= NSYSPATHS || paths[p].type == T_FREE) return E_BPNUM;
    if (--paths[p].refs > 0) return 0;
    if (paths[p].fp) fclose(paths[p].fp);
    free(paths[p].data);
    paths[p].type = T_FREE;
    return 0;
}

int io_dup(int p, int *out)
{
    if (p < 0 || p >= NSYSPATHS || paths[p].type == T_FREE) return E_BPNUM;
    paths[p].refs++;
    *out = p;
    return 0;
}

int io_read(int p, uint32_t buf, uint32_t count, uint32_t *done, bool line)
{
    Path *ph = &paths[p];
    *done = 0;
    switch (ph->type) {
    case T_CONSOLE:
        return E_EOF;
    case T_DISC: case T_MEM: {
        if (!ph->data) ph->data = disc_read_file(ph->df);
        if (ph->pos >= ph->size) return E_EOF;
        uint32_t n = 0;
        while (n < count && ph->pos < ph->size) {
            uint8_t c = ph->data[ph->pos++];
            wr8(buf + n++, c);
            if (line && c == '\r') break;
        }
        *done = n;
        return 0;
    }
    case T_NVR: {
        if (!ph->fp) return E_EOF;
        fseek(ph->fp, ph->pos, SEEK_SET);
        uint32_t n = 0;
        int c;
        while (n < count && (c = fgetc(ph->fp)) != EOF) {
            wr8(buf + n++, c);
            if (line && c == '\r') break;
        }
        ph->pos += n;
        *done = n;
        return n ? 0 : E_EOF;
    }
    case T_DEVICE:
        ll_log("        (read from stub device %s)\n", ph->name);
        return E_EOF;
    }
    return E_BPNUM;
}

int io_write(int p, uint32_t buf, uint32_t count, uint32_t *done, bool line)
{
    Path *ph = &paths[p];
    *done = 0;
    switch (ph->type) {
    case T_CONSOLE: {
        uint32_t n = 0;
        for (; n < count; n++) {
            int c = rd8(buf + n);
            fputc(c == '\r' ? '\n' : c, stderr);
            if (ll_trace && ll_trace != stderr) fputc(c == '\r' ? '\n' : c, ll_trace);
            if (line && c == '\r') { n++; break; }
        }
        *done = n;
        return 0;
    }
    case T_NVR:
        if (!ph->fp) return E_BMODE;
        if (ph->pos + count > ph->size && nvram_used() + (ph->pos + count - ph->size) > NVRAM_SIZE)
            return 0xF8;  /* E$Full: NVRAM exhausted */
        fseek(ph->fp, ph->pos, SEEK_SET);
        fwrite(host(buf), 1, count, ph->fp);
        fflush(ph->fp);
        ph->pos += count;
        if (ph->pos > ph->size) ph->size = ph->pos;
        *done = count;
        return 0;
    case T_DEVICE:
        ll_log("        (write %u bytes to stub device %s)\n", count, ph->name);
        *done = count;
        return 0;
    }
    return E_BMODE;
}

int io_seek(int p, uint32_t pos)
{
    if (p < 0 || paths[p].type == T_FREE) return E_BPNUM;
    paths[p].pos = pos;
    return 0;
}

int io_getstt(int p, int code)
{
    Path *ph = &paths[p];
    switch (code) {
    case 0x00:  /* SS_Opt: 128-byte path options to (a0); byte 0 = device type */
        memset(host(REG(A0)), 0, 128);
        wr8(REG(A0), ph->type == T_DISC ? DT_CDFM : ph->type == T_NVR || ph->type == T_MEM ? DT_RBF
                     : ph->type == T_DEVICE ? DT_UCM : DT_SCF);
        return 0;
    case 0x0F: {  /* SS_FD: RBF file descriptor, d2 = bytes wanted */
        uint8_t fd[16] = { 0 };
        struct stat st;
        fd[0] = 0x1B;   /* owner read/write, public read/write (verify the game cares) */
        time_t t = time(NULL);
        if (ph->host[0] && !stat(ph->host, &st)) {
            t = st.st_mtime;
            if (S_ISDIR(st.st_mode)) fd[0] |= 0x80;
            fd[9] = st.st_size >> 24; fd[10] = st.st_size >> 16; fd[11] = st.st_size >> 8; fd[12] = (uint8_t)st.st_size;
        }
        struct tm tm; ll_localtime(&t, &tm);
        fd[3] = (uint8_t)tm.tm_year; fd[4] = (uint8_t)(tm.tm_mon + 1); fd[5] = (uint8_t)tm.tm_mday;
        fd[6] = (uint8_t)tm.tm_hour; fd[7] = (uint8_t)tm.tm_min; fd[8] = 1;   /* link count */
        fd[13] = fd[3]; fd[14] = fd[4]; fd[15] = fd[5];                     /* creation date */
        uint32_t n = REG(D2) < 16 ? REG(D2) : 16;
        for (uint32_t k = 0; k < n; k++) wr8(REG(A0) + k, fd[k]);
        return 0;
    }
    case 0x3B:  /* sound maps */
        if (ph->type == T_DEVICE && !strcasecmp(ph->name, "/audio")) return soundmap_getstt(REG(D2) & 0xFFFF);
        break;
    case 0x59:  /* pointer (UCM): on /video or /ptr */
        if (ph->type == T_DEVICE) return input_getstt(REG(D2) & 0xFFFF);
        break;
    case 0x43:  /* NVRAM free bytes in d2 (on a /nvr path) */
        if (ph->type == T_NVR || (ph->type == T_MEM && ph->host[0])) {
            uint32_t used = nvram_used();
            SETREG(D2, used >= NVRAM_SIZE ? 0 : NVRAM_SIZE - used);
            ll_log("        NVRAM free: %u bytes\n", REG(D2));
            return 0;
        }
        break;
    case 0x01:  /* SS_Ready */
        if (ph->type == T_CONSOLE) return 0xF6; /* E$NotRdy */
        SETREG(D1, 1);
        return 0;
    case 0x02:  /* SS_Size */
        SETREG(D2, ph->size);
        return 0;
    case 0x05:  /* SS_Pos */
        SETREG(D2, ph->pos);
        return 0;
    case 0x06:  /* SS_EOF */
        return ph->pos >= ph->size && ph->type != T_DEVICE ? E_EOF : 0;
    }
    ll_log("        GetStt %02x on %s: unhandled, returning zeros\n", code, ph->name);
    return 0;
}

static void hexdump(const char *label, uint32_t a, int n)
{
    if (!ll_trace || a < MODULE_BASE || a >= MEM_SIZE) return;
    ll_log("        %s @%06x:", label, a);
    for (int i = 0; i < n; i++) ll_log("%s%02x", i % 4 ? "" : " ", rd8(a + i));
    ll_log("\n");
}

/* Play control block (SS_Play, verify layout): dump it and the PCLs it points at. */
static void dump_pcb(uint32_t pcb)
{
    hexdump("PCB", pcb, 32);
    for (int k = 0; k < 3; k++) {
        uint32_t pcl = rd32(pcb + 0x0E + 4 * k);
        for (int depth = 0; pcl && depth < 4; depth++) {
            static const char *n[] = { "video PCL", "audio PCL", "data PCL" };
            hexdump(n[k], pcl, 24);
            uint32_t next = rd32(pcl + 4);
            if (next == pcl) break;
            pcl = next;
        }
    }
}

int io_setstt(int p, int code)
{
    Path *ph = &paths[p];
    if (ph->type == T_DISC && code == 0x2D && ph->df) {     /* SS_Play from the current position */
        dump_pcb(REG(A0));
        uint32_t first = ph->pos / 2048, total = (ph->size + 2047) / 2048;
        return cdfm_play(os9_cur_pid(), REG(A0), ph->df->lbn + first, total > first ? total - first : 0);
    }
    if (ph->type == T_DISC && code == 0x31) { cdfm_abort(os9_cur_pid()); return 0; }  /* SS_Abort */
    if (ph->type == T_DEVICE && code == 0x56 && (REG(D2) & 0xFFFF) == 0x11) {  /* video: VBL signal */
        ll_log("        vblank signal %d %s\n", REG(D3) & 0xFFFF, (REG(D3) & 0xFFFF) ? "armed" : "disarmed");
        os9_vblank_signal(REG(D3) & 0xFFFF);
        return 0;
    }
    if (ph->type == T_DEVICE && code == 0x59) return input_setstt(REG(D2) & 0xFFFF);
    if (ph->type == T_DEVICE && code == 0x3B && !strcasecmp(ph->name, "/audio")) return soundmap_setstt(REG(D2) & 0xFFFF);
    if (ph->type == T_DEVICE && code == 0x3D && !strcasecmp(ph->name, "/audio")) {   /* attenuation */
        audio_set_attenuation(REG(D3));
        return 0;
    }
    if (ph->type == T_DEVICE && (code == 0x56 || code == 0x52) && !strcasecmp(ph->name, "/video")) {
        uint32_t d0 = REG(D0);
        int e = video_setstt(code, &d0);
        SETREG(D0, d0);
        return e;
    }
    else if (ph->type == T_DEVICE && (code == 0x56 || code == 0x51 || code == 0x52)) {
        int sub = REG(D2) & 0xFFFF;
        ll_log("        %02x/%02x args d3=%x d4=%x d5=%x d6=%x d7=%x a0=%06x\n", code, sub,
               REG(D3), REG(D4), REG(D5), REG(D6), REG(D7), REG(A0));
        uint32_t a0 = REG(A0);
        if (code == 0x56 && a0 >= MODULE_BASE && a0 < MEM_SIZE && (sub == 2 || sub == 7 || sub == 8 || sub == 0x1c || sub == 0x1b || sub == 1)) {
            int n = sub == 2 || sub == 1 ? (REG(D5) & 0xFFFF) : (REG(D6) & 0xFFFF) * (REG(D7) & 0xFFFF);
            if (n > 32) n = 32;
            ll_log("        instrs:");
            for (int k = 0; k < n; k++) ll_log(" %08x", rd32(a0 + 4 * k));
            ll_log("\n");
        }
    }
    else if (ph->type == T_DEVICE || ph->type == T_DISC) hexdump("a0", REG(A0), 32);
    switch (code) {
    case 0x00: return 0;  /* SS_Opt */
    case 0x02:            /* SS_Size */
        if (ph->type == T_NVR && ph->fp) {
            ph->size = REG(D2);
            fflush(ph->fp);
            ll_truncate(ph->fp, ph->size);
        }
        return 0;
    }
    ll_log("        SetStt %02x on %s: unhandled (stub ok)\n", code, ph->name);
    return 0;
}

int io_delete(uint32_t name_addr)
{
    char name[64], hp[256];
    parse_name(name_addr, name, sizeof name);
    if (strncasecmp(name, "/nvr/", 5)) return E_BMODE;
    host_nvr(name, hp, sizeof hp);
    return remove(hp) ? E_PNNF : 0;
}

int io_makdir(uint32_t name_addr)
{
    char name[64], hp[256];
    parse_name(name_addr, name, sizeof name);
    if (strncasecmp(name, "/nvr/", 5)) return E_BMODE;
    host_nvr(name, hp, sizeof hp);
    return ll_mkdir(hp) && errno != EEXIST ? E_BPNAM : 0;
}
