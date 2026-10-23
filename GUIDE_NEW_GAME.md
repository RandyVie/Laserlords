# Guide: native ports of CD-i games (and other disc-era games)

A working manual for Claude Code (and humans) based on the Laser Lords port. Read it before
starting a new game or a new project of this kind. It covers the method, the tools in this repo,
what is already known about the CD-i, the bugs that cost the most time and how to find them
quickly, and how to work with the person you're doing it for.

Contents:
1. What this approach is
2. Ground rules
3. The runner: map of the code
4. Workflow for a new CD-i game
5. The reference loop: running the real CD-i in MAME
6. Debug toolbox (runner flags, environment switches, scripts)
7. CD-i facts verified against the ROM or MAME
8. Reading the ROM
9. Playbooks for typical symptoms
10. Game-specific code in this repo (what to change for another game)
11. Packaging, releases and git
12. Working with the user
13. Shell and tool gotchas
14. A different platform

---

## 1. What this approach is

High-level emulation (HLE), "ScummVM, not MAME":

- The game's original 68000 code runs unchanged on the Musashi CPU core.
- The operating system (CD-RTOS, an OS-9/68K derivative) and the hardware the game talks to
  (CD file manager, MCD212 video, ADPCM audio, pointer, NVRAM) are reimplemented natively in C,
  with SDL3 for window, audio and input.
- The player supplies their own disc image. **No BIOS is needed to play**: the system is
  reimplemented, not emulated. A BIOS (and MAME) is only used during development as the ground
  truth.

Why it works: games only use a small part of the system, through a narrow API (OS-9 system calls,
I$SetStt/I$GetStt sub-codes). Implementing that API is far less work than emulating chips, and
the result is a small native app per platform.

Why it is subtle: wherever the real system's behaviour differs from your guess (timing, line
numbering, byte counts, an instruction's side effect), the game misbehaves in ways that look
unrelated. Almost every serious bug in Laser Lords was "my reimplementation assumed X, the ROM
does Y". **Measure against the real thing instead of guessing** (sections 5 and 8).

## 2. Ground rules

- **Never commit game data**: disc images (.chd/.bin/.cue), extracted files (`ext/`, `data/`),
  decompiled game code (`out/`), traces, BIOS ROMs (`CDi Bios/`, `*.rom`), session recordings
  (`*.llrec`), logs. `.gitignore` covers these; check `git diff --cached --name-only` before every
  commit anyway.
- Commit and push **only when the user asks** (see the project memory). Don't nag about commits.
- Keep `NOTES.md` as the research log: every system call, sub-code, address, data format and
  every bug's root cause. It is what makes the next game cheap. Mark guesses **(verify)**.
- Work in milestones and report at the end of each: what works, what doesn't, what was learned.
- Don't build speed hacks or cosmetic workarounds until the real cause is understood. When you
  must add one, put it behind a setting, and make `--accurate` turn it off.

## 3. The runner: map of the code

```
runner/src/
  main.c      CLI, window loop (wall-clock pacing), headless loop, scaling and border crop,
              fullscreen, key/mouse/gamepad mapping, help box, settings, recording hook
  os9.c       OS-9 kernel subset: modules, processes, scheduler, signals, events, alarms,
              F$Sleep, F$Time, game-speed factor, debug hot-PC statistics and register dumps
  io.c        paths and devices (/cd, /video, /ptr, /audio, /nvr), file I/O, CSD, Get/SetStt routing
  cdfm.c      CD file manager real-time play (SS_Play): PCB/PCL handling, sector delivery,
              ADPCM streaming, host cutscene skip
  video.c     MCD212 renderer: FCT/LCT display lists, ICA/DCA instructions, CLUT/DYUV/RGB555/RL7,
              matte, mixing; UCM display-control driver (dc_* sub-codes) and its CPU cost model
  audio.c     ADPCM decoding (level A/B/C), output FIFO, effect mixer, attenuation, WAV writer
  soundmap.c  sound maps (effects played from memory)
  input.c     pointer device (pt_coord), joystick-style deflection, scripted input,
              session recording and replay
  disc.c      bin/cue/chd reading (libchdr), mem.c memory map and heap, cpu.c Musashi glue,
              settings.c, log.c, compat.h (Windows shims)
tests/test_kernel.c   53 checks (kernel, NVRAM, video, ADPCM against xa.py, disc); some need the disc
tools/                Python tooling: disc extraction, module info, disassembly, Ghidra decompile,
                      MAME scripts (tools/mame), ROM tools (tools/rom), frame/audio measurement
packaging/, scripts/  macOS app/dmg, Linux (Docker), Windows (MinGW) builds
```

