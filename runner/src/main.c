/* Laser Lords native runner.
 *   laserlords [options] path/to/ll.bin
 * Runs the disc's boot module on the embedded 68000 with the OS-9 / CD-RTOS layer, and shows
 * the MCD212 output in an SDL window (or headless, for traces and frame dumps). */
#include "ll.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void cpu_init(void);
uint64_t os9_cycles(void);
int os9_run_until(uint64_t until);

#define CPU_HZ ll_cpu_hz
#define FIELD_CYCLES (CPU_HZ / ll_field_hz)

static void usage(void)
{
    fprintf(stderr,
        "usage: laserlords [options] [disc.chd | disc.cue | disc.bin]\n"
        "  (no disc: the last one used, or a file dialog)\n"
        "  --fullscreen        start fullscreen (F11 / Alt+Enter / Cmd+F toggle, F2 cycles scaling,\n"
        "                      F3 toggles the border crop, F1 shows the controls)\n"
        "  --accurate          no cosmetic fixes (show the warp-crossfade panel noise and the\n"
        "                      borders as the game draws them)\n"
        "  --headless          no window; run for --seconds of emulated time\n"
        "  --seconds N         emulated run time when headless (default 60)\n"
        "  --shots DIR:N       save every Nth field as DIR/fNNNNN.png\n"
        "  --trace FILE|-      log every system call\n"
        "  --input T:ACTION    headless input at T seconds: b1 / b2 / b3 (press), m:DX,DY (move),\n"
        "                      s:SX,SY,DUR (hold stick, -1..1, for DUR seconds)\n"
        "  --wav FILE          headless: write the audio output to a WAV file\n"
        "  --replay FILE       headless: replay a recorded session (the window records every session\n"
        "                      to session.llrec in the data folder; the previous one is session-1.llrec)\n"
        "  --record FILE       record this session's input (the window always writes session.llrec)\n"
        "  --debug-field N     debug: log the display program and per-line plane state for field N\n"
        "  --nvram DIR         host folder backing /nvr (default: per-user data folder)\n"
        "  --cpu-mhz N         emulated CPU clock (default 15)\n"
        "  --speed N           planet pace in percent, 25-100 (default 100 = original CD-i pace;\n"
        "                      F4 in the window cycles 100/70/50)\n"
        "  --boot MODULE       boot module (default cdi_launcher)\n"
        "  --skip MODULE       debug: a fork of MODULE exits at once (repeatable)\n"
        "  --res-byte OFF=VAL  debug: launcher state byte written when a skipped module exits\n");
    exit(2);
}

static char shot_dir[512];
static int shot_every;

static void maybe_shot(void)
{
    static int last = -1;   /* last bucket saved: two fields can land in one step */
    if (!shot_every || ll_frame_count / shot_every == last) return;
    last = ll_frame_count / shot_every;
    char path[600];
    snprintf(path, sizeof path, "%s/f%05d.png", shot_dir, ll_frame_count);
    video_save_png(path);
}

static int run_headless(double seconds)
{
    uint64_t end = (uint64_t)(seconds * CPU_HZ);
    int r = 1;
    while (r == 1 && os9_cycles() < end) {
        input_script_tick((double)os9_cycles() / CPU_HZ);
        input_replay_apply(os9_cycles());
        input_record_tick(os9_cycles());
        uint64_t to = os9_cycles() + FIELD_CYCLES, next = input_replay_next();
        if (next > os9_cycles() && next < to) to = next;   /* stop exactly where the input changed */
        r = os9_run_until(to);
        maybe_shot();
        audio_wav_pad_to((long long)(os9_cycles() * 37800 / CPU_HZ));
    }
    audio_wav_close();
    if (r == 1) r = os9_run(os9_cycles());   /* prints the hot-PC report */
    return r;
}

/* ---- host input -> CD-i pointer ----
 * Mouse: relative motion (captured after a click, Esc releases), left = button 1, right = 2.
 * Keyboard: arrows / WASD move, Space / Enter / Z = button 1, X / Backspace = button 2.
 * Gamepad (incl. Steam Deck): left stick / d-pad move, A = button 1, B = button 2. */
