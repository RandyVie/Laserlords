# Laser Lords: research notes

Running log of everything learned about the disc, the modules and the OS calls.
Items marked **(verify)** are inferences that still need a runtime trace.

## Tooling (how to regenerate everything)

| Step | Command | Output (gitignored) |
|---|---|---|
| CHD to raw | `chdman extractcd -i "Laser Lords (Europe).chd" -o data/ll.cue -ob data/ll.bin` | `data/ll.bin` (684 MB, 2352-byte sectors) |
| Filesystem + modules | `.venv/bin/python -I tools/cdi.py` | `ext/` |
| Module headers | `.venv/bin/python -I tools/modinfo.py` | stdout |
| Disassembly | `.venv/bin/python -I tools/disasm.py ext/cdi_tribes out/asm/cdi_tribes.s` | `out/asm/*.s` |
| Syscall catalogue | `.venv/bin/python -I tools/syscalls.py ext/cdi_*` | `out/syscalls.md` |
| C decompile (Ghidra 12.1 headless) | `tools/decompile.sh` | `out/c/*.c` |
| Runner | `cmake -S . -B build && cmake --build build` | `build/laserlords` |
| Kernel tests (M3) | `cmake --build build && (cd build && ctest --output-on-failure)` | 37 checks; disc checks use `data/ll.bin` |
| Play (window) | `build/laserlords data/ll.bin` | SDL window, real time, Esc quits |
| Headless frames (M4) | `cd traces && ../build/laserlords --headless --seconds 60 --shots DIR:50 ../data/ll.bin` | a PNG every 50 fields |
| Syscall trace (M2) | `cd traces && ../build/laserlords --headless --trace t.log --seconds 60 ../data/ll.bin` | `traces/t.log` |
| Offline image decode | `.venv/bin/python -I tools/video.py slides 120952 400 16` | `out/img/stream.png` |

Runner options: `--nvram DIR` sets the host folder backing `/nvr` (default `./nvram`). Debug options: `--skip MODULE` makes a fork of MODULE exit at once; `--res-byte 0x42=N` writes the launcher state byte (see below) when a skipped program exits. Reaching the main game directly: `--skip cdi_bumperanim --skip cdi_space --res-byte 0x42=2`. A run that stops prints the hottest PCs (busy-wait loops).

Python deps live in `.venv` (`numpy`, `capstone`). Ghidra: `brew install ghidra`.

## Program modules

All OS-9/68k modules, header parity valid. Type byte is at 0x12, language at 0x13 (not nibbles).

| Module | Size | Exec | Excpt | Static mem | Stack | Compiler | Functions found | Code covered |
|---|---|---|---|---|---|---|---|---|
| cdi_launcher | 0x5030 | 0x56 | 0x1a6 | 0x6de | 0xc00 | Microware C (LINK A5) | 249 | 93% |
| cdi_tribes | 0x20872 | 0x54 | 0x1a4 | 0x5746 | 0xc00 | Microware C | 992 | 83% |
| cdi_space | 0x9fba | 0x52 | 0x1a2 | 0x2932 | 0xc00 | Microware C | 429 | 89% |
| cdi_nvrui | 0xbf24 | 0x58a8 | 0 | 0x32b8 | 0xc00 | **different compiler**: LINK A6 frames, A5 = static base | 236 | 53% (20 KB data block at 0x694c) |
| cdi_bumperanim | 0x2f10 | 0x58 | 0x1c2 | 0x5e0 | 0xc00 | Microware C | 128 | 83% |

Data modules: `cdi_bumpdata` (0x84 bytes), and in `cdi_nvrui_res`: `cdi_ndat` (0x5ca0), `cdi_atnc18cl` (0xf8c), `cdi_atnc24cl` (0x1604). The `atnc18/24cl` names suggest 18 and 24 pixel CLUT fonts **(verify)**.

Ghidra decompiles all 2,280 functions without failure (`out/c/`, ~60k lines). It's raw output: A6-relative globals appear as `*(int *)(unaff_A6 + -0x7ff0)`. That's the C backup for a later rebuild.

### Microware C conventions seen

- A6 = static storage base, biased +0x8000 (globals at `-0x7ff0(a6)` and up). A5 = frame pointer (`LINK A5`).
- First two args in D0, D1, rest on the stack. Return in D0.
- cstart (exec entry) stores D6 (static size) at `-0x7ff0(a6)`/`-0x7fec(a6)`, D3 at `-0x7fe8(a6)`, A4 at `-0x7fe6(a6)`, then relocates initialized data (`idata`/`irefs`).
- The **excpt** entry is the unhandled-trap handler: on TRAP #15 it calls `F$TLink` for trap library `math`, else prints `**** Can't install trap handler ****` and exits. No reachable code executes TRAP #1-15, so **no trap library is needed**. Every system call is a plain `TRAP #0`.

## System call surface (M1)

Every syscall sits in a small C library wrapper (one site per wrapper per module), so the counts below are sites in all 5 modules, not frequency.

Kernel: `F$Link F$Load F$UnLink F$UnLoad F$Fork F$Chain F$Wait F$Send F$Exit F$Icpt F$Sleep F$Alarm F$Event F$Time F$Mem F$SRqMem F$SRtMem F$SRqCMem F$DatMod F$CRC F$SetCRC F$PrsNam F$CmpNam F$GModDr F$GPrDsc F$GPrDBT F$CpyMem F$SetSys F$SSpd F$SysID F$PErr F$SPrior F$TLink`.
Much of this is linked-in library code that's never called. The trace in M2 will show the real subset.

I/O: `I$Open I$Create I$Close I$Read I$ReadLn I$Write I$WritLn I$Seek I$Dup I$Delete I$MakDir I$GetStt I$SetStt`.

### I$SetStt / I$GetStt codes (D1) and sub-codes (D2)

Standard OS-9 codes: 0x00 SS_Opt, 0x01 SS_Ready, 0x02 SS_Size, 0x04, 0x05 SS_Pos, 0x06 SS_EOF.

CD-RTOS-specific codes. Names are guesses from usage **(verify all by trace)**:

