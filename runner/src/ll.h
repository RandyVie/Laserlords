/* Laser Lords runner: shared declarations. */
#ifndef LL_H
#define LL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- logging (log.c) ---- */
extern FILE *ll_trace;        /* syscall trace, NULL = off */
extern int   ll_verbose;
void ll_log(const char *fmt, ...);
void ll_fatal(const char *fmt, ...);
void ll_flush(void);

/* ---- emulated memory (mem.c) ----
 * Flat 16 MB, 24-bit address bus like the 68000.
 *   0x000000  vectors, fake system globals
 *   0x010000  module area (loaded modules, data modules)
 *   0x200000  general heap (F$SRqMem, process static storage, stacks)
 *   0xA00000  coloured memory, plane A (F$SRqCMem colour 1) (verify colours)
 *   0xB00000  coloured memory, plane B (F$SRqCMem colour 2)
 */
#define MEM_SIZE      0x1000000u
#define MEM_MASK      0x0FFFFFFu
#define SYSGLOB_BASE  0x001000u
#define MODULE_BASE   0x010000u
#define MODULE_END    0x200000u
#define HEAP_BASE     0x200000u
#define HEAP_END      0xA00000u
#define CMEM_A_BASE   0xA00000u
#define CMEM_B_BASE   0xB00000u
#define CMEM_SIZE     0x080000u   /* 512 KB per plane, as on a real player */

extern uint8_t *mem;
static inline uint32_t rd8(uint32_t a)  { return mem[a & MEM_MASK]; }
static inline uint32_t rd16(uint32_t a) { a &= MEM_MASK; return (uint32_t)mem[a] << 8 | mem[a + 1]; }
static inline uint32_t rd32(uint32_t a) { return rd16(a) << 16 | rd16(a + 2); }
static inline void wr8(uint32_t a, uint32_t v)  { mem[a & MEM_MASK] = (uint8_t)v; }
static inline void wr16(uint32_t a, uint32_t v) { wr8(a, v >> 8); wr8(a + 1, v); }
static inline void wr32(uint32_t a, uint32_t v) { wr16(a, v >> 16); wr16(a + 2, v); }
static inline uint8_t *host(uint32_t a) { return mem + (a & MEM_MASK); }
size_t mem_strlen(uint32_t a, size_t max);
void   mem_cstr(uint32_t a, char *out, size_t max);  /* copy NUL-terminated guest string */

void     mem_init(void);
uint32_t heap_alloc(uint32_t size);              /* 0 on failure; memory is zeroed */
void     heap_free(uint32_t addr, uint32_t size);
uint32_t cmem_alloc(int color, uint32_t size);   /* coloured (video) memory */
uint32_t modarea_alloc(uint32_t size);
void     modarea_free(uint32_t addr, uint32_t size);

/* ---- disc (disc.c) ---- */
typedef struct {
    char     name[32];
    uint32_t lbn;
    uint32_t size;
    uint8_t  attr_flags;
} DiscFile;

bool  disc_open(const char *path);
int   disc_file_count(void);
const DiscFile *disc_file(int i);
const DiscFile *disc_find(const char *name);     /* case-insensitive, leading "/cd/" ignored */
/* Read one sector. Returns user-data length (2048 Form 1, 2324 Form 2), copies
 * the 4-byte subheader to sub[] if non-NULL. */
int   disc_read_sector(uint32_t lbn, uint8_t *data, uint8_t sub[4]);
/* Read a whole non-real-time file into a malloc'd buffer (Form 1 data). */
uint8_t *disc_read_file(const DiscFile *f);

/* ---- OS-9 kernel (os9.c) ---- */
extern uint64_t ll_cpu_hz;
extern uint32_t ll_field_hz;
extern int ll_game_speed;
void os9_init(void);
bool os9_boot(const char *module_name);          /* load + fork the first process */
uint32_t os9_crc(uint32_t crc, uint32_t addr, uint32_t len);
int  os9_install_module(const uint8_t *img, uint32_t len);  /* tests: module from memory */
int  os9_run(uint64_t max_instructions);         /* run scheduler; returns exit status */
bool os9_trap(int trapno);                       /* TRAP #n entry from the CPU hook */
int  os9_cur_pid(void);
void os9_signal_pid(int pid, int sig);
/* ---- CD file manager real-time play (cdfm.c) ---- */
int  cdfm_play(int pid, uint32_t pcb, uint32_t lbn, uint32_t nsec);
void cdfm_abort(int pid);
void cdfm_tick(int ticks_per_sec);
bool cdfm_busy(void);
bool os9_process_alive(const char *name);
void os9_charge(int cycles);
extern int ll_ucm_base, ll_ucm_entry;
void cdfm_skip(void);
bool cdfm_skippable(void);
uint64_t os9_cycles(void);
void cdfm_reset(void);
void os9_vblank_signal(int sig);                 /* 0 disarms */
void os9_skip_module(const char *name);          /* debug: fork of NAME exits at once */
void os9_skip_sets_res(int off, int val);        /* debug: launcher state poke on skip */

