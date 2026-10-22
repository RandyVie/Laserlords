# Laser Lords (CD-i, Europe): native macOS runner

## Goal

Make Laser Lords (Spinnaker, 1992, Philips CD-i) playable natively on macOS (Apple Silicon), without MAME or any other full-system emulator. Every existing CD-i emulator crashes on this game, so the plan is a **high-level emulation (HLE) runner**:

- Run the game's original 68000 code unchanged inside an embedded 68000 CPU core.
- Replace the CD-i hardware and the CD-RTOS operating system (an OS-9/68k derivative) with native implementations of only the system calls this game actually uses.
- Graphics, audio, input and saving go through SDL3.

Think ScummVM, not MAME. We do not emulate chips cycle by cycle; we implement the OS API the game talks to.

A full decompile to C (Ghidra) is a possible phase 2, informed by everything the runner teaches us. Not in scope now.

## Ground rules

- Target: macOS on Apple Silicon. Language: C17. Build: CMake. Libraries: SDL3 (`brew install sdl3`), Musashi 68000 core (https://github.com/kstenerud/Musashi, vendor it, check its license).
- Python 3 for offline tooling (extraction, asset decoding, analysis).
- **Never commit game data.** No .chd, .bin, extracted modules or ripped assets in the repo. The user supplies their own disc image; `.gitignore` the data folders from day one.
- Work in milestones. At the end of each milestone, stop and report what works, what doesn't, and what was learned about the game before moving on.
- When something about the CD-i or OS-9 is uncertain, verify it against the binary or a trace rather than assuming. Notes marked **(verify)** below are educated guesses.
- Keep a running `NOTES.md` with every system call, setstat code, memory address and data format discovered. That file is the real deliverable of the early milestones.

## Getting the disc into a usable form

The user has `Laser_Lords__Europe_.chd` (MAME CHD v5, single track, MODE2_RAW, 290,955 frames). Its SHA1 verifies clean, so the dump is good.

```
brew install rom-tools        # provides chdman (or: brew install mame)
chdman extractcd -i Laser_Lords__Europe_.chd -o ll.cue -ob ll.bin
```

`ll.bin` is raw 2352-byte sectors. The two Python tools in the appendix already work on it: `cdi.py` parses the filesystem and extracts the program modules, `xa.py` decodes CD-i ADPCM audio.

## What we already know about the disc

### Sector format

Raw sector, 2352 bytes:

| Offset | Size | Content |
|---|---|---|
| 0 | 12 | sync |
| 12 | 4 | header (MSF + mode, mode = 2) |
| 16 | 4 | subheader: file number, channel, submode, coding |
| 20 | 4 | subheader copy |
| 24 | 2048 / 2324 | user data (Form 1 / Form 2) |

Submode bits: `0x01` end of record, `0x02` video, `0x04` audio, `0x08` data, `0x10` trigger, `0x20` Form 2, `0x40` real-time, `0x80` end of file.

Audio coding byte: bits 0-1 mono/stereo (0/1), bits 2-3 sample rate (0 = 37,800 Hz, 1 = 18,900 Hz), bits 4-5 bits per sample (0 = 4-bit, 1 = 8-bit). Same ADPCM as CD-ROM XA.

Video coding byte, low nibble: 0 CLUT4, 1 CLUT7, 2 CLUT8, 3 RL3, 4 RL7, 5 DYUV, 6/7 RGB555 lower/upper **(verify)**. Coding `0x08` shows up in `anime.rtf` and is not yet identified.

### Disc label (sector 16)

Standard CD-i disc label: `\x01CD-I \x01`, system identifier `CD-RTOS`, volume identifier `Laser Lords`, volume size 288,700 blocks, block size 2048, path table size 10 bytes at LBN 2269. One directory only (root, at LBN 2270). All structures are big-endian.

### Files

| File | LBN | Size (bytes) | Notes |
|---|---|---|---|
| COPYRIGHT / ABSTRACT / BIBLIOGRAPHY | 2271-2273 | small | text |
| cdi_launcher | 2274 | 22,528 | OS-9 program module, exec 0x56 |
| **cdi_tribes** | 2304 | 135,168 | **main game**, OS-9 program module (133,234 bytes), exec 0x54 |
| cdi_space | 2392 | 40,960 | program module, exec 0x52 (space travel?) |
| cdi_nvrui | 2422 | 48,932 | program module, exec 0x58a8 (save/NVRAM UI) |
| cdi_nvrui_res | 2446 | 33,328 | three data modules: cdi_ndat, cdi_atnc18cl, cdi_atnc24cl (fonts/resources?) |
| cdi_bumperanim | 2463 | 12,288 | program module |
| cdi_bumpdata | 2469 | 2,048 | data module |
| ntsc_bumper.rtf / pal_bumper.rtf | 2470 / 3335 | 1,771,520 each | intro bumper, video + audio |
| space.rtf | 4200 | 29,480,960 | all data sectors |
| anime.rtf | 18595 | 164,601,856 | animations, video coding 0x08 + audio |
| themes.rtf | 98967 | 45,025,280 | music: 4 stereo tracks on channels 12-15, 37.8 kHz 4-bit (already ripped and verified) |
| slides.rtf | 120952 | 52,305,920 | DYUV stills + audio |
| luxor / argos / fornax / tekton / woo / hive / ravanna `.rtf` | various | 1-4 MB | per planet: mostly data on channel 7, plus channels 1-3 and 16 |
| same planets `_v.rtf` | various | 4-82 MB | per planet: audio on many channels (voice/ambience), mono 18.9 kHz 4-bit |
| messages.rtf / help.rtf | 276234 / 283371 | 14.6 / 10.9 MB | audio channels, likely spoken messages and help |

Real-time files interleave many channels; the game selects channels via play control lists. One file holds several independent streams.

### Program modules

All program modules are standard OS-9/68k modules: sync `0x4AFC` at offset 0, module size at 0x04, name offset at 0x0C, type/language at 0x12, exec entry offset at 0x30. They are compiled C (Microware C: `UNLK A5 / RTS` epilogues everywhere). Startup code installs trap handlers (the string `Can't install trap handler` is in `cdi_tribes`), so expect `F$TLink` for the C I/O and math trap libraries **(verify which traps and which modules it asks for)**.

`cdi_tribes` contains all in-game text in plain ASCII and references the `.rtf` files by name, which makes it easy to find the asset-loading code in a disassembly.

## Architecture

```
laserlords/
  tools/            Python: extraction, asset decoders, module/syscall analysis
  runner/
    src/
      main.c        SDL3 setup, main loop
      cpu.c         Musashi glue, memory map, trap dispatch
      os9.c         OS-9 kernel subset (F$ calls)
      io.c          path/device layer (I$ calls), virtual devices
      disc.c        reads ll.bin: filesystem, sectors, real-time channels
      video.c       CD-i display planes, CLUTs, decoders, SDL texture
      audio.c       ADPCM decoder, real-time audio playback via SDL
      input.c       pointer device from mouse/trackpad
      nvram.c       save data to a file in ~/Library/Application Support
    third_party/musashi/
  NOTES.md
```

### Core idea

1. Load the modules from the disc into emulated RAM exactly where OS-9 would, set up a process context (stack, static storage, parameters) and jump to the exec entry of `cdi_launcher` (or straight to `cdi_tribes` if the launcher turns out to be trivial).
2. Hook the 68000 `TRAP #0` instruction. In OS-9, `TRAP #0` is followed by a 16-bit function code: `F$xxx` kernel calls and `I$xxx` I/O calls (I/O codes start at 0x80). Our dispatcher reads the code, implements the call natively, sets the return registers and carry flag, and resumes after the code word.
3. Other traps (C library / math trap handlers) are either loaded from the disc as real modules if present, or implemented natively once we see what they do.
4. The game talks to hardware through device paths (`I$Open` on things like `/cd`, `/video`, `/ptr`, `/nvr`, then `I$SetStt` / `I$GetStt` with CD-i specific status codes for display control, play control, pointer and so on). Each virtual device is a native implementation.

### Likely CD-i specifics (all to verify by tracing)

- **Video:** the User Communications Manager (UCM) via `I$SetStt`/`I$GetStt` on the video path. Two display planes (A and B) with CLUTs, transparency and a cursor plane, set up through drawmaps and Field/Line Control Tables. Resolution 384x240 (NTSC) or 384x280 (PAL); the disc carries both bumpers so the game probably checks the region. Render both planes plus cursor into one SDL texture, scaled with nearest neighbor or a clean upscale.
- **Real-time playback:** the CD device driver plays `.rtf` files from a Play Control List, routing selected channels to memory buffers (graphics/data) and to the audio processor (ADPCM). We implement this by reading sectors from `ll.bin` directly, filtering by channel, and feeding audio to SDL and data to emulated RAM, then signaling the game the way the driver would (signals or events).
- **Pointer:** a pointer device returning X/Y and button state; map to the Mac mouse/trackpad.
- **NVRAM:** save games go to a small file.

## Milestones

### M0: Repo and tools
Set up the repo layout, `.gitignore`, CMake skeleton that builds an empty SDL3 window on Apple Silicon, and drop in `cdi.py` and `xa.py` from the appendix. Add `tools/modinfo.py` that lists every OS-9 module on the disc with all header fields.

### M1: Static analysis
Disassemble `cdi_launcher` and `cdi_tribes` (Ghidra headless or a Python 68000 disassembler like `m68k` / `capstone`). Produce a list of every `TRAP #n` site with the function code that follows it, grouped and counted. Same for every `I$SetStt` / `I$GetStt` status code. Document the result in `NOTES.md`. This gives the full API surface before writing any runtime code.

### M2: CPU runner and syscall tracer
Musashi integrated, modules loaded, emulated RAM laid out, process context set up. Every OS-9 call is logged with its arguments and stubbed with a plausible return. Goal: the game runs as far as it can and we get a trace of the real call sequence. Report where it stops.

### M3: OS-9 kernel subset
Implement the kernel calls the trace shows: memory allocation, module linking, process/fork (launcher to game), signals, events, sleep, time. Implement the file layer on top of the disc filesystem for regular files.

### M4: Video
Image decoders first, tested offline in Python against the `.rtf` data (CLUT7, CLUT8, RL7, DYUV; identify coding 0x08). Then the display device in C: planes, CLUTs, cursor, compositing to an SDL texture. Milestone done when the title screen or first planet renders.

### M5: Real-time playback and audio
Port the ADPCM decoder from `xa.py` to C. Implement play control for `.rtf` files: channel masks, buffer delivery to the game, audio to SDL, completion signals. Done when music plays and planet graphics stream in.

### M6: Input and saving
Mouse to pointer device, keyboard shortcuts for the controller buttons, NVRAM to file.

### M7: Playthrough and polish
Play through the game, fix crashes, then fullscreen, window scaling, and a proper `.app` bundle. The app asks for the `.chd` or `.bin` on first launch and remembers the path.

## Appendix: existing tools

Both scripts expect `ll.bin` in the working directory.

### cdi.py (filesystem parser, module extractor, sector survey)

```python
import struct, sys
B=open('ll.bin','rb')
def sec(n):
    B.seek(n*2352); r=B.read(2352)
    sub=r[16:20]; form2 = bool(sub[2]&0x20)
    return r[24:24+(2324 if form2 else 2048)], sub
lbl,_=sec(16)
pt_size=struct.unpack('>I',lbl[136:140])[0]; pt_addr=struct.unpack('>I',lbl[148:152])[0]
pt=sec(pt_addr)[0][:pt_size]
dirs=[];i=0
while i<len(pt):
    nl=pt[i]; addr=struct.unpack('>I',pt[i+2:i+6])[0]; par=struct.unpack('>H',pt[i+6:i+8])[0]
    name=pt[i+8:i+8+nl]; dirs.append((name,addr,par)); i+=8+nl+(nl&1)
print('dirs',dirs)
files=[]
for dname,addr,par in dirs:
    s=addr; done=False
    while not done:
        d,_=sec(s); j=0
        while j<2048:
            L=d[j]
            if L==0: break
            size=struct.unpack('>I',d[j+14:j+18])[0]; lbn=struct.unpack('>I',d[j+6:j+10])[0]
            flags=d[j+25]; il=d[j+26:j+28]; nl=d[j+32]; name=d[j+33:j+33+nl]
            su=d[j+33+nl+((nl+1)&1):j+L]
            attr=struct.unpack('>H',su[4:6])[0] if len(su)>=6 else 0
            fnum=su[8] if len(su)>8 else 0
            if name not in (b'\x00',b'\x01'):
                files.append((dname,name.decode('latin1'),lbn,size,flags,il.hex(),attr,fnum))
            j+=L
        # dir size from first record
        done=True
for f in files: print(f)

import collections, os
os.makedirs('ext',exist_ok=True)
def read_file(lbn,size):
    out=b''; n=(size+2047)//2048
    for k in range(n): out+=sec(lbn+k)[0][:2048]
    return out[:size]
for d,name,lbn,size,fl,il,attr,fnum in files:
    if not name.endswith('.rtf'):
        open('ext/'+name,'wb').write(read_file(lbn,size))
# survey rtf sector types
for d,name,lbn,size,fl,il,attr,fnum in files:
    if name.endswith('.rtf'):
        n=size//2048 if size%2048==0 else (size+2047)//2048
        c=collections.Counter()
        for k in range(min(n,4000)):
            B.seek((lbn+k)*2352+16); sh=B.read(4)
            sm=sh[2]; kind='A' if sm&4 else 'V' if sm&2 else 'D' if sm&8 else '?'
            c[(kind,sh[1],sh[3])]+=1
        print(name, n, dict(c.most_common(6)))
```

### xa.py (CD-i ADPCM decoder, usage: python3 xa.py <lbn> <sectors> <prefix>, e.g. python3 xa.py 98967 21985 themes)

```python
import struct, wave, sys, collections, numpy as np
B=open('ll.bin','rb')
K0=[0,60,115,98]; K1=[0,0,-52,-55]
def decode(lbn, nsec, chan_filter=None):
    chans=collections.defaultdict(list); fmt={}
    for k in range(nsec):
        B.seek((lbn+k)*2352); r=B.read(2352)
        fn,ch,sm,ci=r[16:20]
        if not sm&4: continue
        chans[ch].append(r[24:24+2304]); fmt[ch]=ci
    return chans, fmt
def xa_to_pcm(sectors, ci):
    stereo=ci&1; bits8=(ci>>4)&1; rate=18900 if (ci>>2)&1 else 37800
    nunits = 4 if bits8 else 8
    out=[]; st=[[0,0],[0,0]]
    for s in sectors:
        for g in range(18):
            grp=s[g*128:(g+1)*128]; hdr=grp[:16]; data=grp[16:]
            for u in range(nunits):
                p=hdr[4+u]; sh=p&15; f=(p>>4)&3; c=(u&1) if stereo else 0
                p1,p2=st[c]; buf=[]
                for i in range(28):
                    if bits8: n=data[i*4+u]; n = n-256 if n>127 else n; v=(n<<8)>>sh
                    else:
                        b=data[i*4+(u>>1)]; n=(b>>4) if u&1 else (b&15); n = n-16 if n>7 else n; v=(n<<12)>>sh
                    v += (p1*K0[f]+p2*K1[f]+32)>>6
                    v=max(-32768,min(32767,v)); p2,p1=p1,v; buf.append(v)
                st[c]=[p1,p2]; out.append((c,buf))
    if stereo:
        L=[];R=[]
        for c,b in out: (R if c else L).extend(b)
        n=min(len(L),len(R)); a=np.empty(n*2,np.int16); a[0::2]=L[:n]; a[1::2]=R[:n]
    else:
        a=np.array([x for _,b in out for x in b],np.int16)
    return a, rate, 2 if stereo else 1
lbn,nsec,prefix=int(sys.argv[1]),int(sys.argv[2]),sys.argv[3]
chans,fmt=decode(lbn,nsec)
import os; os.makedirs('audio',exist_ok=True)
for ch in sorted(chans):
    a,rate,nc=xa_to_pcm(chans[ch],fmt[ch])
    w=wave.open(f'audio/{prefix}_ch{ch:02d}.wav','wb'); w.setnchannels(nc); w.setsampwidth(2); w.setframerate(rate); w.writeframes(a.tobytes()); w.close()
    print(prefix, ch, f'{len(a)/nc/rate:.1f}s', rate, 'stereo' if nc==2 else 'mono', 'peak',int(np.abs(a.astype(np.int32)).max()))
```
