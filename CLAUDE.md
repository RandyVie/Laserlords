# Laser Lords native runner: notes for Claude Code

High-level emulation of the CD-i game Laser Lords: the original 68000 code runs on Musashi, the
CD-i system (CD-RTOS/OS-9, CD file manager, MCD212 video, ADPCM audio) is reimplemented in C with
SDL3. Read **GUIDE_NEW_GAME.md** before starting new work or a new game; NOTES.md is the research
log (append root causes there).

Rules:
- Never commit game data, decompiled game code, BIOS ROMs, traces, logs or session recordings
  (.gitignore covers them; still check `git diff --cached --name-only`). Commit and push only when
  the user asks.
- Verify behaviour against the real CD-i instead of guessing: MAME with the user's BIOS
  (`tools/mame/`), the ROM itself (`tools/rom/`). Most bugs were a wrong unit or a missed side
  effect in the reimplementation.
- For a bug, have the user turn on input recording (`record_session=1` in settings.txt; off by
  default), reproduce it and quit, then replay `session.llrec` from the data folder headless
  (`--replay`) with a scratch copy of the NVRAM folder.
- Cosmetic workarounds go behind a setting; `--accurate` turns them off.

Build and test:

```bash
cmake -S . -B build && cmake --build build -j && (cd build && ctest)
open build/LaserLords.app      # the user tests this; packages: scripts/build-*.sh
```

Data folder (settings, saves, session recordings): `~/Library/Application Support/LaserLords/LaserLords/`.