/* OS-9 error codes used by the runner */
enum {
    E_BPNUM = 0xC9, E_BMODE = 0xCB, E_MEMFUL = 0xCF, E_UNKSVC = 0xD0, E_EOF = 0xD3,
    E_BPNAM = 0xD7, E_PNNF = 0xD8, E_CEF = 0xDA, E_MNF = 0xDD, E_NOCHLD = 0xE2,
    E_EVNF = 0xA7, E_IPRCID = 0xE0,
};

/* ---- I/O layer (io.c) ---- */
typedef struct Path Path;
void io_init(void);
void io_set_nvram_dir(const char *dir);
/* Each returns 0 on success or an OS-9 error code. Registers come from the CPU. */
int  io_open(uint32_t name_addr, int mode, int *path_out, uint32_t *name_end);
int  io_create(uint32_t name_addr, int mode, uint32_t attrs, int *path_out, uint32_t *name_end);
int  io_close(int path);
int  io_dup(int path, int *out);
int  io_read(int path, uint32_t buf, uint32_t count, uint32_t *done, bool line);
int  io_write(int path, uint32_t buf, uint32_t count, uint32_t *done, bool line);
int  io_seek(int path, uint32_t pos);
int  io_getstt(int path, int code);   /* reads/writes CPU registers directly */
int  io_setstt(int path, int code);
int  io_delete(uint32_t name_addr);
int  io_makdir(uint32_t name_addr);

/* ---- pointer (input.c) ---- */
void input_set_pointer(int x, int y, int buttons);
void input_move_pointer(double dx, double dy);
void input_set_buttons(int buttons);
void input_set_deflection(double sx, double sy);
void input_mouse_motion(double dx, double dy);
void input_decay(double dt);
bool input_script_add(const char *spec);
void input_script_tick(double now_seconds);
bool input_record_open(const char *path, int speed);
void input_record_tick(uint64_t cycle);
void input_record_close(void);
int  input_replay_open(const char *path);
uint64_t input_replay_next(void);
void input_replay_apply(uint64_t cycle);
void input_get_pointer(int *x, int *y, int *buttons);
int  input_getstt(int sub);
int  input_setstt(int sub);

/* ---- audio (audio.c) ---- */
typedef struct { int p1[2], p2[2]; } Adpcm;
void adpcm_reset(Adpcm *d);
int  adpcm_decode_sector(Adpcm *d, const uint8_t *sector, uint8_t coding, int16_t *out, int *rate);
int  adpcm_decode_groups(Adpcm *d, const uint8_t *groups, int ngroups, uint8_t coding, int16_t *out, int *rate);
void audio_output_pcm(const int16_t *pcm, int frames, int rate);
void audio_set_attenuation(uint32_t d3);
void audio_play_effect(const int16_t *pcm, int frames, int rate, void (*done)(void *), void *arg);
void audio_stop_effect(void);
void audio_tick(long long target_frames);
int  soundmap_setstt(int sub);
int  soundmap_getstt(int sub);
void soundmap_reset(void);
int  audio_drain(int16_t *out, int max_frames);   /* 37.8 kHz stereo frames */
void audio_reset(void);
bool audio_wav_open(const char *path);
void audio_wav_pad_to(long long frames);
void audio_wav_close(void);

/* ---- settings (settings.c) ---- */
const char *settings_get(const char *key, const char *def);
void settings_set(const char *key, const char *val);

/* ---- video (video.c) ---- */
#define VIDEO_W 768                 /* MCD212 pixel units per line (normal res = 2 units/pixel) */
#define VIDEO_H 280                 /* PAL field lines */
extern uint32_t ll_frame[VIDEO_H][VIDEO_W];
extern int      ll_frame_count;
int  video_setstt(int code, uint32_t *d0);   /* SetStt 0x56 / 0x52 on the video path */
int  video_field(void);                      /* render one field; returns INTERRUPT count */
void video_reset(void);
bool video_save_png(const char *path);
extern bool video_fix_crossfade;   /* cosmetic warp-crossfade fix (default on) */

#endif