Build and test:

```bash
cmake -S . -B build && cmake --build build -j
(cd build && ctest)
build/laserlords "Disc.chd"                         # window
open build/LaserLords.app                           # what the user should test (no packaging needed)
```

## 4. Workflow for a new CD-i game

Milestones as used for Laser Lords (M0-M7), with the shortcuts learned since.

**M0 Disc and inventory.** `tools/cdi.py` lists the disc's directory and extracts modules to
`ext/` (with `LL_BIN=data/ll.bin` or a CHD converted by `chdman extractcd`). `tools/modinfo.py`
shows module headers. Note which program modules exist and how they chain (the launcher usually
forks/chains the others).

**M1 Static analysis.** `tools/disasm.py ext/<module>` (recursive descent, knows OS-9 TRAP
function codes) and `tools/syscalls.py` / `tools/wrappers.py` to list system calls and Get/SetStt
codes. Optional Ghidra decompile (`tools/decompile.sh`, output in `out/c/`, never committed):
very useful later to read game logic (main loops, handlers, checks like "bytes > 0x17ff").

**M2-M3 Boot the game.** The kernel subset in `os9.c` already covers what Laser Lords needed.
Run with `--trace -` and watch for unimplemented calls (`ll_log` prints them). Implement missing
calls from the OS-9 manual; confirm semantics in the ROM kernel when it matters (section 8).

**M4 Video, M5 audio and real-time play, M6 input and saving, M7 polish and packaging**: largely
done for the CD-i; a new game mostly exercises sub-codes Laser Lords didn't use. Grep the trace
for "not implemented" / "unsupported" / "ignored".

**Then, before calling anything done: compare with MAME** (section 5). Do this early, not after
the user reports problems. A checklist that would have saved most of the Laser Lords time:

- [ ] Intro/attract sequence: contact sheet every 4 s, runner vs MAME, same images and timing?
- [ ] Vertical/horizontal placement: picture top/bottom rows identical (runner y = (MAME y-43)/2)?
- [ ] One gameplay movement: steps per field and pixels per step identical (`frames.py sprite`)?
- [ ] One fade: duration in fields identical (`bright.lua` vs runner trace)?
- [ ] Sound effects: valid sound-group headers, levels in proportion to music (`audio_rms.py`)?
- [ ] Every screen type the game has: scrolling, menus, dialogue, text, transitions.
- [ ] Press buttons early, late and repeatedly during intros and loading (freezes hide there).
- [ ] Conversations / text rendering (fonts are often loaded from the disc stream).

## 5. The reference loop: running the real CD-i in MAME

MAME runs the real ROM and is the ground truth for behaviour and timing.

Setup (once per machine; the BIOS stays outside the repo):

```bash
brew install mame
tools/mame/make_bios.sh "CDi Bios" /tmp/mw     # builds /tmp/mw/roms/cdimono1.zip, verifies it
```

`cdimono1` needs `cdi200/cdi220/cdi220b.rom` (from cdimono1.zip) plus the servo and slave MCU
dumps from cdi910.zip; the script merges them. MAME reports them as "needs redump"; that's fine.

Running (headless, ~15-20x real time):

```bash
PRESSES=14 STOP=200 SNAP=0.5 tools/mame/run.sh /tmp/mw "$PWD/Disc.chd" tools/mame/route.lua 205
# snapshots: /tmp/mw/snap/cdimono1/NNNN.png  (use a fresh work dir or snap dir per run)
```

Facts about the MAME setup:
- MAME boots into the Philips shell; the disc only starts after clicking PLAY CD-I. The Lua
  scripts move the mouse +37 units at 12 s and press at 14 s. The game then runs **~19 s behind
  the runner** (BIOS boot + click): align on a clear event (first logo frame) and verify.
- Snapshots are 768x603: 43 rows of front-panel area on top, then 2 rows per field line.
  **Runner row y = (MAME row - 43) / 2.** Compare at full size; thumbnails hid a 20-line error.
- Inputs: `:MOUSEX`, `:MOUSEY` (relative; only the per-frame change matters; keep the counter
  positive, MAME clamps negatives), `:MOUSEBTN` buttons 1-3. `ports.lua` lists them.
- Audio: `MAME_SOUND=1` and `-wavwrite file.wav` (48 kHz). MAME's mixer is ~4x quieter than the
  runner's output; compare proportions.

