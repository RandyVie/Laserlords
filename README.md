# Laser Lords: native runner

Plays **Laser Lords** (Spinnaker / AIM, Philips CD-i, 1992) natively on macOS, Linux (Steam Deck included) and Windows, without a CD-i emulator.

The game's original 68000 code runs unchanged on an embedded CPU core (Musashi). The CD-i operating system (CD-RTOS / OS-9), the CD file manager, the MCD212 video chip and the ADPCM audio are reimplemented natively. See [NOTES.md](NOTES.md) for everything learned about the disc and the system.

**You need your own disc image.** No game data is included or ever committed. Use a MAME CHD (`.chd`), or `.cue`/`.bin` from `chdman extractcd`.

## Playing

Start the app; it asks for the disc image once and remembers it. Or pass it on the command line:

```bash
build/laserlords "Laser Lords (Europe).chd"
```

| Action | Mouse | Keyboard | Gamepad / Steam Deck |
|---|---|---|---|
| Move pointer / walk | move (click to capture, Esc releases) | arrows / WASD | left stick / D-pad |
| Button 1 (select, action) | left click | Space, Enter, Z | A |
| Button 2 | right click | X, Backspace | B |
| Fullscreen | | F11, Alt+Enter, Cmd+Enter, Cmd+F | |
| Skip a cutscene | left click | Space | A |
| Show the controls | | F1 | |
| Scaling (sharp / nearest / linear) | | F2 | |
| Borders on / off | | F3 | |
| Slow the planets down (100% / 70% / 50%) | | F4 | |
| Quit | | Cmd+Q / Ctrl+Q | |

Saves and settings live in the per-user data folder:
- macOS: `~/Library/Application Support/LaserLords/LaserLords/`
- Linux / Steam Deck: `~/.local/share/LaserLords/LaserLords/`
- Windows: `%APPDATA%\LaserLords\LaserLords\`

The in-game Computer → Save Game / Load Game screens write `nvram/laserlords<N>.nvri` there, and `settings.txt` remembers the disc, fullscreen and scaling.

The game is played with the pointer: point at the words at the bottom of the screen (Beamdown, Hyperspace, Computer) and press button 1. The controls are shown on first start and with F1.

The intro takes about 2½ minutes, and the original only lets you skip it during the title. The runner adds a skip: pressing a button during the intro, a warp or a video message (any cutscene that isn't reading the controls) ends that cutscene's video stream, just as if it had finished. It never acts on the planets, where the game streams scenery and conversations from the disc.

**Original pace:** timing is calibrated against the real CD-i (MAME running the original ROM): the intro's images and fades and the warrior's walking speed match it. F4 can slow the planets down further (70%, 50%); it is remembered as `planet_pace` in `settings.txt`.

**Bug reports:** every session in the window records its input to `session.llrec` in the data folder (the previous session is kept as `session-1.llrec`). `laserlords --headless --replay session.llrec --seconds N "disc.chd"` plays the same session again exactly, so a problem can be reproduced and traced.

There is no arrow cursor, as on the original: on the orbit screen the blinking word is the current choice (move left / right to change it), and on the planets the pointer works as a joystick.

**Cosmetic fixes (on by default):**
- During warp crossfades the game's own display program shows ~1-5 s of colour noise where the cockpit panel was. The runner hides it, showing the space image there instead (`fix_crossfade=0` in `settings.txt` turns this off).
- The game's pictures are 240 lines (rows 20-259 of the 280-line field); the black bands above and below are cropped (the opening logo keeps the top band), so the picture fills a 14:9 window. F3 or `crop_top=0` shows the whole field.

To see the game exactly as it draws itself, run with `--accurate`.

## Building

Requirements: CMake 3.20+, a C17 compiler. SDL3 is used if installed (`brew install sdl3`); otherwise it's downloaded and linked statically (`-DLL_BUNDLED_SDL=ON` forces this).

```bash
cmake -S . -B build && cmake --build build -j   # build/laserlords (+ build/LaserLords.app on macOS)
(cd build && ctest)                              # tests; disc tests use data/ll.bin or *.chd if present
```

Release packages go into `dist/`:

| Target | Command | Output |
|---|---|---|
| macOS app + dmg | `scripts/build-macos.sh` (`UNIVERSAL=1` for Intel + Apple Silicon) | `dist/LaserLords.app`, `dist/LaserLords-macOS.dmg` |
| Linux x86_64 / Steam Deck | `scripts/build-linux.sh` (needs Docker) | `dist/LaserLords-linux-x86_64.tar.gz` |
| Windows x86_64 | `scripts/build-windows.sh` (needs `mingw-w64`) | `dist/windows/LaserLords.exe` |

### Steam Deck

1. Copy `laserlords` and your disc image to the Deck, for example `~/Games/LaserLords/`.
2. In Desktop Mode, add `laserlords` to Steam as a non-Steam game, with the disc as its launch option: `"/home/deck/Games/LaserLords/Laser Lords (Europe).chd"`. The file dialog doesn't work in Gaming Mode, so give the path once; it's remembered.
3. It starts fullscreen in Gaming Mode. The Deck's controls arrive as a standard gamepad (stick/D-pad + A/B).

## Developer tools

See **[GUIDE_NEW_GAME.md](GUIDE_NEW_GAME.md)** for the full workflow (porting another CD-i game, comparing with MAME, debugging recipes, verified CD-i facts).


- `--headless --seconds N --shots DIR:N --wav FILE --trace FILE`: run without a window, dump frames, audio and a full system-call trace.
- `--input T:b1`, `T:m:DX,DY`, `T:s:SX,SY,DUR`: scripted input for headless runs.
- `tools/`: disc extraction (`cdi.py`), module info, disassembler, syscall catalogue, Ghidra C decompilation (`decompile.sh` → `out/c/`), offline image decoders, MAME reference scripts (`tools/mame/`), CD-i ROM inspection (`tools/rom/`), frame and audio measurement (`frames.py`, `audio_rms.py`), session-to-MAME conversion (`llrec_to_route.py`).

## Third-party code

- Musashi 68000 core (MIT), `runner/third_party/musashi`
- libchdr (BSD-3), with zstd, LZMA and miniz, `runner/third_party/libchdr`
- SDL3 (zlib licence)

MAME's `mcd212.cpp` was used as a hardware reference for the video chip's behaviour; no code was copied.