static SDL_Gamepad *pads[4];
static int mouse_buttons, key_buttons;
static double key_speed;

static void open_gamepads(void)
{
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n && i < 4; i++) if (!pads[i]) pads[i] = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

static void handle_input_event(const SDL_Event *e, SDL_Window *win)
{
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION: {
        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        if (ww > 0 && wh > 0) input_mouse_motion(e->motion.xrel * 768.0 / ww, e->motion.yrel * 560.0 / wh);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int bit = e->button.button == SDL_BUTTON_LEFT ? 1 : e->button.button == SDL_BUTTON_RIGHT ? 2 : 0;
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (!SDL_GetWindowRelativeMouseMode(win)) SDL_SetWindowRelativeMouseMode(win, true);   /* and counts */
            mouse_buttons |= bit;
        } else mouse_buttons &= ~bit;
        break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        SDL_Keycode k = e->key.key;
        int bit = (k == SDLK_SPACE || k == SDLK_RETURN || k == SDLK_Z) ? 1 : (k == SDLK_X || k == SDLK_BACKSPACE) ? 2 : 0;
        if (bit) key_buttons = down ? key_buttons | bit : key_buttons & ~bit;
        if (down && k == SDLK_ESCAPE) SDL_SetWindowRelativeMouseMode(win, false);
        break;
    }
    case SDL_EVENT_GAMEPAD_ADDED: open_gamepads(); break;
    }
}

/* Per host frame: keys and sticks move the pointer, all sources OR into the buttons. */
static void poll_input(double dt)
{
    const bool *ks = SDL_GetKeyboardState(NULL);
    double kx = (ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_D]) - (ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_A]);
    double ky = (ks[SDL_SCANCODE_DOWN] || ks[SDL_SCANCODE_S]) - (ks[SDL_SCANCODE_UP] || ks[SDL_SCANCODE_W]);
    key_speed = (kx || ky) ? (key_speed < 120 ? 120 : key_speed + 600 * dt) : 0;   /* units/s, accelerating */
    if (key_speed > 900) key_speed = 900;
    input_move_pointer(kx * key_speed * dt, ky * key_speed * dt);

    double dx = kx, dy = ky;   /* deflection: strongest of keys and sticks */
    int pad_buttons = 0;
    for (int i = 0; i < 4; i++) {
        if (!pads[i]) continue;
        double sx = SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_LEFTX) / 32767.0;
        double sy = SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_LEFTY) / 32767.0;
        if (sx * sx + sy * sy < 0.04) sx = sy = 0;                          /* dead zone */
        sx += SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_DPAD_RIGHT) - SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        sy += SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_DPAD_DOWN) - SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_DPAD_UP);
        if (fabs(sx) > fabs(dx)) dx = sx;
        if (fabs(sy) > fabs(dy)) dy = sy;
        double mag = sqrt(sx * sx + sy * sy);
        double speed = 700 * mag * mag;                                      /* gentle near the centre */
        if (mag > 0) input_move_pointer(sx / mag * speed * dt, sy / mag * speed * dt);
        if (SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_SOUTH)) pad_buttons |= 1;
        if (SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_EAST)) pad_buttons |= 2;
    }
    input_set_deflection(dx > 1 ? 1 : dx < -1 ? -1 : dx, dy > 1 ? 1 : dy < -1 ? -1 : dy);
    input_decay(dt);
    input_set_buttons(mouse_buttons | key_buttons | pad_buttons);
}

/* ---- window, scaling, fullscreen ----
 * The 768x280 field (two MCD212 units per normal pixel) is shown at 4:3 as 768x576. Scale modes:
 * "sharp" (default: integer nearest-neighbour upscale, then a smooth fit), "nearest", "linear". */
#define OUT_W 768
#define OUT_H 576

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex, *big;
static SDL_AudioStream *aud;
static int big_k, big_h;
static char scale_mode[16] = "sharp";

/* Border crop: the game's pictures are 240 lines (rows 20-259 of the 280-line field); rows 0-19
 * are black and rows 260+ lie below the active picture. Both bands are cut, except that the
 * opening logo, which draws from row 0, keeps the top band. F3 or crop_top=0 (or --accurate)
 * shows the whole field. */
