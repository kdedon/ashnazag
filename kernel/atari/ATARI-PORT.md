# Atari port: Falcon030 and CT60/CT63

Host port of our kernel (AMIX 2.1 objects + Atari platform layer) to:

- **Falcon030 base model**: 68030 at 16 MHz, no FPU, up to 14 MB ST-RAM, no FastRAM (the user's FPGA core).
- **CT60/CT63 Falcon**: 68060 (with FPU; EC/LC possible), SDRAM FastRAM from 0x01000000 (a friend's real machine).

Unix boots first: the ROM runs our root-sector code, which loads the kernel; TOS only initialises the hardware and is gone after hand-off. Console = Videl screen + IKBD keyboard (no serial at the user's end).

## 1. Kernel base per CPU

| Target | Base image | MMU/page | Notes |
|---|---|---|---|
| Falcon030 | **stock AMIX 2.1p2a 030 kernel** (`kit-2.1c` relink, `amix-2.1p2a/stand/unix`) | 030 PMMU, 2 KB pages, stock `pstart` | the 040/060 port is not needed; stock `chk_fpu` probes and gates every FPU use on `fpu_present` |
| CT60/CT63 | the port's `unix-040` (boots 040 and 060) | 4 KB pages, `pstart040.s` | 060 machine layer exactly as on the Amiga 060 (§6) |

Platform layer overlay as on the Mac (`relink-mac.sh` method): weaken the Amiga platform symbols by name and link Atari objects over them (`config putchar getchar callrom sysdump haltsys rtnfirm hw_clkstart clkreld p1int`–`p6int coinfo scropen parinit qlintr slpoll autocon delayus`, `M68Kvec` slots). One source tree builds both images (`unix-atari030.elf`, `unix-atari060.elf`).

Open: the Mac overlays already in `build.sh` (I_NREAD, s5 pages, kvmpages, DLM, guest, A/UX personality) were made against `unix-040`. Each must be checked for 040-only addresses or code before it is applied to the 030 image; anything address-based needs a by-symbol or by-signature form.

## 2. Hardware map against the platform interface

Sources: **NB** NetBSD/atari (`ref/netbsd/sys/arch/atari`, BSD, has Falcon support for all items below), **LX** Linux/m68k atari (`ref/linux`, GPL), **FM** FreeMiNT (`ref/freemint`, GPL), **SP1** atari-sysv-sp1 (`ref/atari-sysv-sp1`, GPL), **CT** ct60tos (`ref/ct60tos`, facts only), **MAC** our Mac layer (`kernel/mac`), **PORT** AMIX 040/060 port. Our modules stay MIT/BSD: GPL sources are read for facts, BSD code can be adapted.

| Item | Falcon facts | Interface slot | Source | New work |
|---|---|---|---|---|
| CPU entry, boot record | ROM hands control to root-sector code; loader passes a Linux/m68k bootinfo (MACH_ATARI, `BI_ATARI_MCH_COOKIE`, two `BI_MEMCHUNK`s) | `config(d0,d1)`, `atari_entry` | MAC `macentry.s` bootinfo parser; LX/Hatari `--lilo` record layout | Atari tags, entry shim |
| ST-RAM | 0–14 MB, 24-bit; the only RAM the DMA chips (disk DMA, sound, Videl) can reach | `chipmem`, DMA pool | NB `stalloc.c` (ST-RAM pool), SP1 `snd.c` (ST ring at boot) | `stram_alloc()` for drivers |
| FastRAM (TT-RAM) | none on a stock Falcon; CT60 SDRAM at 0x01000000 (up to 512 MB); Hatari `--ttram` | `MAINSTORE`/`VSIZOFMEM` | CT `sdram.S` (base, `_FRB` cookie) | region choice (§3) |
| MMU map | I/O at 0xFFFF8000–0xFFFFFFFF (also 0x00FF8000 alias), IDE 0xFFF00000, ROM 0x00E00000 | `pstart` slots 0 and 3 | stock `pstart` fits as is: 0xFFFFxxxx is in the cache-inhibited 3–4 GB slot | 060: DTT1 covers it; CT: 0x00FFxxxx must not be used on 060 |
| Interrupts | IPL 2 HBL, 4 VBL (autovector), 5 SCC (vectored), 6 MFP (vectored, vector base programmable), 7 NMI | `M68Kvec`, `p1int`–`p6int` | NB `vectors.s`/`intr.c`, LX `ataints.c` | Atari `ataintr.s` (§4) |
| MFP 68901 | 0xFFFFFA01; timers A–D at 2.4576 MHz; GPIP4 = IKBD/MIDI ACIA, GPIP5 = disk (DMA/SCSI/IDE/FDC) | `hw_clkstart`, `clkreld`, tick | NB `clock.c` | 60 Hz tick (§4) |
| Videl | regs 0xFFFF8200–82C3, palette 0xFFFF9800 (256 × long), monitor type in 0xFFFF8006 bits 7–6 (mono/RGB/VGA/TV); 1/2/4/8 interleaved planes, 16-bit chunky | console, `/dev/fb` | NB `grfabs_fal.c` (mode tables), MAC `video/fbcons.c` (1-bit text), MAC `display/` service | Videl mode set, planar blit (§7) |
| IKBD | 6850 ACIA 0xFFFFFC00 (MIDI at FC04); scancodes, relative mouse packets, joystick; via MFP GPIP4 at IPL 6 | console input, event devices | NB `kbd.c`, `kbdmap.c`, `ms.c`; FM `arch/acia.S`, `keyboard.c` (facts) | `ikbd.c` feeding the display service like `adbkbd.c` |
| IDE | 0xFFF00000, PIO only, 16-bit, interrupt shared on GPIP5 | `sd.h` layer under AMIX `dd` | NB `wdc_mb.c`, LX `pata_falcon.c` | SCSI-command to ATA translation under `dd` (§5) |
| SCSI | NCR 5380 behind the ST DMA chip (0xFFFF8604/8606, "Falcon SCSI"); DMA to ST-RAM only; shares the DMA chip with the floppy | `sd.h` layer | NB `atari5380.c`, `ncr5380.c`, `dma.c` (DMA-chip arbitration); MAC `scsi/` structure | 5380 driver, bounce buffers |
| ACSI | none on the Falcon (TT/ST only) | — | — | — |
| Floppy | WD1772 via the same DMA chip; select via YM port A | later | NB `fd.c` | after M2 |
| SCC 85C30 | 0xFFFF8C81 (A = LAN, B = Modem 2), PCLK 8 MHz, IPL 5 | serial tty | MAC `scc/scc.c` (same chip; only addresses, clock and register spacing change) | addresses, baud table |
| DMA sound / codec | 0xFFFF8900–893F (DMA, crossbar, codec), samples in ST-RAM | `/dev/audio` | SP1 `snd.c` (TT DMA sound, superset on Falcon), FM `xdd/audio` (crossbar facts) | Falcon modes, crossbar |
| YM2149 | 0xFFFF8800; port A: floppy select, speaker, strobe | bell, floppy | NB `ym2149.c` | console bell |
| DSP 56001 | host port 0xFFFFA200–A207 | passthrough only | FM `xdd/dsp56k` (facts) | M4 |
| RTC/NVRAM | MC146818 at 0xFFFF8961 (index) / 8963 (data); NVRAM holds boot preference, video mode, language | `clkset`/`stime`, root `date` | NB `clock.c`, `nvram.c`; MAC `rtc/` hook points; SP1 `setboot` (boot-pref byte 0x40 = Unix) | rtc driver |
| Bus control | 0xFFFF8007 (16/8 MHz bus, blitter) | `config` | LX | leave as TOS set it |
| Reboot/halt | MMU and caches off, jump through ROM reset vector at 0x00E00004 | `haltsys`, `rtnfirm` | MAC `rtc/` restart flow, NB `machdep.c` | per CPU (CT: cpusha first) |
| Network | none built in; NetUSBee (cartridge port), DaynaPORT SCSI/Link (ZuluSCSI/BlueSCSI), CT60 EtherNat, SuperVidel | DLPI | SP1 `dp.c`, FM `sockets/xif/ethernat` (facts) | NetUSBee: `netusbee/` (major 18, `/dev/aen0`); DaynaPORT: `dayna/` (major 58, `/dev/dpn0`); others after M2 |

## 3. Memory

The core manages one contiguous region (`MAINSTORE`/`VSIZOFMEM`). Policy in `config`:

- **FastRAM present** (CT60, Hatari `--ttram`): VM region = FastRAM; ST-RAM becomes `chipmem`, a device pool for DMA buffers, the screen and (M4) the TOS guest.
- **Falcon030 without FastRAM**: VM region = ST-RAM above the kernel minus a fixed pool at the top (screen + DMA buffers, size from a boot argument, default 512 KB).
- Both RAM kinds in the VM come with the planned multi-region work (`plat_memsegs[]`, PLAN "Large and split memory"); not needed for M1–M3.

DMA: disk and sound DMA reach only ST-RAM (24-bit). The SCSI path bounces through a ST-RAM buffer when the user page is in FastRAM; IDE is PIO and needs none. On the 060 the caches do not snoop Falcon DMA: flush/invalidate around each transfer, reusing the port's `dma_cache040.s` service.

Caches on the 030: stock `sup_cacr` already runs the supervisor without data cache; keep it. On the 060: copyback FastRAM, write-through ST-RAM, cache-inhibited I/O (CT60 TOS uses the same split).

Kernel placement: linked at 0x1000 with the entry first (the Linux/m68k convention Hatari's `--lilo` expects) for M1. The disk loader relocates the same objects (AMIX kernels are built relocatable) so that M4 can place the kernel in FastRAM, or high in ST-RAM, leaving ST-RAM page 0 for a passthrough guest.

## 4. Interrupts and clock

The MFP (IPL 6) and SCC (IPL 5) sit above AMIX's `splhi` (IPL 4, inlined at ~340 sites), so the core never masks them.

**Recommendation: raise the inlined `spl` level.** Patch every `move.w #$2400,sr` in the generic objects to `#$2600` (same length; found by signature, checked by count, like the port's byte patchers). Then `splhi` masks MFP and SCC, and their handlers may call `clock_int`, STREAMS and wakeups directly as the Amiga level-2 handler does. Cost: MFP and SCC latency during `splhi` sections; the SCC receiver has a 3-byte FIFO, enough at 38400 bps.

Rejected alternatives: VBL (IPL 4) as tick — rate depends on the monitor (50/60/71 Hz), not 60; IPL-6 stub deferring to a lower level — 68k has no software interrupt, and `spl` lowering is inlined so pending work cannot be run on `splx`.

Tick: MFP timer A, prescaler 64, data 160 → 240 Hz; every 4th interrupt calls `clock_int` (exact 60 Hz = `HZ`); the 240 Hz count also serves `delayus` calibration and IKBD timeouts.

Vectors: MFP vector base 0x40 (vectors 64–79), software end-of-interrupt mode; SCC vectored at its own base; HBL (2) masked; VBL (4) only counts frames for the display service.

## 5. Disks and layout

AMIX's own `dd` disk driver stays (as on the Mac); only the `sd.h` hardware layer is new:

- **IDE first** (Falcon internal, Hatari `--ide-master`, CF adapters, likely the FPGA core): translate the SCSI commands `dd` sends (TEST UNIT READY, INQUIRY, READ CAPACITY, READ/WRITE 6/10, MODE SENSE) to ATA PIO with LBA28. Polled first, then GPIP5 interrupt.
- **SCSI second** (external SCSI-2 port, ZuluSCSI/BlueSCSI): NCR 5380 + DMA chip, polled PIO first, DMA with bounce later.

Disk layout: AHDI root sector (what the ROM boots and TOS understands), sector 0:

| Partition (AHDI id) | Content | Slice |
|---|---|---|
| `BGM`/`GEM` (optional) | FAT for the TOS side and data exchange | s4–s7 by order |
| `AXB` | boot area: second-stage loader + contiguous kernel image | — |
| `AXR` | root (s5 first, ufs later) | s1 |
| `AXS` | swap and dump | s2 |
| `AXU` | /home | s3 |

s0 = whole disk, as on the Mac (`apm.c` → `ahdi.c`, same slice policy and `root=cNd0sM` boot argument). Ids `AXB/AXR/AXS/AXU` are ours; ASV's `UNX` (VTOC inside) and NetBSD's `NB?` ids are left alone. XGM extended partitions: read, not created. The boot loader honours the NVRAM boot preference (none or 0x40 SysV: Unix; anything else, 0x80 TOS included: TOS).

Image builder: `mkdisk.sh` writes a raw 512 MB image (`DISKMB`): root sector with boot code, AXB 4 MB, ufs root 128 MB, swap 64 MB, `/home` the rest (`GEM=1`: a TOS FAT partition instead). Hatari writes to images: always boot a copy.

## 6. CPU plans

**No FPU (Falcon030, also LC/EC060).** The kernel never needs the FPU. AMIX userland does use the FPU (cc emits 68881 code; `fsck` and libm fail without one), so soft-float alone cannot cover existing binaries.

**Recommendation: in-kernel FPU emulation via the F-line trap** — NetBSD's m68k FPE (BSD), already linked into the port by `relink-040-fpe.sh` with glue to AMIX's `u_trap` frame. For the Falcon it moves onto the 030 image: vector 11 → `fpe_vec11`, 030 format-0 F-line frames, `fpu_present` = 0 keeps the context code off, the emulated register file lives in the u-area save area. New ELF userland may additionally be built soft-float for speed; both coexist. Hatari runs `--fpu none` to prove it.

**68060 (CT60/CT63).** Use the port's 060 machine layer unchanged: `cputype060.s`, `fpu060.s`/`fpuinit060.s`, Motorola 060SP — ISP on vector 61 (`isp61_060.s`; movep, 64-bit mul/div, cas2, chk2/cmp2) and FPSP (`fpsp060_glue.s`) — plus PCR superscalar enable (revision-1 parts: CT `sdram.S` fact). Atari-specific on top:

- caches/TT registers: ITT0/DTT0 supervisor-only over RAM (Mac N1 values 0x003FA000/0x003FA060 cover 0–1 GB), DTT1 cache-inhibited over 0x80000000–0xFFFFFFFF (I/O at 0xFFFFxxxx);
- no `movep` in our drivers (060 traps it; ISP emulates it slowly); use byte moves;
- DMA flush around disk/sound transfers (§3);
- EC060 (no MMU) is unsupported; LC060 runs the FPE as on the Amiga LC060.

Hatari `--cpulevel 6` covers early checks; the friend's machine is the acceptance test.

## 7. Console

The user has screen + keyboard only, so the console is built in from M1:

- **Output**: Videl framebuffer, 1 bit/pixel text through the Mac `fbcons.c` renderer (8×16 font). M1 moves the screen into the ST-RAM pool and programs a 2-colour mode for the monitor in 0xFFFF8006, with or without TOS: VGA 640×480, RGB/TV 640×400 interlaced at the refresh TOS left (50/60 Hz), SM124 640×400. `video=MON640xLINES[@HZ]` (MON mono, rgb, vga or tv; e.g. `video=rgb640x200@50`) forces one. A plane-0-only view of any planar mode gives 1-bit output for free.
- **Input**: IKBD ACIA at IPL 6 (after the `spl` patch: maskable) → scancode → the shared keymap/console path of the display service; mouse packets → event device (X later).
- `putchar` (panic path) draws straight into the framebuffer, polled; `getchar` polls the ACIA.
- Session modes: on a VGA monitor a `/dev/fb` session may set 640×480 at 1 bit or 8 interleaved planes, or 320×480/320×240 at 16-bit chunky (`FBIOSMODE`); the mode goes with the session on a switch. X draws 8 planes directly (`x11/falcon/XFALCON.md`).

## 8. Boot path

1. ROM (TOS 4.0x on the Falcon, CT60 TOS on a CT60) reads the root sector of the boot device; if its words sum to 0x1234 it runs it, position-independent, from a buffer.
2. Root-sector code (`bootsec.s`, ≤ 342 bytes, entered with d3 = 'DMAr', d4 = device): read the AXB loader with XBIOS `DMAread` and call it; if it returns, boot the first 0x81 partition like the AHDI root code.

**EmuTOS 1.4 does not run hard-disk boot code**: its `hdv_boot` path runs only when `_bootdev` ≤ 1 (floppy), so IDE, SCSI and ACSI root sectors and partition boot sectors are ignored (probed, §10). Machines on EmuTOS (Hatari by default, possibly the FPGA core) therefore boot by `--lilo` (Hatari), or by a small EmuTOS change adding root-sector boot, offered upstream; to decide in M2 together with which ROM the FPGA core runs.
3. Loader (`axbload.c`, AXB sectors 0–14; command line in 15, kernel ELF from 16): TOS/Unix prompt with timeout (NVRAM preference, Alternate = TOS); read the ELF; boot record as `--lilo` builds it (`phystop`, `_ramtop`/`_ramvalid`, `_CPU`, `_FPU`, `_MCH`, command line with `root=c?` filled in); IPL 7; caches off (040/060: `cpusha` first), MMU and TT registers off; copy the segments; boot record at `end`; jump to 0x1000. The ELF headers are bounds-checked and the loaded part's word sum compared before anything is copied. `instboot.py` installs it on any AHDI disk whose partitions lie within the disk and apart, and keeps the old root sector for `--restore`.
4. `atari_entry` → `config` → stock `stext`/`pstart`.

Development shortcut: Hatari `--lilo` loads the same ELF straight from the host and builds a Linux/m68k bootinfo, with no TOS at all (needs MMU on and ≥ 8 MB ST-RAM). The QEMU `-kernel` path on the Mac uses the same record, so `config`'s parser is shared.

Probe (`probe/mkprobe.sh`): the same code in the root sector and in partition 0's boot sector turns the background red if the ROM runs it.

## 9. Milestones

| M | Goal | Done when | Runs on |
|---|---|---|---|
| M1 | kernel to console on Falcon030 | `--lilo` boot, `--fpu none`, 14 MB ST: banner, memory sizes and single-user `#` prompt on the Videl screen, typed commands echo via IKBD; RAM-disk root linked in as on the Mac; 60 Hz tick within 0.1 % of host time; FPE: `fsck -n` on the RAM root works | Hatari |
| M2 | disks and shell | IDE root (s5) from an AHDI image booted through our root sector and loader; multi-user login on the console; swap; RTC date; reboot/halt; SCSI disk second; then the test suite (`tests/`) in Hatari | Hatari, then the FPGA core (user) |
| M3 | 68060 | `unix-atari060` on Hatari `--cpulevel 6 --ttram`; FastRAM as VM; ISP/FPSP exercised; DMA flushing; then the friend's CT60/CT63 | Hatari, real CT60 |
| M4 | TOS container passthrough | EmuTOS container on a Falcon host with Videl, DMA sound, IKBD, YM, DSP forwarded (shared grade), disks and MFP timer A kept by the host; ST-RAM low area reserved 1:1 for the guest | Hatari, real Falcon/CT60 |

## 10. Test emulator and media

- Hatari: the user's fork https://github.com/slaapliedje/hatari, branch `debugger-memdump-mmu` (a88efcf = upstream main 2026-10-02 + memory dumps through the MMU). No branch has Falcon-specific work; its 030 MMU fixes (found under ASV on the TT) are already in upstream main. ATW800/ET4000 and DaynaPORT branches are VME/TT-SCSI work, not used here. `build-hatari.sh` clones, pins and builds it in `ref/hatari/build` (fetches cmake and the SDL2 headers into `build/deps` when the host lacks them; nice, -j1, about 30 min).
- Probe results (Hatari, Falcon, 030): TOS 4.04 runs the IDE root sector; EmuTOS 1.4 reaches the desktop without running it on IDE, SCSI or ACSI. EmuTOS desktop: 030/no FPU/14 MB at 1500 VBLs (25 s wall), 060 + 64 MB TT-RAM by 4000 VBLs.
- `run-hatari.sh`: Falcon, 030 without FPU (or `-c 060`), 14 MB ST-RAM, optional TT-RAM, IDE/SCSI image, screenshot at a fixed VBL count (deterministic), wall-clock timeout, config and NVRAM in a throw-away directory.

| Step | Needs | From |
|---|---|---|
| M1 | AMIX 2.1 tape (kernel objects, root files), EmuTOS (not needed with `--lilo`) | `toolchain/amix-root`, `ref/emutos-release` |
| M2 | boot-path test: a TOS 4.04 image (runs the root sector; test only, never tuned for) or a patched EmuTOS | aux root zip (user's), `ref/emutos-release` |
| M2 on hardware | the user's FPGA core with its own ROM | user |
| M3 | CT60 TOS for hardware-like 060 boots (optional; EmuTOS first) | `ref/ct60tos/flash.tos/ct60tos.bin` (built image; its `nonfree/` parts stay local) |
| M4 | EmuTOS; a real TOS 4.04 image only as an optional passthrough profile | `ref/emutos-release`; `media/` (user's own) |

Proprietary media (TOS 4.04, AMIX, ASV images) stay local and are never published.
