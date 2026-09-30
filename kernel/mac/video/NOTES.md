# Frame-buffer console

Text console on the mode the firmware left (A/UX Startup: its lowest-depth mode, 1 bpp). DAFB
registers are only read (sense lines).

## What the screen shows, and from when

| Boot point | Screen |
|---|---|
| `aux_entry`, after `mac_scc_init` | `fbcons_auxinit` finds the mode and clears the screen. Every message after that, including `A/UX Startup hand-off`, the aux checks and their halts |
| `mac_shim_main` (direct boot) | `fbcons_biinit` from the boot record, before the shim banner |
| `config`, `pstart`, kernel `printf`, `panic`, `mac_stop`, early exceptions | all through `putchar` (SCC first, then screen) |
| console tty (`/dev/console`, SCC channel A stream) | output drawn in the write service (`scc_conout`) as it arrives, SCC gets a best-effort copy; input from `fbcons_input` |

The frame buffer is addressed VA = PA. With the MMU off that is physical; with it on,
`pstart`'s DTT1 (0x807FA060) covers 0x80000000–0xFFFFFFFF, supervisor only, so the existing
mapping serves. `mac_stop` turns the MMU off and still prints.

## Cache mode: why DTT1's cache-inhibited is kept

- DTT1 CM = 11: cache-inhibited, not serialized. Stores go through the 040 store buffer, so
  glyph writes do not stall on VRAM.
- Write-through would help only scroll reads, and cannot be had: TTs match in 16 MB units,
  and 0xF9xxxxxx also holds the DAFB registers (0xF9800000), which must not be cached.
- Scroll is a VRAM-to-VRAM copy of the visible bytes: at 640×480, 1 bpp (what A/UX Startup
  leaves) 37 KB per line feed, a few ms; at 8 bpp (direct boot) 300 KB. A line feed on the
  bottom line scrolls once for every line feed that follows in the same block before the
  next ESC (jump scroll; the screen ends identical, checked by the host test).
- The console tty draws from its write service procedure, which AMIX runs at IPL 1: VIA1
  (clock, ADB) waits while a block is drawn.

## Mode discovery (`fbprobe.c`)

**A/UX Startup switches every display to its lowest-depth mode (1 bpp) just before the
jump and does not update `ScrnBase`/`ScreenRow`** (derived from the shipped A/UX Startup 3.1,
CODE 4:49e4 → 4:4c02 → 4:4d80). A/UX's own kernel likewise takes the lowest mode (`video_find` →
`getVPBlock`, 0x10047cae, through the Slot Manager). The probe reads the declaration data
directly instead of calling the ROM.

1. **Direct boot**: `BI_MAC_VADDR/VROW/VDEPTH/VDIM` (VDIM = height << 16 | width, as Linux
   uses it; verify with the booter used). No depth switch happens.