#define CROP_LINES 20
bool crop_top = true;

static int top_lines(void) { return crop_top && !os9_process_alive("cdi_bumperanim") ? CROP_LINES : 0; }
static int bottom_lines(void) { return crop_top ? CROP_LINES : 0; }

static const char help_text[] =
    "Move the pointer: mouse (click in the window to capture it, Esc releases it), "
    "arrow keys / WASD, or the left stick / D-pad.\n\n"
    "Select, talk, act (button 1): left click, Space, Enter or Z; gamepad A.\n"
    "Button 2: right click, X or Backspace; gamepad B.\n\n"
    "Point at the words at the bottom of the screen (Beamdown, Hyperspace, Computer) "
    "and press button 1 to choose them.\n\n"
    "Skip a cutscene: press button 1.\n\n"
    "F1: this help    F11 or Cmd+Enter: fullscreen    F2: scaling    F3: borders\n"
    "F4: slow the planets down (100% = original CD-i pace, 70%, 50%)\n"
    "Cmd+Q / Ctrl+Q: quit";

static void update_title(void)
{
    char t[96];
    if (ll_game_speed >= 100) snprintf(t, sizeof t, "Laser Lords  (F1: controls)");
    else snprintf(t, sizeof t, "Laser Lords  (planet speed %d%%, F4 to change)", ll_game_speed);
    SDL_SetWindowTitle(win, t);
}

static void cycle_speed(void)
{
    ll_game_speed = ll_game_speed >= 100 ? 70 : ll_game_speed >= 70 ? 50 : 100;
    char v[8]; snprintf(v, sizeof v, "%d", ll_game_speed);
    settings_set("planet_pace", v);
    update_title();
}

static void show_help(void)
{
    SDL_SetWindowRelativeMouseMode(win, false);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Laser Lords controls", help_text, win);
}

static bool window_init(bool fullscreen)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_APP_NAME, "Laser Lords");
    int out_h = crop_top ? OUT_H * (VIDEO_H - 2 * CROP_LINES) / VIDEO_H : OUT_H;
    if (!SDL_CreateWindowAndRenderer("Laser Lords  (F1: controls)", OUT_W * 3 / 2, out_h * 3 / 2,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &ren)) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetWindowMinimumSize(win, OUT_W / 2, OUT_H / 2);
    SDL_SetRenderVSync(ren, 1);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, VIDEO_W, VIDEO_H);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    if (fullscreen) SDL_SetWindowFullscreen(win, true);
    snprintf(scale_mode, sizeof scale_mode, "%s", settings_get("scale", "sharp"));
    return true;
}

static void toggle_fullscreen(void)
{
    bool fs = !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN);
    SDL_SetWindowFullscreen(win, fs);
    settings_set("fullscreen", fs ? "1" : "0");
}

static void cycle_scale_mode(void)
{
    const char *next = !strcmp(scale_mode, "sharp") ? "nearest" : !strcmp(scale_mode, "nearest") ? "linear" : "sharp";
    snprintf(scale_mode, sizeof scale_mode, "%s", next);
    settings_set("scale", scale_mode);
    SDL_Log("scale mode: %s", scale_mode);
}

/* Largest rectangle of the picture's shape (4:3 for all 280 lines) centred in the output. */
static SDL_FRect fit_rect(int out_h)
{
    int w, h;
    SDL_GetCurrentRenderOutputSize(ren, &w, &h);
    float s = (float)w / OUT_W < (float)h / out_h ? (float)w / OUT_W : (float)h / out_h;
    SDL_FRect r = { (w - OUT_W * s) / 2, (h - out_h * s) / 2, OUT_W * s, out_h * s };
    return r;
}

