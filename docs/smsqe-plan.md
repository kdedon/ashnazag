# SMSQ/E environment plan
Checked against the source (github.com/CodeDreamer/SMSQE, v3.43).
## Source and licence
- `licence_txt`: BSD 2-clause (Tebby, Kilgus, Lenerz et al., 1989-2016).
  Ship the notice and disclaimer with binaries.
- ~2000 QL-syntax 68k assembly files (CRLF, `xdef/xref/section`).

## Toolchain
- QMAC and QLINK (QL-native, `extras/exe`), driven by SuperBASIC
  `make_bas`; they run only on QDOS/SMSQ.
- Our cross tools cannot assemble it as is. Options: write a QMAC-syntax
  assembler and linker (small, C900 style), or bootstrap by running QMAC
  under SMSQ/E in our own environment. Prebuilt Atari images are the
  first-target fallback.

## Atari build
- Boot: one file, TOS program (`sys/boot/st/host`) with a boot-sector
  variant. The program does Supexec, sets SR $2700, SP $400, then jumps
  to the loader appended in the same file. Loader, hwinit, drivers and the
  OS follow as modules. It never returns to TOS, never uses BIOS/GEMDOS
  after start, and does its own memory sizing from the hardware.
- Hardware (keys/atari): MFP 68901 (200 Hz timer C, serial, GPIP/disk
  IRQ), keyboard and MIDI ACIAs at $FFFFFC00/04 (IKBD packets and mouse),
  YM2149 at $FFFF8800 (beep, parallel), Shifter at $FFFF8200-8260 (palette
  $8240, ST mono/medium/low), TT video, DMA/WD1772 and ACSI at $FFFF8600,
  SCSI (TT), SCC, RTC, blitter, STE display, QVME/Edwin cards, PMMU/FPU
  and cache control.
- Falcon is detected (type $10) but has no Videl driver: ST video modes only.

## Driver model
- Modules loaded in order by the loader; each has a header with a select
  routine. Atari set: hwinit, nasty (init), `driver_ql` (screen),
  `driver_mo` (mono), `driver_ser`, `driver_dv3` (disks), `kbd_lang`,
  `sysspr`.
- Screen: console driver `CON`/`SCR` over `iod/con2`, with a
  per-machine plug (`pt_setup`, mode `ptm.*`, depth `ptd.*`): Atari
  `at_ql`, Q68 `q68_8`/`q68_16`, QPC `8`/`16`.
- Keyboard: `kbd_poll` reads a queue of codes; machine-specific
  `kbd_init`/`kbd_int` fill it (IKBD on Atari, AT on Q68).
- Mouse: Q68 `driver_mouse`; Atari via IKBD.
- Drives: DV3 (`dv3/*`) with `WIN`, `FLP`, QLWA, FAT, `RAM`, `MEM`;
  Atari `dv3/atari/{acsi,scsi,fd,hd}`; Q68 SD card; QPC host files.
- Ticker: `hdop_poll` and a frame/timer interrupt server (Atari MFP
  timer C at 200 Hz, `at_poll`; Q68 50 Hz frame `q68_int2`).
- Also `SER`, `PAR`, `NUL`, `PIPE`, sound `ssss`.

## Best base for the "aux" machine
Q68. It has a plain linear 8 or 16-bit frame buffer with no bitplanes,
RAM-only memory map, a 50 Hz frame interrupt, an AT keyboard and mouse
queue, and small separate drivers. Atari is planar and register-heavy;
QPC needs a host call layer we would have to replace anyway. Take the
Q68 8/16 console, `kbd_*` and `mouse` modules, replace register access
with display-service events and a block-device service; keep the rest.

## 1. Falcon passthrough (first target)
- `starttos -P` runs the PRG from `C:` (or a boot-sector disk). It owns the machine: needs exclusive hardware,
  root grant, no Unix use of its disk, and a way back (reset or hot key,
  restoring MFP, video, vectors).
- ST-compatible modes only; keyboard and mouse via IKBD passthrough.
- Test: QL screen, keys and mouse, `DIR`, SuperBASIC, return to Unix.

## 2. Other hosts (aux driver set)
- Screen: Q68-style linear surface over the display service (512x256
  mode 4/8, larger true-colour mode).
- Keyboard/mouse: events into the Q68 queue and pointer interface.
- Drives: DV3 block driver over a Unix image; folder driver over hostfs.
- Ticker: frame interrupt from the guest framework.

## 3. Stages and tests
1. Toolchain: reproducible binary.
2. Falcon run; 3. register census in Hatari.
4. aux driver set on the Quadra in QEMU: desktop, text, window, pointer;
   4b. image and folder drives.
5. `startsmsq -e ENV` switching, second session, memory limit.
Tests: GROUP=smsq in tests/run-qemu.sh and a Hatari case.

## Media
- SMSQ/E source and binaries: BSD, redistributable with the notice.
- QL ROMs/Minerva and QL software: user-supplied.