Scripts in `tools/mame/`:
- `route.lua`: presses (`PRESSES`), timed moves (`ROUTE="80:d:8,88:r:12"`), snapshots.
- `replay.lua` + `tools/llrec_to_route.py`: **replay a player's recorded session on the real
  CD-i**. This is the strongest tool for "it breaks when I do X": it follows the player's route
  closely enough to reach the same spot, then shows what the real machine does there.
- `bright.lua`: per-frame brightness log to time fades and image changes cheaply.

## 6. Debug toolbox

Runner options (`laserlords --help` prints the full list):

| Option | Use |
|---|---|
| `--headless --seconds N` | Run without a window, ~30x real time |
| `--shots DIR:N` | Save every Nth field as `DIR/fNNNNN.png` (384x280). Two fields can land in one step, so names can be off by one: list the dir instead of predicting names |
| `--trace FILE` | Every system call, signal, PCL/PCB event, dc_* call, sound map, replay input |
| `--debug-field N` | Dump the FCT/LCT instructions and per-line plane state (VSR, ICM, weights) for field N |
| `--input T:b1`, `T:s:SX,SY,DUR`, `T:m:DX,DY` | Scripted input (button, stick held for DUR s, pointer move) |
| `--record FILE` / `--replay FILE` | Record / replay input exactly (window sessions record automatically to `session.llrec` in the data folder; previous one kept as `session-1.llrec`) |
| `--wav FILE` | Write the audio output |
| `--speed N`, `--cpu-mhz N` | Game-clock factor (planets only), emulated CPU clock (for experiments) |
| `--accurate` | Turn off cosmetic fixes |
| `--skip MODULE`, `--res-byte` | Jump past program stages |

Environment switches:

| Variable | Effect |
|---|---|
| `LL_TRACE_FIELDS=1` | `-- field N` markers in the trace (essential for timing) |
| `LL_PCHIST_FROM=N` | Restart the hot-PC statistics at field N (printed when a headless run ends): find where a hung game spins |
| `LL_DUMP_FROM=N`, `LL_PEEK=addr,...` | Dump the running process's registers, stack and memory every 50 fields from N |
| `LL_PLANE_ONLY=a\|b` | Render one plane (transparent = magenta): find what's hidden behind what |
| `LL_UCM_BASE`, `LL_UCM_ENTRY` | Display-driver cost model (calibration) |
| `LL_REPLAY_SHIFT`, `LL_REPLAY_SHIFT_FROM` | Shift replay events in cycles (realign an old recording) |
| `LL_WINDOW_SHOT=f.bmp`, `LL_WINDOW_SHOT_AT=s` | Save the real window after s seconds and quit (checks scaling, crop, speed) |

Measurement scripts: `tools/frames.py` (row brightness, first differing frame between two runs,
sprite movement per frame, first content row), `tools/audio_rms.py` (levels per window),
`tools/rom/` (section 8). Contact sheets with ffmpeg:
`ffmpeg -pattern_type glob -i 'DIR/f0*.png' -vf "scale=192:140,tile=8x6" -frames:v 1 sheet.png`.

Replays are deterministic: the same recording, disc and NVRAM give identical frames. A fix that
changes timing makes old recordings drift; that's expected (ask for a new recording).

## 7. CD-i facts verified against the ROM or MAME

These cost days to find. Each one broke something visible. ROM = cdi220b (Mono-I PAL).

**Kernel**
- **F$Sleep** (kernel +0x57b4): 0 = until a signal; **1 = only give up the time slice** (stays
  ready); n = sleep n-1 ticks; bit 31 = 1/256 s units. Sleeping a full tick for 1 slowed every
  yield-loop (intro fades took 3.2 s instead of 0.4 s; the game dropped 27 of 39 intro images).
- **Tick rate 100/s**: clock module `sgstom` sets D_TckSec ($28 in the system globals) to 100;
  `tim070driv` programs the 68070 timer (~100 Hz) and calls the kernel tick routine.