static void present(void)
{
    SDL_UpdateTexture(tex, NULL, ll_frame, VIDEO_W * 4);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    int top = top_lines(), shown = VIDEO_H - top - bottom_lines(), out_h = OUT_H * shown / VIDEO_H;
    SDL_FRect src = { 0, (float)top, VIDEO_W, (float)shown };
    SDL_FRect dst = fit_rect(out_h);
    if (!strcmp(scale_mode, "sharp")) {
        /* pass 1: integer nearest upscale into an offscreen texture; pass 2: smooth fit */
        int k = (int)(dst.h / out_h + 0.999f);
        if (k < 1) k = 1;
        if (k > 4) k = 4;
        if (k != big_k || out_h != big_h) {
            if (big) SDL_DestroyTexture(big);
            big = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, OUT_W * k, out_h * k);
            SDL_SetTextureScaleMode(big, SDL_SCALEMODE_LINEAR);
            big_k = k; big_h = out_h;
        }
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        SDL_SetRenderTarget(ren, big);
        SDL_RenderTexture(ren, tex, &src, NULL);
        SDL_SetRenderTarget(ren, NULL);
        SDL_RenderTexture(ren, big, NULL, &dst);
    } else {
        SDL_SetTextureScaleMode(tex, !strcmp(scale_mode, "linear") ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        SDL_RenderTexture(ren, tex, &src, &dst);
    }
    SDL_RenderPresent(ren);
}

/* Ask for the disc image with the native file dialog; pumps events until answered. */
static char picked[1024];
static int pick_state;   /* 0 waiting, 1 chosen, -1 cancelled */

static void SDLCALL on_pick(void *ud, const char *const *files, int filter)
{
    (void)ud; (void)filter;
    if (files && files[0]) { snprintf(picked, sizeof picked, "%s", files[0]); pick_state = 1; }
    else pick_state = -1;
}

static const char *pick_disc(void)
{
    static const SDL_DialogFileFilter filters[] = {
        { "CD-i disc image (CHD, CUE, BIN)", "chd;cue;bin" },
        { "All files", "*" },
    };
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Laser Lords",
        "Please choose your Laser Lords CD-i disc image (.chd, or .cue/.bin).\n"
        "It is remembered for next time.", win);
    pick_state = 0;
    SDL_ShowOpenFileDialog(on_pick, NULL, win, filters, 2, NULL, false);
    while (pick_state == 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) return NULL;
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
    return pick_state == 1 ? picked : NULL;
}