| Code | Sub-codes seen | Used by | Guess |
|---|---|---|---|
| 0x31 | – | space, bumperanim (Set + Get) | |
| 0x3b | 0, 1 | launcher (Set/Get), tribes, space | |
| 0x3d | 0 | launcher, tribes, space (10 callers in space) | |
| 0x43 | – | launcher, nvrui (Get) | |
| 0x51 | 0 | launcher | |
| 0x52 | 2, 3, 4 | launcher, space | |
| 0x56 | 4, 0xb, 0xc, 0xe, 0x11, 0x12, 0x13 | everyone | likely display control (UCM) with a DC_ sub-function |
| 0x59 | 0, 3, 0x10 | launcher, space, bumperanim | |
| 0xd0 | – | nvrui (Get) | NVRAM / CSD related? |

I couldn't find published header values for these codes (`ucm.h`, `cdfm.h`). The M2 tracer will log arguments and the memory blocks they point at, which should identify them.

## Program flow (from strings and call sites)

- `cdi_launcher` creates or links data module `cdi_launch.res` (shared state, `F$DatMod`), reads `/nvr/csd` (Configuration Status Descriptor, which names the devices), checks for `/nvr/laserlords1.nvri` ("Saved Game Number 1"), runs `cdi_bumperanim` with `cdi_bumpdata`, and uses `F$Fork`/`F$Chain`/`F$Wait` to start `cdi_tribes`, `cdi_space` and `cdi_nvrui` (with `cdi_nvrui_res` / `cdi_nvruiparms`).
- No device names like `/video` or `/cd` appear as literals. Device names come from the CSD at runtime, so our `/nvr/csd` must provide them.
- `cdi_bumperanim` picks `pal_bumper.rtf` or `ntsc_bumper.rtf` (region check).
- `cdi_tribes` opens the planet files (`luxor.rtf` + `luxor_v.rtf` and so on), `anime.rtf`, `themes.rtf`, `messages.rtf`, `help.rtf`. It also uses an event named `line_event` (likely a line or field interrupt event for timing).
- `cdi_space` references `space.rtf`, `slides.rtf`, `anime.rtf`, `themes.rtf`, `messages.rtf`, plus **`brutus.rtf c3di.rtf dweeb.rtf jane.rtf jonjen.rtf katoi.rtf mokwun.rtf ooaa.rtf photon.rtf sam.rtf sarp.rtf thorax.rtf venga.rtf warga.rtf zendo.rtf zenobia.rtf`, which aren't on the disc**. Probably character names that index into `slides.rtf`/`messages.rtf`, or dead references **(verify)**.
- Leftover dev path in the launcher: `/d0/lords0.dsk`.

## Disc survey

The sector survey matches LASER_LORDS_NATIVE.md exactly (24 files, root directory only).

## M2: runtime trace findings

