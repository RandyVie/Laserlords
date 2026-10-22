/* Disc access: the CD-i filesystem on top of raw 2352-byte MODE2 sectors, read either from a
 * MAME CHD (via libchdr) or a raw .bin (chdman extractcd output). */
#include "ll.h"
#include <libchdr/chd.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#define RAW 2352

static FILE *bin;
static chd_file *chd;
static uint8_t *hunk;             /* CHD: current decompressed hunk */
static uint32_t hunk_no = UINT32_MAX, hunk_bytes, frames_per_hunk;
#define CHD_FRAME 2448            /* CD CHDs store 2352 bytes of sector + 96 of subcode per frame */

/* Raw 2352-byte sector `lbn` into raw; false past the end. */
static bool read_raw(uint32_t lbn, uint8_t *raw)
{
    if (chd) {
        uint32_t h = lbn / frames_per_hunk;
        if (h != hunk_no) {
            if (chd_read(chd, h, hunk) != CHDERR_NONE) return false;
            hunk_no = h;
        }
        memcpy(raw, hunk + (lbn % frames_per_hunk) * CHD_FRAME, RAW);
        return true;
    }
    return !fseek(bin, (long)lbn * RAW, SEEK_SET) && fread(raw, 1, RAW, bin) == RAW;
}
static DiscFile files[64];
static int nfiles;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

int disc_read_sector(uint32_t lbn, uint8_t *data, uint8_t sub[4])
{
    uint8_t raw[RAW];
    if (!read_raw(lbn, raw)) {
        memset(data, 0, 2324);
        return 0;
    }
    if (sub) memcpy(sub, raw + 16, 4);
    int len = (raw[18] & 0x20) ? 2324 : 2048;
    memcpy(data, raw + 24, len);
    return len;
}

bool disc_open(const char *path)
{
    size_t pl = strlen(path);
    if (pl > 4 && !strcasecmp(path + pl - 4, ".chd")) {
        if (chd_open(path, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE) return false;
        const chd_header *h = chd_get_header(chd);
        hunk_bytes = h->hunkbytes;
        frames_per_hunk = hunk_bytes / CHD_FRAME;
        if (!frames_per_hunk || h->unitbytes != CHD_FRAME) {
            ll_log("disc: %s is not a CD CHD (unit %u)\n", path, h->unitbytes);
            chd_close(chd); chd = NULL;
            return false;
        }
        hunk = malloc(hunk_bytes);
    } else if (pl > 4 && !strcasecmp(path + pl - 4, ".cue")) {
        /* chdman's cue names the .bin next to it */
        char binpath[1024];
        snprintf(binpath, sizeof binpath, "%.*sbin", (int)(pl - 3), path);
        bin = fopen(binpath, "rb");
        if (!bin) return false;
    } else {
        bin = fopen(path, "rb");
        if (!bin) return false;
    }
    uint8_t lbl[2324], d[2324];
    disc_read_sector(16, lbl, NULL);
    if (memcmp(lbl, "\x01" "CD-I ", 6)) { ll_log("disc: no CD-i label at sector 16\n"); return false; }
    uint32_t pt_size = be32(lbl + 136), pt_addr = be32(lbl + 148);
    disc_read_sector(pt_addr, d, NULL);
    /* Laser Lords has a single (root) directory; take the first path table entry. */
    (void)pt_size;
    uint32_t dir = be32(d + 2);
    disc_read_sector(dir, d, NULL);
    for (int j = 0; j < 2048 && d[j] && nfiles < 64;) {
        int len = d[j], nl = d[j + 32];
        const uint8_t *name = d + j + 33;
        if (!(nl == 1 && name[0] <= 1)) {
            DiscFile *f = &files[nfiles++];
            memcpy(f->name, name, nl < 31 ? nl : 31);
            f->name[nl < 31 ? nl : 31] = 0;
            f->lbn = be32(d + j + 6);
            f->size = be32(d + j + 14);
            f->attr_flags = d[j + 25];
        }
        j += len;
    }
    (void)be16;
    return nfiles > 0;
}

int disc_file_count(void) { return nfiles; }
const DiscFile *disc_file(int i) { return i < nfiles ? &files[i] : NULL; }

const DiscFile *disc_find(const char *name)
{
    if (!strncasecmp(name, "/cd/", 4)) name += 4;
    else if (!strncasecmp(name, "/cd", 3) && !name[3]) return NULL;
    while (*name == '/') name++;
    for (int i = 0; i < nfiles; i++)
        if (!strcasecmp(files[i].name, name)) return &files[i];
    return NULL;
}

uint8_t *disc_read_file(const DiscFile *f)
{
    uint8_t *buf = malloc(f->size + 2048), d[2324];
    uint32_t n = (f->size + 2047) / 2048;
    for (uint32_t k = 0; k < n; k++) {
        disc_read_sector(f->lbn + k, d, NULL);
        memcpy(buf + k * 2048, d, 2048);
    }
    return buf;
}