static int run_window(void)
{
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 37800 };
    aud = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (aud) SDL_ResumeAudioStreamDevice(aud);
    else fprintf(stderr, "no audio: %s\n", SDL_GetError());

    open_gamepads();
    update_title();
    const char *shot = SDL_getenv("LL_WINDOW_SHOT");
    const char *shot_at = SDL_getenv("LL_WINDOW_SHOT_AT");
    uint64_t shot_ns = (uint64_t)((shot_at ? atof(shot_at) : 6.0) * 1e9);
    if (!shot && strcmp(settings_get("help_shown", "0"), "1")) {   /* first start: show the controls once */
        show_help();
        settings_set("help_shown", "1");
    }
    uint64_t t0 = SDL_GetTicksNS(), base = os9_cycles(), last_ns = t0;
    int r = 1;
    bool running = true, paused = false;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_WINDOW_MINIMIZED) paused = true;
            if (e.type == SDL_EVENT_WINDOW_RESTORED) paused = false;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = e.key.key;
                bool cmd = e.key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL), alt = e.key.mod & SDL_KMOD_ALT;
                if (k == SDLK_Q && cmd) running = false;
                if (k == SDLK_F11 || (k == SDLK_RETURN && (alt || cmd)) || (k == SDLK_F && cmd)) { toggle_fullscreen(); continue; }
                if (k == SDLK_F2) { cycle_scale_mode(); continue; }
                if (k == SDLK_F3) { crop_top = !crop_top; settings_set("crop_top", crop_top ? "1" : "0"); continue; }
                if (k == SDLK_F1) { show_help(); continue; }
                if (k == SDLK_F4) { cycle_speed(); continue; }
            }
            handle_input_event(&e, win);
        }
        uint64_t now_ns = SDL_GetTicksNS();
        poll_input((now_ns - last_ns) / 1e9);
        input_record_tick(os9_cycles());
        last_ns = now_ns;
        uint64_t want = base + (uint64_t)((double)(now_ns - t0) * (double)CPU_HZ / 1e9);
        if (paused) { base = os9_cycles(); t0 = now_ns; SDL_Delay(50); continue; }
        /* Emulate up to wall-clock time, at most 4 fields per host frame so a stall can't spiral. */
        if (want > os9_cycles() + 4 * FIELD_CYCLES) {
            base -= want - (os9_cycles() + 4 * FIELD_CYCLES);
            want = os9_cycles() + 4 * FIELD_CYCLES;
        }
        if (r == 1 && want > os9_cycles()) {
            r = os9_run_until(want);
            maybe_shot();
            if (r != 1) fprintf(stderr, "emulation stopped (%d)\n", r);
        }
        int16_t pcm[4096 * 2];
        int n;
        while ((n = audio_drain(pcm, 4096)) > 0)
            /* keep latency bounded: if the device fell behind by more than 0.25 s, drop */
            if (aud && SDL_GetAudioStreamQueued(aud) < 37800 * 4 / 4) SDL_PutAudioStreamData(aud, pcm, n * 4);
        present();
        /* debug: LL_WINDOW_SHOT=file.bmp saves the presented window after LL_WINDOW_SHOT_AT s (6) */
        if (shot && now_ns - t0 > shot_ns) {
            SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
            if (sf) { SDL_SaveBMP(sf, shot); SDL_DestroySurface(sf); }
            fprintf(stderr, "window shot: %.2f s wall, %.2f s emulated, %d fields\n",
                    (now_ns - t0) / 1e9, (double)os9_cycles() / CPU_HZ, ll_frame_count);
            running = false;
        }
    }
    if (aud) SDL_DestroyAudioStream(aud);
    if (big) SDL_DestroyTexture(big);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv)
{
    {   /* settings live in the per-user data folder (SDL_GetPrefPath works before SDL_Init) */
        char *pref = SDL_GetPrefPath("LaserLords", "LaserLords");
        settings_set_dir(pref);
        SDL_free(pref);
    }
    const char *disc = NULL, *boot = "cdi_launcher";
    double seconds = 60;
    bool headless = false, fullscreen = false, accurate = false, speed_set = false;
    const char *replay_path = NULL, *record_path = NULL;
    const char *wav_path = NULL;
    bool nvram_set = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace") && i + 1 < argc) {
            const char *f = argv[++i];
            ll_trace = !strcmp(f, "-") ? stderr : fopen(f, "w");
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = true;
        else if (!strcmp(argv[i], "--accurate")) { video_fix_crossfade = false; accurate = true; }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_path = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) record_path = argv[++i];
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) {
            char *c = strrchr(argv[++i], ':');
            if (!c) usage();
            *c = 0;
            snprintf(shot_dir, sizeof shot_dir, "%s", argv[i]);
            shot_every = atoi(c + 1);
        }
        else if (!strcmp(argv[i], "--boot") && i + 1 < argc) boot = argv[++i];
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc) os9_skip_module(argv[++i]);
        else if (!strcmp(argv[i], "--nvram") && i + 1 < argc) { io_set_nvram_dir(argv[++i]); nvram_set = true; }
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) { ll_game_speed = atoi(argv[++i]); speed_set = true; }
        else if (!strcmp(argv[i], "--cpu-mhz") && i + 1 < argc) ll_cpu_hz = (uint64_t)(atof(argv[++i]) * 1e6);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {   /* debug: LO:HI hex */
            extern uint32_t ll_watch_lo, ll_watch_hi;
            sscanf(argv[++i], "%x:%x", &ll_watch_lo, &ll_watch_hi);
        }
        else if (!strcmp(argv[i], "--debug-field") && i + 1 < argc) { extern int video_debug_field; video_debug_field = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--field-hz") && i + 1 < argc) ll_field_hz = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            if (!input_script_add(argv[++i])) usage();
        }
        else if (!strcmp(argv[i], "--res-byte") && i + 1 < argc) {   /* OFF=VAL, e.g. 0x42=6 */
            char *eq; long off = strtol(argv[++i], &eq, 0);
            if (*eq != '=') usage();
            os9_skip_sets_res((int)off, (int)strtol(eq + 1, NULL, 0));
        }
        else if (!strncmp(argv[i], "-psn_", 5)) continue;   /* macOS Finder process serial number */
        else if (argv[i][0] == '-') usage();
        else disc = argv[i];
    }
    if (headless && !disc) usage();

    /* cosmetic fixes: setting fix_crossfade=0 or --accurate turns them off */
    if (!accurate && !strcmp(settings_get("fix_crossfade", "1"), "0")) video_fix_crossfade = false;
    if (accurate || !strcmp(settings_get("crop_top", "1"), "0")) crop_top = false;
    if (!speed_set && !headless) ll_game_speed = atoi(settings_get("planet_pace", "100"));

    if (!headless) {
        /* Fullscreen by default on Steam Deck (Gaming Mode sets SteamDeck=1) or when last used. */
        const char *deck = SDL_getenv("SteamDeck");
        if ((deck && deck[0] == '1') || !strcmp(settings_get("fullscreen", "0"), "1")) fullscreen = true;
        if (!window_init(fullscreen)) return 1;
        if (!disc) {
            disc = settings_get("disc", NULL);
            if (disc && !disc_open(disc)) disc = NULL;
            else if (disc) goto opened;
            disc = pick_disc();
            if (!disc) return 0;
        }
    }
    if (!disc_open(disc)) {
        if (!headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Laser Lords", "That file is not a readable CD-i disc image.", win);
        fprintf(stderr, "cannot read CD-i disc image %s\n", disc);
        return 1;
    }
opened:
    if (!headless) {
        char abs_path[4096];
#ifdef _WIN32
        if (!_fullpath(abs_path, disc, sizeof abs_path)) snprintf(abs_path, sizeof abs_path, "%s", disc);
#else
        if (!realpath(disc, abs_path)) snprintf(abs_path, sizeof abs_path, "%s", disc);
#endif
        settings_set("disc", abs_path);
    }
    ll_log("disc: %d files\n", disc_file_count());

    if (!nvram_set) {   /* saves live in the per-user data folder (Application Support / XDG / AppData) */
        char *pref = SDL_GetPrefPath("LaserLords", "LaserLords");
        if (pref) {
            char dir[1024];
            snprintf(dir, sizeof dir, "%snvram", pref);
            io_set_nvram_dir(dir);
            SDL_free(pref);
        }
    }

    mem_init();
    cpu_init();
    io_init();
    os9_init();
    video_reset();
    audio_reset();
    soundmap_reset();
    if (wav_path && !audio_wav_open(wav_path)) { fprintf(stderr, "cannot write %s\n", wav_path); return 1; }
    if (getenv("LL_UCM_BASE")) ll_ucm_base = atoi(getenv("LL_UCM_BASE"));     /* calibration */
    if (getenv("LL_UCM_ENTRY")) ll_ucm_entry = atoi(getenv("LL_UCM_ENTRY"));
    { extern bool video_trace_fields; const char *tf = getenv("LL_TRACE_FIELDS"); video_trace_fields = tf && *tf == '1'; }
    if (replay_path) {
        int sp = input_replay_open(replay_path);
        if (sp < 0) { fprintf(stderr, "cannot read recording %s\n", replay_path); return 1; }
        if (!speed_set) ll_game_speed = sp;
    }
    if (record_path) input_record_open(record_path, ll_game_speed);
    else if (!headless) {   /* record this session's input; keep the previous one */
        char *pref = SDL_GetPrefPath("LaserLords", "LaserLords");
        if (pref) {
            char cur[1024], old[1024];
            snprintf(cur, sizeof cur, "%ssession.llrec", pref);
            snprintf(old, sizeof old, "%ssession-1.llrec", pref);
            remove(old);
            rename(cur, old);
            input_record_open(cur, ll_game_speed);
            SDL_free(pref);
        }
    }
    if (!os9_boot(boot)) return 1;
    int rc = headless ? run_headless(seconds) : run_window();
    input_record_close();
    if (headless) fprintf(stderr, "run ended (%d), %d fields rendered\n", rc, ll_frame_count);
    ll_flush();
    return 0;
}
