# Atari TT030: plan

Target: a stock TT030. 68030 at 32 MHz with 68882, 2-10 MB ST-RAM, TT-RAM from none to 256 MB at 0x01000000, built-in TT video, internal SCSI (5380 with its own DMA), ACSI, floppy, SCC, two MFPs, RTC, VME slot. Expansions (large TT-RAM boards, VME graphics such as the ATW800/2, Riebl Ethernet, SCSI emulators) are optional and never assumed.

Kernel: the Falcon030 image (`PLATFORM=atari`); the machine is picked at boot from `_MCH` (2 = TT). One image runs on both.

Sources: TT030 Hardware Reference Manual (Atari, June 1990); Atari Compendium register map (https://freemint.github.io/tos.hyp/en/hardware_registers.html); Linux `arch/m68k/atari/ataints.c`, `config.c`, `drivers/video/fbdev/atafb.c`, `drivers/scsi/atari_scsi.c`; NetBSD `sys/arch/atari` (`atari5380.c`, `grfabs_tt.c`); Hatari `src/mfp.c` (GPIP map of both MFPs), `src/scu_vme.c`; ASV notes (`ref/atari-sysv-sp1`: README, `tools/snd/PHASE0.md`, `kernel/patch-ttram.py`).

## 1. TT against the Falcon

| Item | TT | Falcon | Kernel state |
|---|---|---|---|
| CPU/FPU | 68030 + 68882 | 68030, FPU optional | stock 030 image; FPU used natively, FPE unused |
| RAM | ST-RAM 2-10 MB; TT-RAM 0x01000000, 0-256 MB | ST-RAM ≤ 14 MB | TT-RAM is the VM, ST-RAM `chipmem` (TT0) |
| Video | TT shifter: ST low/med/high, TT low 320x480x8, TT med 640x480x4, TT high 1280x960x1 (TT mono monitor); mode 0xFFFF8262, palette 0xFFFF8400 (256 x 12-bit) | Videl | console + session modes (TT0) |
| MFPs | ST MFP 0xFFFFFA01 (as Falcon; GPIP7 = mono detect XOR sound); TT MFP 0xFFFFFA81: GPIP5 SCSI DMA, GPIP6 RTC, GPIP7 5380 (active high), GPIP2 SCC DMA, GPIP3 SCC B ring, USART = serial 2 | one MFP | TT MFP quiet, vectors 80-95, handler table (TT0) |
| SCU | 0xFFFF8E01 system mask, 0xFFFF8E0D VME mask: gates VBL/HBL, MFPs, SCC | none | masks set as Linux does: 0x10, 0x60 (TT0) |
| SCSI | 5380 regs 0xFFFF8781 + 2n, own DMA 0xFFFF8701-8717, reaches ST-RAM and TT-RAM | 5380 behind the ST DMA chip | SCSI agent's `tt.c` (PIO), not yet run |
| ACSI, floppy | ST DMA chip, ST-RAM only | floppy only | later |
| IDE | none | yes | `ata_busprobe` finds none |
| Blitter, DSP | none | yes | display service skips the blitter on a TT |
| Sound | STE/TT 8-bit DMA, no codec | 16-bit + crossbar | `dmasnd` module already handles TT |
| SCC | 0xFFFF8C81, as Falcon; ports A (LAN/serial) and B | same | unchanged |
| RTC | MC146818 at 0xFFFF8961/63, as Falcon | same | `atartc.c` unchanged |
| VME | A24/A16 windows 0xFE000000-0xFEFFFFFF | none | later, modules only |
| Cartridge port | 0xFA0000-0xFBFFFF, as ST/Falcon | same | NetUSBee module unchanged |

ASV facts reused: its kernel claims TT MFP GPIP3 (SCC), GPIP6 (RTC clock, 128 Hz), GPIP7 (SCSI); ST MFP Timer A and GPIP7 stay free for DMA sound. We keep the ST MFP Timer A tick (240 Hz / 60 Hz `HZ`), so the RTC interrupt stays free. ASV's TT-RAM ceiling (page structures in ST-RAM) does not apply: our page structures follow the kernel into TT-RAM.

## 2. Stages

- **TT0 boot to a shell (done, Hatari `--machine tt`).** Machine from `_MCH`; console on the TT shifter (ST high 640x400 on a colour monitor, TT high 1280x960 on the mono monitor, by ST MFP GPIP7); TT MFP silenced, vectors 80-95 to a dispatcher with `ata_ttmfp(channel, fn, arg)` for drivers; SCU masks; kernel moves itself into TT-RAM when there is any (the CT60 relocation table, now also on the 030), VM capped at 128 MB; loader sizes TT-RAM from `_ramtop`, or probes it when TOS did not; display service TT palette and modes; `uname` says Atari TT030.
- **TT1 disks.** Run `tt.c` PIO on the TT bus (Hatari `--scsi 0=`, then the user's TT); root on SCSI: `sd` must load before root, so either link `sd`+`scsi` in for a SCSI root (TT has no IDE) or keep a RAM-disk first stage. Then TT SCSI DMA: TT MFP GPIP5/GPIP7 through `ata_ttmfp`, odd residual fix-up, 030 data cache invalidated after DMA into memory. Boot through our root sector under TOS 3.06 (test the `ramprobe` path with `_ramvalid` cleared). Swap, fsck, multi-user.
- **TT2 machine services.** TT MFP USART as a second serial port; SCC on the TT clock; RTC alarm/periodic unused; floppy and ACSI via the ST DMA chip (bounce through ST-RAM); TT-RAM above 128 MB (lift the cap once the 030 kvseg limit is measured).
- **TT3 display.** FBIOSMODE on TT modes (table in `atads.c`); X on the TT shifter (interleaved planes, as the Falcon X server); ST-RAM-only video, so shadows stay in TT-RAM. TOS environment on a TT host: passthrough with the machine's own TOS 3.06, TT shifter state saved per session (done for Unix sessions).
- **TT4 optional hardware, modules only.** `atw` display module: loaded after boot, gives the display service and X the ATW800/2 screen; console and boot always use the built-in video; nothing in the base kernel or loader. Riebl VME Ethernet module; VME interrupts through the SCU VME mask. DaynaPORT already works on the SCSI core.

Each stage ends on the user's TT; Hatari results are necessary, not sufficient.

## 3. Hatari

`run-hatari.sh -m tt` (68030 + 68882, `--monitor vga|mono`, `-t MB` TT-RAM). Hatari's TT models both MFPs, the SCU, TT shifter and TT SCSI; no VME cards (the user's fork has an ATW800/2 model, not used for the base).