2. **A/UX Startup**, from the low-memory copy at info − 0x3C00 (offsets from A/UX
   `<mac/sysequ.h>`):
   1. **Declaration ROM.** `ROMBase` ($2AE); ROM size from the ROM header (+0x40); format
      block in the last 20 bytes (test pattern 0x5A932BC7, byte lanes 0x0F); sResource
      directory; every sResource of category 3 / type 1 (video); its mode lists (ids ≥ 0x80)
      → `mVidParams` → VPBlock. A mode *matches* when its row bytes = `ScreenRow` and
      `ScrnBase − vpBaseOffset` lies in 0xF9000000–0xF97FFFFF. Score: 1, +2 if its pixel
      size is the Monitors depth (main PixMap `pixelSize` when that part of the heap
      survived, else `ChunkyDepth` when `CrsrBase`/`CrsrRow` show the cursor screen is the
      main one), +1 if its bounds are the sense code's size. The best sResource wins (first
      of equals); the console uses its **lowest-depth mode**: base = (`ScrnBase` − matched
      mode's offset) + lowest mode's offset, its row bytes and bounds. Every pointer is
      checked against the ROM's own extent; lists stop at 255 entries.
   2. **1 bpp assumption**: base `ScrnBase`, row `ScreenRow`, depth 1 (DAFB keeps the row
      bytes over 1–8 bpp in most configurations of the Q700/900 ROM). Size from the sense
      code, else row × 8 (≤ 1152) by `ScreenBytes` / row (else 480).
   Sense code: `~(*(long *)0xF980001C) & 7`, as A/UX's `DAFBReadSenseLines` (0x5478a)
   decodes it, **read only**; only for machine 33 and a base in 0xF9000000–0xF97FFFFF. Sizes
   per code (`<sys/obvideo.h>`): 0, 3 → 1152×870; 1, 5 → 640×870; 2 → 512×384; 6 →
   640×480; 4, 7 unknown. The DAFB base/stride registers are not read: their read
   behaviour is not known.
3. Accepted bases: 0xF9000000–0xF97FFFFF (built-in; the region must end below 0xF9800000)
   or NuBus slot space 0xFA000000–0xFEFFFFFF. Anything else leaves the console off.

Q700/900 ROM (420DBFF3), DAFB sResources 0xC0–0xEF: 640×480 at row 1024 (1–8 bpp) or 832
(base offset 0x9C08 at 1 bpp, 0x9C40 at 8 bpp); 640×870 at row 512 (1–4 bpp) and 1024 (8
bpp); 1152×870 at 576/1152. The host test runs the probe on this image. The parser is generic;
its tables for the Q800 ROM (F1ACAD13) are unchecked.

`config` prints the mode, the method, and every raw value and candidate:

```
config: fbcons 0xf9000000 row 0x00000400 depth 0x00000001 0x00000280x0x000001e0 (declaration ROM, lowest mode)
config: lowmem 0x00000000 ScrnBase … ScreenRow … ChunkyDepth … pixelSize …
config: lowmem CrsrPin … ScreenBytes … sense … Monitors depth …
config: ROM 0x40800000 size 0x00100000 sRsrc … score … others … -> base … row …
```

## Pixels

| Depth | Black (text) | White |
|---|---|---|
| 1, 2, 4, 8 (indexed) | all ones | 0 |
| 16 (x-5-5-5) | 0 | 0x7FFF |
| 32 (x-8-8-8) | 0 | 0x00FFFFFF |

Indexed: standard Mac CLUTs (colour or grey) have white at 0 and black at the last index; the
CLUT is not touched. Cells are 8×16; a cell row is `depth` bytes.

## Terminal

Two parsers share screen and cursor: kernel (`\n` = CR LF, never answers) and tty (`vt100`
terminfo). Supported: CR LF VT FF BS TAB SO SI CAN SUB; ESC 7 8 D E M c ( ) #; CSI A B C D E F
G ` d H f J K L M @ P X m (0 1 7 22 27) r s u n (5, 6) c, `?25h/l` (cursor), `?7h/l`
(autowrap); DEC graphics via `ESC ( 0`; pending wrap (xenl). Cursor: inverted cell. A
caller interrupting a drawing caller queues its bytes (512) for the owner. Kernel output
during a panic takes the renderer; `mac_stop` draws what was queued before its message.

## Keyboard interface

`void fbcons_input(int c)`: any IPL, one byte (ASCII or escape sequence). It goes into SCC
channel A's receive ring (`scc_conin`) as if received, so `ldterm` on `/dev/console` sees
it. The ADB driver (`adb/adbkbd.c`) calls it through `adb_ttyin`. Dropped while the
console is closed.

## Hardware checks (open)

1. The screen clears to white at the hand-off and the banner is the first line. If
   nothing: `config: fbcons off: …` on the SCC says why. Text 8× too wide or garbled means
   the depth is wrong (the 1-bpp switch did not happen).
2. `config: lowmem`/`config: ROM` values: ScrnBase is physical 0xF9xxxxxx, the method is
   "declaration ROM", score ≥ 2, and "others" is 0.
3. Sense code for the attached monitor.
4. No DAFB side effects of the sense read (screen stays stable).
5. Scroll time at the actual depth. Shell output is not bounded by the SCC: it is drawn in
   the write service; the SCC gets what fits in its 256-byte ring (`scc_nmirdrop` counts
   the rest).
6. 16/32 bpp colours (x-5-5-5, x-8-8-8) and the indexed CLUT polarity.

## Font

`fbfont.c` is generated by `mkfont.py` from X11's Sony 8x16 (`8x16.pcf.gz`, xfonts-base,
ISO 8859-1 with DEC line-drawing glyphs at 0x01–0x1F); `build.sh` rechecks it when the
source is installed. Licence (Sony, HPND style):

> Copyright 1989 by Sony Corp. Permission to use, copy, modify, and distribute this software
> and its documentation for any purpose and without fee is hereby granted, provided that the
> above copyright notices appear in all copies and that both those copyright notices and this
> permission notice appear in supporting documentation, and that the name of Sony Corp. not
> be used in advertising or publicity pertaining to distribution of the software without
> specific, written prior permission. Sony Corp. makes no representations about the
> suitability of this software for any purpose. It is provided "as is" without express or
> implied warranty.

The public-domain misc-fixed `8x13.pcf.gz` also works (`mkfont.py …/8x13.pcf.gz`).

## Tests

Run `sh kernel/mac/video/verify.sh` (image checks, then the host test).

- `test/run.sh` (host): jump scroll (a 1.6 KB block with wraps, VT/FF and a scroll region
  gives the same pixels as the same bytes one at a time); renders a page (all printable and
  Latin-1 glyphs, inverse, bold, BS, TAB, EL, DEC graphics, ICH/DCH, IL/DL, scroll region,
  RI, autowrap, DSR, kernel LF) at 1 (two geometries), 2, 4, 8 (padded row, 1152×870), 16
  and 32 bpp (padded, 79 columns); decodes every pixel and compares each cell with the font
  bitmap from a text-grid model; checks row padding and guard bytes are untouched. Probe
  cases on fake low memory with the Q700/900 ROM image (PixMap 8 bpp; ChunkyDepth 8 with a
  moved base offset; portrait 640×870 whose row changes with depth; already 1 bpp; no
  matching mode; ROMBase in RAM), 1-bpp fallbacks without a ROM, rejected base, no info
  block. Writes PGMs.
- `verify.sh` (linked image): no BSS, font bytes, hook order, console output path
  (`sccwsrv` → `scc_conout` → `fbcons_write`), `mac_stop` → `fbcons_unlock`, DTT1, DAFB
  read-only, then the host test.

## Sources

A/UX 3.x `/unix` (`video_find`, `getVPBlock`, `DAFBReadSenseLines`, `_DAFBMonitorDetect`) and
A/UX Startup 3.1 (depth switch), derived from the shipped binaries; the declaration-ROM format
(Apple, *Designing Cards and Drivers*) checked on the Q700/900 ROM image; A/UX headers
`<sys/obvideo.h>`, `<sys/video.h>`, `<mac/sysequ.h>`; the Q700/900 ROM's VPBlock tables; the
AMIX link kit (console driver shape); this repo's SCC driver.
