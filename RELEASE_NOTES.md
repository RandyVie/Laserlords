# Laser Lords native runner: release notes

## v0.8.0 (preview)

First public preview. Plays the Philips CD-i game Laser Lords (1992) natively on macOS,
Windows, Linux / Steam Deck and Nintendo Switch (homebrew), with your own disc image and
without a CD-i emulator or BIOS.

**Works and checked against the real CD-i** (MAME running the original ROM):
- Intro and title: same images, fades and timing; skippable with any button.
- Planet gameplay (Luxor): walking pace, smooth scrolling, screen placement.
- Action menu and conversations (text and voices), sound effects and music.
- Saving and loading through the game's own Computer screens.

**Platforms:** universal macOS app (Intel + Apple Silicon, macOS 11+), Windows x64, Linux
x86_64 (also Steam Deck), Nintendo Switch homebrew (.nro). Tested on macOS and a Switch; the
Windows and Linux builds pass the tests but haven't been run on real machines yet.

**Known limitations**
- Only the start of the game has been played through; later planets, combat and puzzles may
  hit system features that aren't implemented yet.
- The macOS app isn't notarised and the Windows exe isn't signed: see READ ME FIRST for how
  to open them.
- No graphics cursor on screen, as on the original (menus use a blinking highlight).

**Reporting bugs:** attach `session.llrec` (your input only) to an issue; it replays the
session exactly. See the README.
