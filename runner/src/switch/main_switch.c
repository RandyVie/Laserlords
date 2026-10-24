/* Nintendo Switch homebrew front end (.nro, devkitPro libnx + SDL2).
 * The CD-i core is shared with the desktop build; this file replaces main.c: libnx for the
 * Joy-Cons, SD card paths and text screens, SDL2 for the picture and sound.
 *
 * Files live in sdmc:/switch/laserlords/: put the disc image there (.chd, or .cue + .bin).
 * Settings and saves (nvram/) are written there too; with record_session=1 in settings.txt the
 * session's input is also recorded to session.llrec (off by default).
 *
 * Controls: left stick / D-pad move, A = button 1, B = button 2 (Y and X work too), + quits,
 * - toggles the border crop. */
#include "ll.h"
#include <SDL.h>
#include <switch.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

void cpu_init(void);
uint64_t os9_cycles(void);
int os9_run_until(uint64_t until);

#define DIR_PATH "sdmc:/switch/laserlords/"
#define CPU_HZ ll_cpu_hz
#define FIELD_CYCLES (CPU_HZ / ll_field_hz)
#define OUT_W 768                  /* picture width in MCD212 units; 280 lines = 576 tall (4:3) */
#define OUT_H 576
#define CROP_LINES 20              /* black bands above and below the 240-line picture */

static PadState pad;
static bool crop = true;

/* ---- text screens (libnx console, used while SDL video is not running) ---- */

static void text_screen(const char *text, bool wait_for_a)
{
    consoleInit(NULL);
    printf("%s", text);
    consoleUpdate(NULL);
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (!wait_for_a || (down & (HidNpadButton_A | HidNpadButton_Plus))) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

static const char help_text[] =
    "\n  LASER LORDS (CD-i) for Nintendo Switch\n\n"
    "  Left stick / D-pad   move the pointer, walk\n"
    "  A                    select, talk, act (button 1)\n"
    "  B                    button 2\n"
    "  -                    border crop on / off\n"
    "  +                    quit\n\n"
    "  Point at the words at the bottom of the screen (Beamdown,\n"
    "  Hyperspace, Computer) and press A to choose them.\n"
    "  Press A during a cutscene to skip it.\n\n"
    "  Saves and settings: " DIR_PATH "\n\n"
    "  Press A to start.\n";

/* ---- disc image: the remembered one, else the first .chd / .cue / .bin in DIR_PATH ---- */

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

static const char *find_disc(void)
{
    static char path[512];
    const char *saved = settings_get("disc", NULL);
    if (saved && disc_open(saved)) return saved;
    static const char *exts[] = { ".chd", ".cue", ".bin" };
    for (int k = 0; k < 3; k++) {
        DIR *d = opendir(DIR_PATH);
        if (!d) return NULL;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || !has_ext(e->d_name, exts[k])) continue;
            snprintf(path, sizeof path, DIR_PATH "%s", e->d_name);
            if (disc_open(path)) { closedir(d); return path; }
        }
        closedir(d);
    }
    return NULL;
}

/* ---- input: libnx pad -> CD-i pointer (same mapping as the desktop gamepad) ---- */