- F$Alarm A$Cycle in ticks (Laser Lords' planet logic: signal every 2 ticks).

**CPU cost of system calls.** The real display driver (UCM) is slow. With free system calls
the game loops ran far too fast (planet walk 2.5x too fast, fades instant once F$Sleep was
right). The runner charges every display/pointer call 50,000 cycles plus 1,100 per display-table
entry written (`video.c`, `os9_charge`). Calibrated on two MAME measurements (fade 24 fields;
walk step every 5 fields). Check it per game (section 9).

**Display lists (UCM / MCD212)**
- FCTs and LCTs are numbered separately from 0.
- **Line numbering** (video driver helpers +0x4a82 and +0x4c6c): block read/write (0x1b, 0x1c)
  address **table rows** (64 bytes each). dc_wrli 0x0a, dc_flnk 0x0c, dc_llnk 0x0d, fill 0x10 and
  0x07/0x08 take **frame lines: row = line / 2** (odd lines go to the second half of an
  interlaced table). An LCT's size is in frame lines too (484 means 242 rows).
- **A link is an instruction**: dc_llnk writes a "reload DCP" instruction into **column 7** of the
  source row; any later write to column 7 removes it. Games use this to switch layouts (Laser
  Lords scrolls by rewriting column 7, dropping its split-screen link, then re-links).
- A self-link (row jumping to itself) ends a table's walk, but the row repeats and picture data
  keeps streaming (the AIM logo's last text line relies on it).
- Active picture: **rows 20-259** of the 280-line field (header LCT links at line 39 = row 19).
  MAME shows black from row 260 on every screen.
- Double buffering: games alternate between two copies of a region; any mapping error shows as
  a jump every other frame.

**CD file manager**
- **PCL_Cnt (+0x18) counts bytes**, not sectors (Laser Lords checks its 3-sector font loaded
  with "> 0x17ff"; a sector count made it reject the font and text drawing hang).
- **Audio sectors go to memory as 2304 bytes** (18 sound groups), without the 20 spare bytes.
  Games index sound groups linearly across sectors; copying 2324 bytes made effects noise.
- PCL done = control byte bit 0 (bit 7 = error); the table entry advances to PCL_Nxt; a PCL
  completes when full or at end of record. PCB status: 0x01 EOR, 0x10 trigger, 0x80 EOF, 0x100
  play stopped (also on abort).
- 75 sectors/s; video PCLs alternate between two buffers in Laser Lords' intro.

**Other**
- Pointer pt_coord: buttons in D0 (bit 15 set while a button is down), x<<16|y in D1, 768x560
  units. Games may use it as a joystick (re-centre every read, compare offsets with a threshold).
- Sound maps (0x3B on /audio): GetStt sub 0 returns the descriptor in A0; status word set to 1
  when the effect ends.
- No visible graphics cursor in Laser Lords (set up once at 0,0).
- `I$Open "all/atnc24cl.ai1"` fails on the real machine too; the font comes from the disc stream.

## 8. Reading the ROM

The user's BIOS (outside the repo) holds the real kernel and drivers. Use it to settle "how does
the real system do X":

```bash
.venv/bin/python -I tools/rom/rom_modules.py cdi220b.rom          # kernel, init, sgstom, video, ucm, cdfm, ...
.venv/bin/python -I tools/rom/rom_disasm.py cdi220b.rom video 69c # disassemble from an offset
```

Recipes:
- `init` (system settings): clock module name at +0x46, time slice +0x48, extension modules.
- A **driver's entry table** is at its exec offset: init, read, write, getstat, setstat, term,
  exception (16-bit offsets). For the CD-i `video` driver, SetStat (0x69c) dispatches codes
  0x51-0x57 via a table at 0x6fe; code 0x56 goes to 0x8a4, which indexes sub-codes 0x00-0x1f via
  a table at 0x8c2 (handler = 0x8c2 + entry). Example: 0x0d dc_llnk at 0x4d98, 0x1c at 0x4a00.
- Kernel system calls: easier to find by behaviour (e.g. F$Sleep adds to D_Ticks `$54(a6)`).
- Compare the handler with the runner's implementation line by line; most bugs are a unit
  (rows vs lines, sectors vs bytes, 2324 vs 2304) or a side effect (a write that removes a link).

## 9. Playbooks for typical symptoms

**"It freezes when I do X"**
1. Ask the player to reproduce it in the window, then quit. Copy `session.llrec` and the NVRAM
   folder (to a scratch copy) from the data folder.
2. `--replay` it headless with `--shots` and `LL_TRACE_FIELDS=1 --trace`; find the press
   (`replay: buttons`) and what the game does after it.
3. No more system calls? `LL_PCHIST_FROM=<field>` gives the hot PCs; disassemble them
   (`out/asm/<module>.s`, offsets are module-relative). `LL_DUMP_FROM` + `LL_PEEK` for registers
   and memory. Look for loops on garbage (zero widths, null pointers: something wasn't loaded).
4. Still polling input? The game is waiting for you; the screen isn't showing it (go to the
   visual playbook).
5. Replay the session in MAME (`llrec_to_route.py` + `replay.lua`) to see what should happen.

**"It's too fast / too slow"**
- Measure, don't guess: frames per game step and pixels per step in both runner and MAME
  (`frames.py sprite`). Check the alarm/tick rate in the trace (signals per 10 s).
- Yield loops paced by F$Sleep(1) depend on CPU cost; adjust the UCM cost model only with two
  independent measurements. A user-facing speed factor is a last resort.

**"The picture is wrong / jumbled / hidden"**
- `--debug-field N` at the bad frame: which LCT row feeds each screen row, VSRs, ICM, weights.
- `LL_PLANE_ONLY=a` / `b`: is the content there but hidden behind the other plane?
- Compare frame-by-frame with MAME at full size. Every-other-frame differences = double
  buffering + a mapping error (line numbering, links).
- Garbage below or beside the picture: a table ran out; check where the real display ends.

**"The sound is noise / too loud"**
- Log effect roughness and sound-group header validity (`soundmap.c` does this in the trace);
  valid groups repeat their parameter bytes (0-3 = 4-7, 8-11 = 12-15).
- Compare levels with MAME as ratios (`audio_rms.py`).

**"Pressing a button too early breaks it"**
- Press at every second through the intro headless and check where each run ends up. Host-side
  conveniences (like the cutscene skip) are the usual cause.

## 10. Game-specific code in this repo

Everything below is Laser Lords specific; a new game needs its own version (ideally move these
into a per-game profile):

- Cutscene skip in `input.c`: only while `cdi_space` runs, not `cdi_tribes`, and never for plays
  younger than 3 s (`cdfm.c`).
- Planet speed factor in `os9.c` applies only while `cdi_tribes` runs (now an optional extra).
- Border crop in `main.c` keeps the top band while `cdi_bumperanim` (the logo) runs.
- Warp-crossfade cosmetic fix in `video.c` (`video_fix_crossfade`).
- Help text and controls description in `main.c` (Beamdown / Hyperspace / Computer).
- The UCM cost calibration values (probably right for other games, verify).
- `tests/test_kernel.c` disc checks assume Laser Lords' layout.

## 11. Packaging, releases and git

- Test with `build/LaserLords.app`; package only for distribution.
- `UNIVERSAL=1 scripts/build-macos.sh` (signed universal app + dmg), `scripts/build-windows.sh`
  (MinGW), `scripts/build-linux.sh` (Docker, Ubuntu 22.04 x86_64; tests run in the container).
- Docker Desktop sometimes hangs on start: wait with a timeout (`for i in $(seq 1 60)`), and if
  it doesn't come up, force-quit it and start again. Quit it when done.
- Background builds outlive later edits: note the commit a build started from, and stop stale
  builds before starting new ones.
- Repo: https://github.com/RandyVie/Laserlords (branch main). Commit messages end with the
  Co-Authored-By line from the session; push only when asked.

## 12. Working with the user

- They report symptoms in plain words ("super fast", "jumbled", "freezes when I talk"). Turn each
  into a measurement before changing code, and verify the fix the same way.
- Ask early for a recording (play, reproduce, quit), not a route description; describing routes
  in a platformer cost hours of blind scripted exploration.
- Reproduce, fix, re-verify against MAME, then tell them plainly what was wrong, what changed and
  what is not verified. Don't claim a fix you haven't reproduced.
- Point them at `build/LaserLords.app` for testing; packages take a while.
- They value: correct pace and visuals, skippable intros, readable controls. Keep cosmetic
  choices (crop, speed) behind settings.

## 13. Shell and tool gotchas

- zsh doesn't word-split variables: use `${=VAR}` for option lists stored in a variable, or a
  bash array. (Several "no frames written" runs were this.)
- Guard every `rm -rf` on variables: `rm -rf "${S:?}/dir"`; the sandbox blocks unguarded ones.
- macOS `awk` has no `strtonum`; use Python for hex parsing.
- `sleep` chains are blocked; use background tasks and wait for their notification.
- Shots: two fields can be emitted in one run step, so a predicted name like `f03500.png` may be
  `f03501.png`.
- Python without PIL: `tools/frames.py` has a small PNG reader; ffmpeg does sheets and crops.
- `tools/disasm.py` expects program modules; for ROM modules use `tools/rom/rom_disasm.py`.

## 14. A different platform

The method carries over, the code mostly doesn't:
1. Run the original CPU code on an embedded core.
2. Reimplement the OS / BIOS API the game uses, natively, starting from traces.
3. Add recording/replay and a trace from day one.
4. Get the reference emulator (MAME or a dedicated one) running headless with Lua or a
   scripting hook, and compare early: intro, placement, one movement, one fade, sound levels.
5. Read the real firmware where behaviour matters (units, side effects, timing).
6. Keep NOTES.md and update this guide with what you learn.