The runner boots `cdi_launcher` (the disc label's application), runs it with Musashi, and logs every TRAP #0. All three program stages run until they need real-time CD data.

### Process context (verified: cstart runs correctly with it)

Entry registers on fork: D0.w = process id, D1 = group/user, D2.w = priority, D3.w = inherited path count, D5 = parameter size, D6 = total data size, A1 = top of data area, A3 = module header, A5 = A7 = parameter string, A6 = static base + 0x8000, PC = module + M$Exec.
Data area layout: `[static | stack | params]`. **The kernel, not cstart, applies M$IData/M$IRefs**:
- M$IData: `long offset, long size, bytes...` copied to static base + offset.
- M$IRefs: two tables (code refs, then data refs). Each is groups of `word msw, word count, count × word lsw` ending at `0,0`. Each entry is a static-relative offset of a long, to which the module address (table 1) or static base (table 2) is added.
- The idata contains `jmp abs.l` stub tables, which run fine after relocation.

### Program flow

1. `cdi_launcher` reads `/nvr/csd` repeatedly (once per device lookup), opens `/video`, `/ptr`, `/audio`, builds the display, `F$Load`s `cdi_tribes` early, then loads and forks `cdi_bumperanim` with `cdi_bumpdata`.
2. `cdi_bumperanim` opens `pal_bumper.rtf`, links `line_event`, starts a play (signal 300), sleeps, and uses PCL signals 0x12d-0x130 to drive the animation.
3. The launcher creates data module `cdi_launch.res` (0x2fe bytes, shared state) and runs a **dispatch loop on byte +0x42**: 1 or 6 = run `cdi_space`, 0/2/3/5/10/11/13 = run `cdi_tribes`, 12 = quit, 7/8/9/0x14/0x15 = special handlers (NVRAM UI and so on, not traced yet). Programs set the byte before they exit.
4. `cdi_space` (intro / space travel) installs an intercept handler, plays `space.rtf` with a data PCL (signal 400), and runs a per-frame loop driven by signal 600 (vertical blank). It polls the pointer every frame. A counter up to 35,999 frames (12 minutes) triggers a timeout path (attract mode?).
5. `cdi_tribes` links `line_event` and `cdi_launch.res`, opens the devices, opens `luxor.rtf` + `luxor_v.rtf` (Luxor = first planet), seeks to 0x16800, plays with channel mask 0x8fe (data channels 1-7, 11), signal 2000, then waits for its PCL data.

**Where it stops:** every stage waits for real-time sector delivery into PCL buffers (M5). Video calls are accepted but not rendered (M4).

### Kernel facts verified by the trace

- `line_event` is a **system event** the game links (never creates). The runner creates it at boot and does Ev$Signl on it every PAL field (50 Hz).
- `F$UnLoad` is called in a loop until it fails. It must return E$MNF once the module is gone.
- `F$Sleep` with D0 = 0x80000000 means "0/256 s" (just yield). The high bit selects 1/256-second units.
- `F$SRqCMem` colour types seen: 128 and 129 (0x80/0x81), probably plane A/B video RAM **(verify)**. The runner currently allocates those from the general heap.
- `F$DatMod` is called with attr 0x8000, perm 0x1ff, type/lang 3 **(verify the meaning of type 3)**.
- `F$SetSys` is called with d0=0x7c d1=0x80000004 **(unknown, stubbed)**.

### CSD (Configuration Status Descriptor), `/nvr/csd`

Plain text, one record per line, CR-terminated: `type:/devname:params`. The game's csd library finds the Nth record of a type and returns the name (between the colons) or params (after the second colon).

| Type | Device | Params the game tests |
|---|---|---|
| 2 | audio | – |
| 3 | video | `TV` (width 384, else 360), `625` (PAL: 280 lines, else 240) |
| 5 | pointer | `CL="a"` / `CL="b"` |

The runner serves a built-in CSD (`2:/audio:`, `3:/video:TV,625`, `5:/ptr:CL="a"`) unless `nvram/csd` exists.

### CD file manager (SetStt on an open `.rtf` path)

Wrapper table in each module (bumperanim 0x174c-0x1890): codes 0x2d, 0x2f, 0x30-0x3a, 0x90/0x91 (GetStt 0x90).

| Code | Args | Meaning |
|---|---|---|
| 0x2D | A0 = PCB | **play** (verified by the signal protocol) |
| 0x31 | – | abort play (probably) |
| others | | not yet seen at runtime |

**PCB** (play control block):

| Offset | Size | Field |
|---|---|---|
| +0x00 | word | status: bit 0 = end of record, 0x10 = trigger, 0x8000 = error/end, 0x100 / 0x80 seen in space's handler |
| +0x02 | word | signal sent on status change (300, 400, 2000) |
| +0x04 | long | record count (decremented by the game) |
| +0x08 | long | channel mask (bumper 0x18001, tribes 0x8fe, space 0x1) |
| +0x0C | word | audio channel mask (bumper 0x8000 = ch 15) |
| +0x0E | long | video PCL list pointer |
| +0x12 | long | audio PCL list pointer |
| +0x16 | long | data PCL list pointer |

**PCL** (from bumperanim's code, partly verified): +0x06 next PCL, +0x0A buffer address, +0x0E buffer size (sectors?), +0x18 transfer count. Each PCL also carries its own signal (bumper uses 0x12d-0x130). **(verify offsets in M5)**

### Display control: video path, SetStt 0x56, sub-code in D2

Wrappers pass the path in D0 and args in D3/D4/D5/... Sub-codes seen: 0, 2, 4, 6, 8, 0xa, 0xc, 0xd, 0xe, 0xf, 0x10, 0x11, 0x13, 0x1c.
- **0x11: signal at next vertical blank**, D3 = signal (one-shot; cdi_space re-arms it from its intercept handler every frame with 600). Implemented.
- 0x4 with D4 = 0x24xxxx counting up: writing a run of display instructions **(verify)**.
- 0x2 with D4 changing every frame in cdi_space: likely page flip / FCT switch.
- Instruction words visible in the launcher's C: 0xC1800800, 0xC2000000, 0xDB00003F, 0x10000000 (NOP), 0x40000000|addr, 0xD9000001, 0x80FFFFFF. These are MCD212 display control program instructions, so the game builds FCT/LCT lists itself and hands them to the driver.

Other video codes: 0x51 (launcher, D4=0x13), 0x52 sub-codes 0-6 (launcher, space).

### Pointer: SetStt/GetStt 0x59

GetStt sub 0 = read pointer (polled every frame by cdi_space), SetStt sub 3 = set position (D3 = x<<16 | y, e.g. 0x16800e1 = 360,225), sub 4 and 0x10 also used.

### Audio device: SetStt 0x3d / 0x3b

0x3d sub 0 with D3 = 0xff00ff (attenuation, muted?) on open. 0x3b sub 0/1 Set and Get on the audio path.

## M3: kernel subset

All kernel calls the traces use are implemented in `runner/src/os9.c`, and the file layer in `runner/src/io.c`. `tests/test_kernel.c` runs hand-assembled 68000 programs through the real scheduler (fork/wait/exit, intercepts, alarms, timed sleep, events within and across processes, memory, data modules) plus C-level tests of the NVRAM file layer and the disc.

### Semantics implemented (and tested where marked ✓)

- **Signals ✓:** S$Kill (0) is fatal and cannot be intercepted. S$Wake (1) only wakes and is never queued. Every other signal is queued and goes to the intercept routine, or kills the process (exit status = signal number) if it has none. Pending signals are delivered **on the way out of the system call** (sending to yourself runs your handler before F$Send returns). Signals arriving inside the handler wait for F$RTE. A sleeper woken early gets D0 = ticks left.
- **Process exit ✓:** closes paths, frees the data area, drops the module link, and cancels the process's alarms, vertical-blank signal and pending plays. A child that exits before its parent waits stays a zombie until F$Wait collects it. F$Wait with no children returns E$NoChld (0xE2) ✓.
- **Modules ✓:** link counts are tracked. At zero links, if no live process runs it, the module is freed. This is what the launcher's `F$UnLoad`-until-error loops rely on. F$UnLink accepts any address inside a module. F$DatMod always creates type Data (language from D3).
- **Module CRC ✓:** `os9_crc` (CRC-24, polynomial 0x800063, initial 0xFFFFFF) gives the OS-9 check value 0x800FE3 over all five program modules on the disc. So F$CRC / F$SetCRC are exact.
- **Coloured memory ✓:** F$SRqCMem colour 0x80 = plane A bank (0xA00000), 0x81 = plane B bank (0xB00000), 512 KB each, falling back to general RAM if a bank fills. Colour 0 = any. **(verify the A/B order against the display setup in M4)**
- **F$SetSys:** the only use is a 4-byte read of system global 0x7C at launcher start. The runner returns the tick count **(verify which global this is)**.
- **Alarms ✓:** A$Set / A$Cycle (D3 = ticks, or 1/256 s with bit 31 set), A$Delete.
- **Events ✓:** Ev$Link / UnLnk / Creat / Delet / Wait / WaitR / Read / Info / Signl / Pulse / Set / SetR, with wait/signal increments, and wake-one or wake-all (bit 15 of the function code).

### NVRAM file layer

The launcher's own save and load paths (states 7 and 8) work end to end against the host folder.
- **Save file** `/nvr/laserlords0.nvri`: 8-byte header, a 0x2ba-byte copy of `cdi_launch.res` data at offset 0x22, then a checksum pass over the file. The first word (2 bytes) is rewritten last. About 730 bytes in total.
- **Free space:** `GetStt 0x43` on a `/nvr` path returns **free bytes in D2**. If it's too small for the save, the launcher opens the save-management UI (`cdi_nvrui`). The runner models the real **8 KB** NVRAM (file sizes + 32 bytes per entry + the CSD).
- **Directory reads** (C library `opendir`/`readdir`): open `/nvr` with mode 0x81, `SS_Opt` byte 0 = device type. DT_CDFM (5) gives CD-format records; anything else gives **RBF 32-byte records**: name with the last character's high bit set, a zero first byte = deleted entry, 3-byte FD sector at +0x1D. The runner reports DT_RBF for `/nvr`, DT_CDFM for disc files and DT_UCM for devices, and synthesizes `..`, `.` plus one record per host file.
- **`SS_FD` (GetStt 0x0F):** RBF file descriptor. The launcher uses attributes (byte 0, bit 7 = directory) and the 5-byte modification date at +3 (yy mm dd hh mm) to show save dates. Served from the host file's stat.
- **Developer leftover:** state 20 tries `I$Create "/d0/lords0.dsk"` (a dev save to floppy). That fails with E$BMode, which is harmless.

### Still stubbed (owned by later milestones)

GetStt 0x59 (pointer, M6), GetStt 0x3b on audio (M5), every video SetStt (M4), and SS_Play data delivery (M5). The kernel no longer has unimplemented calls on any traced path.

## M4: video

**Result:** the AIM bumper (logo + flying disc) and the Laser Lords title screen render correctly, at 50 fields/s in the window or headless with PNG dumps. The title screen then waits for pointer input (M6).

### Display model (`runner/src/video.c`)

The UCM driver keeps **FCTs** (field control tables, run once per field) and **LCTs** (line control tables, 8 instructions per line) per plane. Each field: run plane A and B FCTs, then for each of 280 lines run that line's LCT instructions for both planes, decode a line of pixels per plane from its VSR, and mix. Instruction set and pixel rules follow the MCD212 (reference: MAME `mcd212.cpp`):
- **Instructions:** 0x0X stop, 0x1X nop, 0x2X/0x3X reload DCP (+stop), 0x4X/0x5X **load VSR** (+stop), 0x6X **interrupt**, 0x78 display parameters (low bits: file type 0/1 bitmap, 2 run-length, 3 mosaic; mosaic factor), 0x80-0xBF CLUT entry (bank via 0xC3), 0xC0 image coding (plane A bits 0-3, plane B bits 8-11: 1 CLUT8 / RGB555 on B, 3 CLUT7, 4 CLUT77, 5 DYUV, 0xB CLUT4), 0xC1 transparency, 0xC2 plane order, 0xC4/0xC6 transparent colour, 0xC7/0xC9 mask, 0xCA/0xCB DYUV start value, 0xD0-D7 matte, 0xD8 backdrop, 0xD9/0xDA mosaic hold, 0xDB/0xDC plane weights (0-63).
- **VSR addresses are the game's own 24-bit addresses** (from F$SRqCMem), used directly.
- **Mixing:** `(c-16)*weight/64` per plane summed, then studio-to-full range (`*298/256`). Fades are done by the game rewriting weight instructions.

### UCM display-control sub-codes (SetStt 0x56, D2)

| Sub | Args | Meaning | Status |
|---|---|---|---|
| 0x00 | D3 plane, D4 length | create FCT, returns id in D0 (FCTs numbered 0, 1, ... separately from LCTs) | ✓ (launcher: 350 for A, 32 for B) |
| 0x06 | D3 plane, D4 lines | create LCT, returns id | ✓ (484 lines each, plus 40-line pair) |
| 0x01 / 0x02 | D3 FCT, D4 start, D5 count, A0 buffer | read / write FCT | ✓ |
| 0x04 | D3 FCT, D4 index, D5 instruction | write one FCT instruction | ✓ (fades) |
| 0x0A | D3 LCT, D4 line, D5 column, D6 instruction | write one LCT instruction | ✓ |
| 0x0C | D3 FCT, D4 LCT, D5 line | link FCT to LCT line | ✓ (FCT 1→LCT 3, 2→4) |
| 0x0D | D3 LCT, D4 line, D5 LCT2, D6 line2 | link LCT line (self-link = end) | inferred |
| 0x0E | D3 FCT A, D4 FCT B | execute these FCTs | ✓ |
| 0x11 | D3 signal | signal at next vertical blank (one-shot) | ✓ |
| 0x1B / 0x1C | D3 LCT, D4 line, D5 col, D6 lines, D7 cols, A0 | read / write LCT block | ✓ |
| 0x10 | same, no buffer | fill block with NOP | inferred |
| 0x03 / 0x09 | D3 id | delete table | inferred |
| 0x05, 0x0B, 0x0F, 0x12, 0x13, 0x07, 0x08 | single arg / block | unknown, logged and ignored | **(verify)** |

**INTERRUPT instructions in an LCT pulse `line_event`.** The bumper puts one at line 100. `line_event` is waited on with `Ev$Wait [1..1]`, so the driver sets it to 1 and back to 0 (a pulse); without any interrupt instruction it pulses once per field.

The graphics cursor (SetStt 0x52) implementation (sub 0 position x<<16|y, 2 show, 6 hide, 3 pattern from A0, 5 colour) is inferred **(verify in M6)**. The pointer (GetStt 0x59) is read on the **video** path too: the UCM handles pointer and display.

### Real-time play (`runner/src/cdfm.c`, pulled forward from M5)

- The PCB's video/audio/data fields each point to a **32-entry table of PCL pointers indexed by channel**. Sectors stream at **75/s** from the path's position (bytes / 2048 = sector). A sector is routed when its channel bit is set in PCB+8; the submode bit picks the video, audio or data table.
- **PCL** = `+0 ctrl, +1 submode, +2 coding, +4 signal, +6 next, +0x0A buffer, +0x0E size in sectors, +0x18 sectors filled`. Sector N goes to `buffer + N * sector_len` (2324 Form 2, 2048 Form 1). When full, the PCL signal is sent and delivery moves on to `next`. (Superseded by M5 below: ctrl uses bit 0, not 0x80, and the table entry advances.)
- **PCB status:** trigger bit in the submode → |= 0x10 + PCB signal. End of record → records--; at 0 the play stops with |= 0x01 + signal. End of file → |= 0x8001 + signal. This matches the bumper's handler exactly (signals 301, 300, 302×8, 303, 304, 300).
- Audio sectors are routed to audio PCLs when the game has them; audio output is M5.

### Disc image formats seen

- `slides.rtf`: 95 DYUV stills, 384×240, 40 sectors each, video channel 16 (the intro story). The offline decoder (`tools/video.py`) matches the C decoder's rules.
- `pal_bumper.rtf`: DYUV logo (plane B) + RL7 animation (plane A, CLUT7, run-length).
- `anime.rtf`: game-specific container. Records start `NS07` with an offset table, followed by large records of compressed DYUV delta frames. The game unpacks these itself, so the runner needs no decoder.
- Video coding byte 8 in `anime.rtf` is just a label; the game interprets the data.

### Known gaps

- The title art sits 40 lines down (lines 40-279), while the AIM logo fills from the top. Both come straight from the game's LCT line numbers. Checked: a 20-line offset is wrong. The game's display program links a 40-line header LCT (weight 0, black) before the 484-line picture LCT, and the 240-line images (0x16800 bytes DYUV) end exactly at line 279; shifting the visible window 20 lines down shows leftover memory under the cockpit panel and clips the logo. So the game really leaves lines 0-39 black on PAL (a TV's overscan hid part of it). Over 15 minutes only cdi_bumperanim (the AIM logo) and one warp transition (noise) draw there; the front end crops those lines except while cdi_bumperanim runs.
- Intro length: slides.rtf (LBN 120952-130119, ~122 s at 75 sectors/s, 1 audio sector in 4 = continuous level-B stereo) is real time; the whole intro is ~2.5 min. cdi_space reads the pointer only during the logo and title (pressing button 1 at ~18-26 s skips to the game); during the slideshow it never calls pt_coord. The runner's skip (input.c / cdfm_skip) ends a play with EOR|DONE when a button is pressed while a play runs and the pointer hasn't been read for 1 s; the game then continues as after a natural end. Half-filled PCLs are completed first. Restricted to cdi_space: cdi_tribes keeps plays running for scenery and conversations, and a user reported a freeze while talking to someone with the unrestricted skip (not reproduced).
- **Gameplay speed:** cdi_tribes paces everything off `F$Alarm A$Cycle sig 2500 every 2 ticks` (handler FUN_6dea decrements countdown timers; the main loop runs the player update FUN_b98c when timer -0x56c0 reaches 0, and it is mostly reset to 1). With a 100 Hz tick the warrior moves ~6 px per update = ~300 px/s, far too fast; footstep effects are re-triggered every 3 updates. The CD-i ROM (cdi220b, Mono-I PAL) settles the tick rate: `init` names clock module `sgstom`, which attaches /tim070 and sets D_TckSec ($28 in the system globals) to 100; `tim070driv` programs the 68070 timer (0x80002020, reload 0xFA4E) and jumps to the kernel tick routine. So 100 ticks/s and a 50 Hz game alarm are what the code asks for; per field the warrior moves 5-11 px every second field (~200 px/s). The game needs < 1 MHz of 68000 time per update, so real hardware should not be CPU-bound either. **Measured in MAME 0.289** (cdimono1, cdi220b, scripted mouse via Lua): on Luxor the warrior steps every 5 fields (10 Hz), 10-24 px on MAME's 768-wide screen (5-12 px in 384 terms), ~75 px/s; the runner at 100 % steps every 2 fields (~200 px/s). At 40 % the runner steps every 5 fields with the same step sizes, so the window default is 40 %. The real machine is held back by something we don't model (probably the ROM's UCM display driver), not by its clock. MAME also confirms the black top border on the real machine. The runner adds a game-speed factor (`ll_game_speed`, 25-100 %) that slows the kernel tick clock only; real-time play (cdfm) and fields keep their own real-time counters. Window default 50 %.
- Sound maps: replacing a playing effect wrote "done" into the new effect's status word (fixed: the old effect reports to its own status). A play with an empty or invalid group range now reports done at once instead of never.
- Graphics cursor: cdi_space sets pattern (sub 3), colour (5), 4, position 0 (sub 0, d3=0), 6, 1 (d3=6) once and never moves it, so no visible cursor is expected; menus use a blinking highlight.
- The bumper exits with status 0xA6 because a PCL signal interrupts its `Ev$Wait`. The launcher ignores it. **(verify the OS-9 behaviour of signals during event waits)**
- No interlace / high-resolution modes yet (the game hasn't asked for them).

## M5: audio and real-time play

**Result:** sound works. The AIM jingle, the title music (`space.rtf` channel 15) and the narrated intro slideshow (`slides.rtf`, about 2 minutes) all play in sync with the pictures, in the window (SDL audio) or into a WAV (`--headless --wav FILE`).

### ADPCM (`runner/src/audio.c`)

- CD-i ADPCM levels A/B/C. A Form 2 audio sector holds 18 sound groups of 128 bytes (16 header bytes + 112 data). 4-bit: 8 units × 28 samples; 8-bit: 4 units. Unit parameter: low nibble shift, bits 4-5 filter (K0 = 0, 60, 115, 98 / K1 = 0, 0, -52, -55, /64). Coding byte: bit 0 stereo, bit 2 = 18.9 kHz, bit 4 = 8-bit.
- **Verified bit-exact** against `tools/xa.py` on 20 sectors of `themes.rtf` (stereo 37.8 kHz 4-bit) and `messages.rtf` (mono 18.9 kHz 4-bit) (tests).
- Output: one FIFO at 37,800 Hz stereo (18.9 kHz doubled). SDL resamples for the device; latency is capped at 0.25 s.
- **Attenuation** (SetStt 0x3D on `/audio`): D3 = LL.LR.RR.RL bytes, 0 = full, ≥ 0x80 = off. The game always sends 0x00FF00FF (plain stereo). The 0.5 dB step is assumed **(verify)**.
- Real-time playback: a sector is played when its channel is in the PCB audio mask (PCB+0x0C, 16 bits) and it's an audio sector. Decoder state is kept per play.

### Corrections to the play engine found while getting audio right

1. **PCL control byte:** set **bit 0** when a buffer is done. **Bit 7 means error**: `cdi_space` aborts the play when the byte is negative. This was the cause of the "title music cuts off" bug.
2. **The PCB table entry is the "current PCL"** and advances to `PCL_Nxt` when a buffer completes. `cdi_space` re-reads the entry before each frame and processes only the newest completed buffer; the others are skipped, by design. Previously every frame was dropped from the ring, which left ghost images in the title zoom.
3. **The chain is walked strictly in order.** Skipping a busy PCL broke the 6-sectors-per-frame alignment of the title animation and turned the game's frame-patch offsets into garbage that overwrote the PCB.
4. **PCB status bits** (from both games' handlers): 0x01 end of record, 0x10 trigger, **0x80 play complete** (records used up), **0x100 end of file**, 0x8000 error. `cdi_space` waits for 0x80/0x100, the bumper for 0x01 with records = 0.
5. **Allocator:** `F$SRtMem` of video-bank memory went to the general heap. Frees now go to the owning region.
6. **Display table ids:** FCTs and LCTs are numbered separately from 0. `cdi_tribes` never creates tables; it writes into FCT 0 / LCT 0, the launcher's plane A tables.
7. **Pointer** (pulled forward from M6): `pt_coord` = GetStt 0x59 sub 0 returns **buttons in D0.w, x<<16|y in D1**. Leaving D0 untouched made the game see a button held (path number 7) and skip the intro. `pt_pos` = SetStt 0x59 sub 3. Implemented in `runner/src/input.c`; mouse mapping is M6.

### Intro sequence as it now plays (no input)

AIM bumper → `cdi_space`: `space.rtf` data plays → title: the "LASERLORDS" logo zooms in over a starfield with music (records 1-2 of `space.rtf` at LBN 8834) → copyright screen → story slideshow (`slides.rtf`: 95 DYUV stills with animated sprites, crossfades, music + narration on channel 15; triggers mark slide ends) → further `space.rtf` plays (attract loop?).

### Timing knobs

`--cpu-mhz N` (default 15) and `--field-hz N` (default 50). The title animation's buffer skipping doesn't depend on either, so the game paces itself by vertical blank and newest-buffer logic, not CPU speed.

### Still open

- `/audio` SetStt/GetStt 0x3B sub-codes 0-4 (wrappers exist in tribes, probably sound maps for effects): only seen once in the launcher so far. Implement when gameplay reaches them (M6+).
- CDFM codes 0x2F, 0x30, 0x32-0x3A, 0x90/0x91 haven't been called yet.

## M6: input and saving

**Result:** the game is playable from the title onward. Title → orbit menu (Beamdown / Hyperspace / Computer) → planet Luxor, with the warrior walking, turning and crouching. Saving and loading go through the game's own Computer → Save / Load Game → Game 1-3 screens. Sound effects (sound maps) play.

### Controls (window)

| Action | Mouse | Keyboard | Gamepad / Steam Deck |
|---|---|---|---|
| Move pointer / walk | move (click once to capture, Esc releases) | arrows or WASD | left stick or D-pad |
| Button 1 (select / action) | left click | Space, Enter, Z | A |
| Button 2 | right click | X, Backspace | B |
| Quit | – | Cmd+Q / Ctrl+Q | – |

Saves go to the per-user folder (`SDL_GetPrefPath("LaserLords","LaserLords")/nvram`): `~/Library/Application Support/LaserLords/LaserLords/nvram` on macOS, `~/.local/share/LaserLords/LaserLords/nvram` on Linux / Steam Deck, `%APPDATA%\LaserLords\LaserLords\nvram` on Windows. `--nvram DIR` overrides.

Headless scripted input for tests: `--input T:b1|b2|b3`, `--input T:m:DX,DY` (move), `--input T:s:SX,SY,DUR` (hold stick). Example that reaches the planet and walks: `--input 22:b1 --input 45:b1 --input 60:s:1,0,3`.

### Pointer semantics (verified against the game code)

- `pt_coord` (GetStt 0x59 sub 0): D0.w = buttons, D1 = x<<16|y in **double-resolution units (768×560)**. Buttons: bit 0 = 1, bit 1 = 2, **bit 15 set while a button is down** (cdi_tribes compares with 0x8001 / 0x8002; cdi_space masks with 3).
- `pt_pos` (SetStt 0x59 sub 3) moves the pointer. SetStt 0x59 sub 4 (x,y) is unknown and ignored.
- **Menus** (cdi_space): absolute pointer position selects the item; moving right moves the highlight. Any pointer change resets the idle timer.
- **Planet** (cdi_tribes, FUN_1016C): a joystick. It re-centres the pointer to (0x168, 0xE1) on every read. x < 0x155 = left, > 0x17C = right; y < 0xCE = up, > 0xF5 = down. That gives numpad-style direction codes (0x47-0x51). Button 1 alone → 0xCF; button 2 + direction → special moves. The runner therefore reports pointer + a deflection offset (64 units at full stick/key) + a decaying mouse offset.

### Sound maps (SetStt/GetStt 0x3B on /audio, `runner/src/soundmap.c`)

- SetStt sub 0 = create (D3 = coding, 4 = mono 18.9 kHz 4-bit; D4 = 0x12; A0 = buffer) → D0 = id. GetStt sub 0 (D3 = id) → **A0 = descriptor**, which the game fills: +2 first sound group, +4 last, +6 repeat, +0x0A buffer, +0x0E size, +0x1A groups, +0x20 per-effect pointer. The launcher stores id/descriptor in `cdi_launch.res` +0x38/+0x3C.
- SetStt sub 1 = play (D3 = id, A0 = status word: the game writes 0, the runner writes 1 when the effect ends). Sub 2 = stop. Sub 3/4 and GetStt sub 1: not seen yet.
- **This was a real bug:** with the old stub GetStt returned the caller's A0 (the `cdi_launch.res` data), so cdi_space wrote its sound descriptor over the shared display-table ids, and cdi_tribes drew into a non-existent FCT (0x2AC).
- Effects are mixed over CD audio on one timeline that's filled up to emulated time every field.

### `cdi_launch.res` layout (from the launcher, +0x00..)

`+00 FCT A, +04 LCT A, +08 LCT A (40 lines), +0C byte, +0E FCT B, +12 LCT B, ... +2C/+30/+34 video buffers, +38 sound-map id, +3C sound-map descriptor, +42 launcher state byte, +45 byte (copied by space/tribes)`.

### More play-engine facts found in M6

- **PCB status 0x100 = play stopped** (cdi_tribes clears its "busy" flag only on it); 0x80 = end of file. These were swapped before.
- **Abort** (SetStt 0x31) still signals the PCB (status 0x100): cdi_space waits for it after the title is skipped. A new play replacing an old one stays silent.
- A PCL also completes at **end of record**, not only when full (Luxor's data module is a 3-sector record).

### Open

- The planet screen leaves the bottom ~60 lines black. That's possibly a status panel that appears on demand, or something not yet emulated. Check with real footage or by exploring.
- Cursor sub-codes (SetStt 0x52) are still partly guessed; no visible cursor is needed so far.

## M7: playthrough and polish

- **Demo playthrough:** Computer → Demo runs unattended for 5+ minutes with no errors (alien video calls, star map, orbits of other planets). Known glitch: during some demo transitions the cockpit panel shows ~0.8 s of coloured noise. The cockpit-light CLUT updates (FCT 0 from index 0x6D) read colour tables that streamed animation data has just overwritten. **(open)**
- **Direct CHD support:** vendored libchdr (BSD, static, bundled zstd/lzma/miniz). `disc.c` reads `.chd`, `.cue` (→ `.bin`) or `.bin`. The tests pass on both (`ctest`: `kernel`, `kernel_chd`).
- **Front end:**
  - `settings.txt` in the per-user folder (absolute disc path, fullscreen, scale mode).
  - Native file dialog when no disc is known.
  - 4:3 output (768×576 logical) with "sharp" scaling (integer nearest + linear fit); F2 cycles sharp/nearest/linear.
  - F11 / Alt+Enter / Cmd+Enter / Cmd+F toggle fullscreen; starts fullscreen on Steam Deck (`SteamDeck=1`) or when last used. Pauses while minimised.
- **macOS:** `LaserLords.app` target (original icon from `tools/make_icon.py`, Info.plist, SDL3 dylib copied into `Contents/Frameworks` and relinked to `@rpath`, ad-hoc signed). 4.3 MB; `scripts/build-macos.sh` adds a .dmg. Homebrew SDL3 is arm64-only, so `UNIVERSAL=1` builds SDL3 from source for arm64 + x86_64.
- **Portability:** `runner/src/compat.h` maps mkdir/ftruncate/localtime_r/access/strcasecmp/dirent to Windows (MinGW and MSVC; minimal dirent for MSVC). SDL3 3.4.14 is fetched and linked statically when not installed (`LL_BUNDLED_SDL`). When cross-compiling, the Musashi opcode generator is built and run with the host compiler at configure time.
- **Windows:** cross-built with MinGW-w64 (`packaging/windows/mingw-toolchain.cmake`, static runtime). `LaserLords.exe` (GUI, icon) and `laserlords-cli.exe` (console) import only system DLLs. **Compile-verified only, not run yet.**
- **Linux / Steam Deck:** `packaging/linux/Dockerfile` (Ubuntu 22.04 x86_64, static SDL3 with X11/Wayland/Pulse/PipeWire/ALSA back ends loaded at run time), `scripts/build-linux.sh`. **Verified in an x86_64 container:** 53/53 tests pass on the CHD, and a scripted headless run reaches cdi_tribes. The binary needs only libc/libm (glibc ≥ 2.34; SteamOS 3 has newer). Not yet run on a real Deck or in a desktop session.
- **Universal macOS:** `UNIVERSAL=1 scripts/build-macos.sh` gives an arm64 + x86_64 app with SDL compiled in (4.4 MB dmg). It runs natively and under Rosetta. Not notarized: on other Macs, right-click → Open the first time (or `xattr -dr com.apple.quarantine LaserLords.app`).

Packages built: `dist/LaserLords.app`, `dist/LaserLords-macOS.dmg`, `dist/LaserLords-linux-x86_64.tar.gz`, `dist/windows/LaserLords.exe` (+ `laserlords-cli.exe`).

Still open after M7: see "Visual issues" below.

## Visual issues (investigated after M7)

### Black strip under the planet scene: fixed

The cause was missing **matte (region) control**. The MCD212's 8 matte registers (0xD0-0xD7, "at x = X do OP": 0 stop, 4/6 set plane A/B weight, 8/9 reset/set a matte flag, 12-15 both) drive per-pixel matte flags and weights. Transparency modes 3-6 (MF0, MF1, MF0 or key, MF1 or key) test those flags. Implemented in `video.c` (`matte_line`, recomputed every line; weights used per pixel in the mix).

Tribes uses them for its screen frame. The playfield (rows 0-175) is plane A CLUT8. At row 176 the game sets plane A's weight to 0 and `D1900040` + `D2000042` (matte flag 0 set from x = 64, stop), so plane A turns transparent and plane B shows the carved stone frame and the relief panel under the playfield. Its "show message panel" routine (FUN_ADC4/AE5E, reached via jump table -$29CE) instead writes `DB00003F C1800805 D180FC40 D290FEBE D30002C0` to row 176. That makes plane A opaque between x = 64 and 702: the dark message box (plane A rows from a 60-line, 384-byte-per-line buffer) inside the stone frame. Verified by rendering both states.

**Build pitfall found on the way:** the CLI target `laserlords` and the app target `LaserLords` collided on macOS's case-insensitive file system (same `CMakeFiles/*.dir`), so `build/laserlords` silently stopped rebuilding after the app target was added. The app target is now `LaserLordsApp` (output still `LaserLords.app`).

### Cockpit-panel noise during warp crossfades: explained, not changed

- **Where:** only during warp/approach crossfades (demo: ship → warp → planet). In ~5 minutes of demo, 11 stretches, the longest 266 fields (5.3 s).
- **Mechanism, from field dumps:** in this mode FCT 0 sets `C0001515`, so **both planes are DYUV**; normally it's `C0001511` (plane A CLUT8). Plane B's rows run continuously from b00000 to the bottom (a full-screen DYUV image). Plane A's line table reloads the VSR to the panel art (a11700, 54 rows, **CLUT8 data**, intact in memory) from row 186. While A's weight is 0 (fields before and after the fade) the screen is clean; as the game crossfades A in (weights e.g. 29/34, stepped once per animation frame), A's lower rows decode CLUT8 data as DYUV, which is the noise.
- **Checked and ruled out:** data corruption (the panel bytes are intact); a per-line coding switch (no 0xC0 in any LCT, and the FCT has one C0); a different display buffer (the game flips the top LCT's tail link between LCT 0 and LCT 5, and both point rows 186+ at the panel); mattes (only "stop" ops in this mode); CPU speed and field rate (pacing comes from the stream).
- **Conclusion:** the runner executes the game's display program as the MCD212 is documented (and as MAME implements it), and that program produces the noise. Either the original shows it too, or some undocumented hardware/driver behaviour hides plane A's lower rows during the fade. Real-hardware footage of a warp would settle it. A targeted cosmetic workaround is possible: while plane A is DYUV, treat rows whose VSR jumps back below the field's first plane-A VSR as transparent. **Now applied as an opt-out cosmetic fix** (setting `fix_crossfade`, default 1; `--accurate` disables it). Rule, in `video.c`: while **both** planes are DYUV, rows of a plane whose VSR was reloaded by its line table to an address below the first VSR that plane got this field are treated as transparent. The other plane shows there. Verified on a 5-minute demo run against `--accurate`: noise streaks in the panel rows went from 546 fields to 0. Every differing frame differs only in rows ≥ 226, and frames outside the crossfades are byte-identical.


## Timing against the real CD-i (MAME)

MAME 0.289 (`cdimono1`, ROM cdi220b + the 68HC05 MCU dumps) runs the real ROM and disc; Lua scripts click PLAY CD-I, press buttons and move the mouse, and log snapshots or per-frame brightness. The game starts ~19 s later than in the runner (BIOS boot).

- **F$Sleep** (ROM kernel at kernel+0x57b4): D0 = 0 sleeps until a signal; **1 only gives up the time slice** (the process stays ready); n sleeps n-1 ticks; bit 31 = 1/256 s units. The runner used to sleep a full tick for 1, which made cdi_space's fade loop (5 x F$Sleep(1) per weight step) take 3.2 s per fade, so the game dropped most of the intro images (27 of 39 in slides.rtf).
- **Display driver cost:** with F$Sleep(1) as a yield and free system calls, fades became instant. The ROM's UCM driver costs real CPU time, so the runner now charges every display/pointer call `ll_ucm_base` = 50,000 cycles plus `ll_ucm_entry` = 1,100 per display-table entry written (video.c), taken from the Musashi time slice. Calibration: intro fade 24 fields (MAME ~0.4-0.5 s), planet walk a step every 5 fields at 79 px/s (MAME every 5 fields, ~75 px/s). This also makes the planet pace right without slowing the kernel clock, so the speed factor (F4) is now an optional extra.
- Intro compared every 4 s against MAME: same images in the same order, both reach "Orbiting: Luxor" together.
- **Input recording:** the window writes `session.llrec` (pointer state on every change, stamped with the emulated cycle); `--replay` reproduces a session exactly (verified: 19/19 frames identical between a scripted run and its replay).

## Conversation freeze (fixed)

Reproduced from the player's recorded session (session.llrec): pressing button 1 next to an NPC made cdi_tribes stop making system calls. LL_PCHIST_FROM / LL_DUMP_FROM (os9.c) showed it inside the text renderer (tribes+0x19a46, glyph bit expansion into a CLUT4 bitmap) with a glyph source pointer near address 0, glyph width 0 and a garbage height: the font was never set up, so drawing one line took ~10^10 iterations.

The font (`all/atnc24cl.ai1`, 3 sectors) is played from luxor.rtf into a buffer; sub_12b24 accepts it only if the PCL shows no error and **PCL_Cnt (+0x18) > 0x17FF**, i.e. it counts **bytes** (3 x 2048 = 6144). cdfm counted sectors, so the font was rejected. (The I$Open of "all/atnc24cl.ai1" fails on the real machine too; it is a leftover, the font comes from the disc stream.) With PCL_Cnt in bytes the action menu (Hail / Name / Word / Offer / Tools / Recite / Pickup / Status / Help / Beamup) draws at once; the intro still matches MAME.

## Conversation screen not shown (fixed)

Second report from the player (session.llrec): next to the old lady, button 1 opened the action menu but nothing appeared; the game sat in its menu loop (tribes+0x16286, waiting for release / input). LL_PLANE_ONLY=a|b (video.c) showed the conversation screen ready in plane B (portrait, text area) and plane A's scene on top. The session was converted to timed mouse moves and replayed in MAME (scratch replay.lua): the real CD-i follows the same route and shows the menu bar in rows ~214-276, i.e. LCT 0 line 175 at y = 40 + 175, so the display chain was **not** jumping to LCT 6 there.

The ROM's dc_llnk (video driver sub 0x0D at driver+0x4d98) writes the link as a "reload DCP" instruction (0x3xxxxxxx) into **column 7** of the source line; dc_llnk(lct, line, lct2, line2) = lct.line col 7 -> lct2.line2. cdi_tribes rewrites column 7 of LCT 0 lines 14-173 (dc 0x1c, 160 lines x 1 column) to scroll, which drops the split-screen link, and re-links (120 dc_llnk calls on Luxor) when it wants it. The runner kept links in a side table that writes never cleared. Now dc_llnk also NOPs column 7 and any write to column 7 clears the link. With that the menu bar shows, Hail opens the conversation ("What's this? Another Starman? ...") with the old lady's portrait, and her voice plays (messages channel 15, 18.9 kHz).

Debug switches added on the way: LL_TRACE_FIELDS=1 (field markers in the trace), LL_PCHIST_FROM=N (hot-PC statistics from field N), LL_DUMP_FROM=N + LL_PEEK=addr,... (register / memory dumps every 50 fields), LL_PLANE_ONLY=a|b, LL_REPLAY_SHIFT / LL_REPLAY_SHIFT_FROM.