static void poll_input(double dt, bool *quit)
{
    padUpdate(&pad);
    u64 held = padGetButtons(&pad), down = padGetButtonsDown(&pad);
    if (down & HidNpadButton_Plus) *quit = true;
    if (down & HidNpadButton_Minus) { crop = !crop; settings_set("crop_top", crop ? "1" : "0"); }

    HidAnalogStickState ls = padGetStickPos(&pad, 0);
    double sx = ls.x / 32767.0, sy = -ls.y / 32767.0;         /* stick y is up-positive */
    if (sx * sx + sy * sy < 0.04) sx = sy = 0;                /* dead zone */
    sx += !!(held & HidNpadButton_Right) - !!(held & HidNpadButton_Left);
    sy += !!(held & HidNpadButton_Down) - !!(held & HidNpadButton_Up);
    double mag = sqrt(sx * sx + sy * sy);
    double speed = 700 * (mag > 1 ? 1 : mag * mag);           /* units/s, gentle near the centre */
    if (mag > 0) input_move_pointer(sx / mag * speed * dt, sy / mag * speed * dt);
    input_set_deflection(sx > 1 ? 1 : sx < -1 ? -1 : sx, sy > 1 ? 1 : sy < -1 ? -1 : sy);
    input_decay(dt);

    int b = 0;
    if (held & (HidNpadButton_A | HidNpadButton_Y)) b |= 1;
    if (held & (HidNpadButton_B | HidNpadButton_X)) b |= 2;
    input_set_buttons(b);
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    socketInitializeDefault();                  /* stdout over the network when started by nxlink */
    if (nxlinkStdio() < 0) socketExit();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    mkdir(DIR_PATH, 0755);
    settings_set_dir(DIR_PATH);

    const char *disc = find_disc();
    if (!disc) {
        text_screen("\n  LASER LORDS\n\n  No disc image found.\n\n"
                    "  Copy your own Laser Lords (Europe) disc image (.chd, or .cue + .bin)\n"
                    "  to " DIR_PATH " on the SD card and start again.\n\n"
                    "  Press + to exit.\n", true);
        return 0;
    }
    settings_set("disc", disc);
    if (strcmp(settings_get("help_shown", "0"), "1")) {
        text_screen(help_text, true);
        settings_set("help_shown", "1");
    }

    crop = strcmp(settings_get("crop_top", "1"), "0") != 0;
    if (!strcmp(settings_get("fix_crossfade", "1"), "0")) video_fix_crossfade = false;
    ll_game_speed = atoi(settings_get("planet_pace", "100"));
    io_set_nvram_dir(DIR_PATH "nvram");
    if (!strcmp(settings_get("record_session", "0"), "1")) {   /* opt-in, for bug hunting */
        remove(DIR_PATH "session-1.llrec");
        rename(DIR_PATH "session.llrec", DIR_PATH "session-1.llrec");
        input_record_open(DIR_PATH "session.llrec", ll_game_speed);
    }

    mem_init();
    cpu_init();
    io_init();
    os9_init();
    video_reset();
    audio_reset();
    soundmap_reset();
    if (!os9_boot("cdi_launcher")) {
        text_screen("\n  The disc image could not be started (cdi_launcher not found).\n\n  Press + to exit.\n", true);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) return 1;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Window *win = SDL_CreateWindow("Laser Lords", 0, 0, 1280, 720, SDL_WINDOW_FULLSCREEN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, VIDEO_W, VIDEO_H);

    /* audio: the core makes 37.8 kHz stereo; SDL converts to what the console plays (48 kHz) */
    SDL_AudioSpec want = { 0 }, have;
    want.freq = 48000; want.format = AUDIO_S16; want.channels = 2; want.samples = 1024;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    SDL_AudioStream *cvt = dev ? SDL_NewAudioStream(AUDIO_S16, 2, 37800, have.format, have.channels, have.freq) : NULL;
    if (dev) SDL_PauseAudioDevice(dev, 0);

    uint64_t freq = SDL_GetPerformanceFrequency(), t0 = SDL_GetPerformanceCounter(), last = t0;
    uint64_t base = os9_cycles();
    bool quit = false;
    int r = 1;
    static int16_t pcm[4096 * 2], out[8192 * 2];
    while (!quit && appletMainLoop()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) quit = true;
        uint64_t now = SDL_GetPerformanceCounter();
        poll_input((double)(now - last) / freq, &quit);
        last = now;
        input_record_tick(os9_cycles());

        /* emulate up to wall-clock time, at most 4 fields per frame so a stall can't spiral */
        uint64_t want_c = base + (uint64_t)((double)(now - t0) * (double)CPU_HZ / freq);
        if (want_c > os9_cycles() + 4 * FIELD_CYCLES) {
            base -= want_c - (os9_cycles() + 4 * FIELD_CYCLES);
            want_c = os9_cycles() + 4 * FIELD_CYCLES;
        }
        if (r == 1 && want_c > os9_cycles()) r = os9_run_until(want_c);

        int n;
        while ((n = audio_drain(pcm, 4096)) > 0) {
            if (!cvt) continue;
            SDL_AudioStreamPut(cvt, pcm, n * 4);
            int got = SDL_AudioStreamGet(cvt, out, sizeof out);
            /* keep latency bounded: drop if the device is more than ~0.25 s behind */
            if (got > 0 && SDL_GetQueuedAudioSize(dev) < (Uint32)(have.freq * 4 / 4)) SDL_QueueAudio(dev, out, got);
        }

        /* picture: rows 20-259 hold the 240-line picture (the opening logo also uses 0-19) */
        int top = crop && !os9_process_alive("cdi_bumperanim") ? CROP_LINES : 0;
        int shown = VIDEO_H - top - (crop ? CROP_LINES : 0);
        SDL_UpdateTexture(tex, NULL, ll_frame, VIDEO_W * 4);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        double out_h = (double)OUT_H * shown / VIDEO_H;
        double s = 1280.0 / OUT_W < 720.0 / out_h ? 1280.0 / OUT_W : 720.0 / out_h;
        SDL_Rect src = { 0, top, VIDEO_W, shown };
        SDL_Rect dst = { (int)((1280 - OUT_W * s) / 2), (int)((720 - out_h * s) / 2), (int)(OUT_W * s), (int)(out_h * s) };
        SDL_RenderCopy(ren, tex, &src, &dst);
        SDL_RenderPresent(ren);
    }

    input_record_close();
    if (cvt) SDL_FreeAudioStream(cvt);
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    socketExit();
    return 0;
}
