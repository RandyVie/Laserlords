# Laser Lords: native runner

## ⬇️ Download (v0.8.0 preview)

| System | Download |
|---|---|
| macOS 11+ (Intel and Apple Silicon) | [LaserLords-0.8.0-macOS.dmg](https://github.com/RandyVie/Laserlords/releases/download/v0.8.0/LaserLords-0.8.0-macOS.dmg) |
| Windows 10/11 (64-bit) | [LaserLords-0.8.0-windows-x64.zip](https://github.com/RandyVie/Laserlords/releases/download/v0.8.0/LaserLords-0.8.0-windows-x64.zip) |
| Linux x86_64 / Steam Deck | [LaserLords-0.8.0-linux-x86_64.tar.gz](https://github.com/RandyVie/Laserlords/releases/download/v0.8.0/LaserLords-0.8.0-linux-x86_64.tar.gz) |
| Nintendo Switch (homebrew) | [LaserLords-0.8.0-switch.zip](https://github.com/RandyVie/Laserlords/releases/download/v0.8.0/LaserLords-0.8.0-switch.zip) |

Release notes, checksums and older versions: [all releases](https://github.com/RandyVie/Laserlords/releases). You need your own Laser Lords disc image; see [Download and play](#download-and-play) for how to start each version.

Plays **Laser Lords** (Spinnaker / AIM, Philips CD-i, 1992) natively on macOS, Linux (Steam Deck included), Windows and Nintendo Switch (homebrew), without a CD-i emulator or BIOS.

The game's original 68000 code runs unchanged on an embedded CPU core (Musashi). The CD-i operating system (CD-RTOS / OS-9), the CD file manager, the MCD212 video chip and the ADPCM audio are reimplemented natively. See [NOTES.md](NOTES.md) for everything learned about the disc and the system.

**You need your own disc image.** No game data is included or ever committed. Use a MAME CHD (`.chd`), or `.cue`/`.bin` from `chdman extractcd`.

> **Status: preview.** The intro, the first planet (Luxor), walking, conversations, sound, saving and loading are tested and match the real CD-i (checked against MAME running the original ROM). The rest of the game has not been played through yet, so expect bugs further in; please report them (see [Reporting bugs](#reporting-bugs)). Tested on macOS and on a Nintendo Switch; the Windows and Linux builds pass the tests but haven't been tried on real machines yet.

## Disclaimer

This is an unofficial fan project. It is not affiliated with, endorsed by or connected to Philips, Spinnaker Software, American Interactive Media or the current rights holders of Laser Lords. "Laser Lords", "CD-i" and related names are trademarks of their respective owners.

This repository and its downloads contain **no game data, no game code and no CD-i system ROM (BIOS)**: only original code that reimplements the CD-i system the game runs on. To play you need your own copy of the game as a disc image. Please don't ask for, or share, disc images or ROMs.

The software is provided as is, without warranty (see [LICENSE](LICENSE)).

## Download and play

Get the file for your system from the [Releases](https://github.com/RandyVie/Laserlords/releases) page, then point it at your own disc image (`.chd`, or `.cue` + `.bin`).

| System | Download | How to start |
|---|---|---|
| macOS 11+ (Intel and Apple Silicon) | `LaserLords-<version>-macOS.dmg` | Open the dmg and drag Laser Lords to Applications. The app isn't notarised by Apple: the first time, right-click it and choose **Open**, then **Open** again. |
| Windows 10/11 (64-bit) | `LaserLords-<version>-windows-x64.zip` | Unzip and run `LaserLords.exe`. The exe isn't code-signed, so Windows may show "Windows protected your PC": click **More info** → **Run anyway**. |
| Linux x86_64 / Steam Deck | `LaserLords-<version>-linux-x86_64.tar.gz` | Unpack and run `laserlords` (see [Steam Deck](#steam-deck) below). |
| Nintendo Switch (homebrew) | `LaserLords-<version>-switch.zip` | Needs custom firmware with the Homebrew Menu. Copy the zip's `switch` folder to the root of the SD card; see [Nintendo Switch](#nintendo-switch-homebrew) below. |

Each download includes `READ ME FIRST.txt`, the licence and the third-party notices. `SHA256SUMS.txt` lists checksums of the files.

On first start the app asks for the disc image and remembers it; the controls are shown once (and with F1 later).

## Playing

Start the app; it asks for the disc image once and remembers it. Or pass it on the command line:

```bash
build/laserlords "Laser Lords (Europe).chd"
```

| Action | Mouse | Keyboard | Gamepad / Steam Deck |
|---|---|---|---|
| Move pointer / walk | move (the first click captures the mouse, Esc releases it) | arrows / WASD | left stick / D-pad |
| Button 1 (select, talk, act) | left click | Space, Enter, Z | A |
| Button 2 | right click | X, Backspace | B |
| Fullscreen | | F11, Alt+Enter, Cmd/Ctrl+Enter, Cmd/Ctrl+F | |
| Skip a cutscene | any button | any button | A or B |
| Show the controls | | F1 | |
| Scaling (sharp / nearest / linear) | | F2 | |
| Borders on / off | | F3 | |
| Slow the planets down (100% / 70% / 50%) | | F4 | |
| Quit | | Cmd+Q / Ctrl+Q | |

Saves and settings live in the per-user data folder:
- macOS: `~/Library/Application Support/LaserLords/LaserLords/`
- Linux / Steam Deck: `~/.local/share/LaserLords/LaserLords/`
- Windows: `%APPDATA%\LaserLords\LaserLords\`

The in-game Computer → Save Game / Load Game screens write `nvram/laserlords<N>.nvri` there, and `settings.txt` remembers the disc, fullscreen, scaling, border crop (`crop_top`), planet pace (`planet_pace`) and the crossfade fix (`fix_crossfade`).

The game is played with the pointer: point at the words at the bottom of the screen (Beamdown, Hyperspace, Computer) and press button 1. The controls are shown on first start and with F1.

The intro takes about 2½ minutes, and the original only lets you skip it during the title. The runner adds a skip: pressing a button during the intro, a warp or a video message (any cutscene that isn't reading the controls) ends that cutscene's video stream, just as if it had finished. It never acts in a stream's first 3 seconds (the title's fade-in would hang) or on the planets, where the game streams scenery and conversations from the disc.

**Original pace:** timing is calibrated against the real CD-i (MAME running the original ROM): the intro's images and fades and the warrior's walking speed match it. F4 can slow the planets down further (70%, 50%); it is remembered as `planet_pace` in `settings.txt`.

**Bug reports:** every session in the window (and on the Switch) records its input to `session.llrec` in the data folder (the previous session is kept as `session-1.llrec`). `laserlords --headless --replay session.llrec --seconds N "disc.chd"` plays the same session again exactly, so a problem can be reproduced and traced.

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
| Nintendo Switch (homebrew) | `scripts/build-switch.sh` (needs Docker; devkitPro image) | `dist/switch/laserlords.nro` |
| Release files | `scripts/make-release.sh` (after the builds above) | `dist/release/` (versioned downloads with read-me, licences, checksums) |

### Steam Deck

1. Copy `laserlords` and your disc image to the Deck, for example `~/Games/LaserLords/`.
2. In Desktop Mode, add `laserlords` to Steam as a non-Steam game, with the disc as its launch option: `"/home/deck/Games/LaserLords/Laser Lords (Europe).chd"`. The file dialog doesn't work in Gaming Mode, so give the path once; it's remembered.
3. It starts fullscreen in Gaming Mode. The Deck's controls arrive as a standard gamepad (stick/D-pad + A/B).

### Nintendo Switch (homebrew)

For a Switch running custom firmware (Atmosphère) with the Homebrew Menu:

1. Copy `laserlords.nro` and your own disc image (`.chd`, or `.cue` + `.bin`) to `sdmc:/switch/laserlords/` on the SD card.
2. Start it from the Homebrew Menu (open the Album). Controls: left stick / D-pad move, A = button 1, B = button 2, − = border crop, + = quit.
3. Saves, settings and the session recording (`session.llrec`, for bug reports) are written to the same folder.

For quick testing, the Homebrew Menu's netloader (press Y) accepts builds over Wi-Fi: `nxlink -s dist/switch/laserlords.nro` (with `-s`, the app's log appears in the terminal).

## Reporting bugs

Open an [issue](https://github.com/RandyVie/Laserlords/issues) and describe what you did and what went wrong. Every session records your input (not the game) to `session.llrec` in the data folder (on the Switch: `sdmc:/switch/laserlords/`). Reproduce the problem, quit, and attach that file: with it the session can be replayed exactly and the bug found quickly.

## Developer tools

See **[GUIDE_NEW_GAME.md](GUIDE_NEW_GAME.md)** for the full workflow (porting another CD-i game, comparing with MAME, debugging recipes, verified CD-i facts). `laserlords --help` lists every option.

- `--headless --seconds N --shots DIR:N --wav FILE --trace FILE`: run without a window, dump frames, audio and a full system-call trace.
- `--input T:b1`, `T:m:DX,DY`, `T:s:SX,SY,DUR`: scripted input for headless runs.
- `--record FILE` / `--replay FILE`: record or replay input exactly; `--debug-field N`: dump the display program for one field.
- `tools/`: disc extraction (`cdi.py`), module info, disassembler, syscall catalogue, Ghidra C decompilation (`decompile.sh` → `out/c/`), offline image decoders, MAME reference scripts (`tools/mame/`), CD-i ROM inspection (`tools/rom/`), frame and audio measurement (`frames.py`, `audio_rms.py`), session-to-MAME conversion (`llrec_to_route.py`).

## Licence and third-party code

The project's own code is under the [MIT licence](LICENSE). Bundled and linked components keep their own licences; the full texts are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md):

- Musashi 68000 core (MIT), `runner/third_party/musashi`
- libchdr (BSD-3), with zstd (BSD-3), LZMA (public domain) and miniz (MIT), `runner/third_party/libchdr`
- SDL3 (zlib licence); on the Switch SDL2 (zlib), libnx (ISC) and Mesa/libdrm (MIT), from devkitPro

MAME's `mcd212.cpp` was used as a hardware reference for the video chip's behaviour; no code was copied.
