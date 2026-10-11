# CT60/CT63 060 Falcon: plan

Sources: Hatari (ref/hatari, a88efcf), ref/ct60tos (`flash.tos/patchs.txt`, `sdram.S`), kernel/atari/ATARI-PORT.md §3, §6, §8, the AMIX 040/060 port.

## 1. What Hatari models

| CT60 feature | Hatari |
|---|---|
| 68060 CPU, PCR, 060 MMU (access-error frames, FSLW) | yes: `--cpulevel 6 --mmu on`, `mmu_bus_error` with status060 (`cpu/cpummu.c`) |
| Internal FPU, unimplemented FP/integer instructions | yes: traps as on silicon (vector 11/61) unless `int_no_unimplemented`/`fpu_no_unimplemented` is set |
| ITT/DTT, caches | registers yes; caches are not coherent with the cycle model (no DMA snoop issue arises) |
| FastRAM | yes, but as TT-RAM: `--ttram 0..1024` MB at 0x01000000, `_FRB` buffer from TOS patch in `tos.c`; no SDRAM board, EEPROM or timing |
| CT60 TOS 1 MB in flash, boot flow | no: a 1 MB image at 0xE00000 loads, but its code reads the SDRAM EEPROM, flash and CT60 registers, which are unmapped; expect bus errors (test in C0) |
| `CT60`/`_FRB`/PMMU cookies, XBIOS 0xC60A-D, TOS copy to SDRAM | no (only with the real ROM) |
| Timers | MFP as on any Falcon; no CT60 temperature/fan/flash registers |
| SuperVidel, EtherNat | no |

Consequence: Hatari proves the 060 CPU paths and FastRAM above 16 MB. It does not prove the CT60 boot ROM, SDRAM init or cache set-up; the friend's machine does.

## 2. Gaps in our kernel

Done in the port (Amiga 060, hardware-accepted): 060 HAT, PCR/cputype, FPSP, ISP on vector 61, fault frames (`getfault040.s`, `krnxmemflt040.s`, FSLW decoding).

| Gap | Detail | Action |
|---|---|---|
| 060 base kernel | done: `PLATFORM=atari060` builds `unix-atari060.elf` (`relink-atari060.sh`, Atari objects over `unix-040`, `pstartata.s`) | — |
| Access-error frames | port reads FSLW from format-4 frames; the Atari I/O at 0xFFFFxxxx and 0x00FFxxxx aliases fault differently (060 bus errors on missing hardware vs 030 retry) | probe unmapped I/O in C0; keep DTT1 over 0x80000000+ |
| FastRAM above 16 MB | done: VM in FastRAM, ST-RAM `chipmem`, kernel moves itself there (`ata_reloc`); VM capped at 128 MB | 512 MB untested |
| Loader hand-over | FastRAM chunk from `_ramtop`; the running TOS image's physical start is passed; `cpusha %bc` before the jump (axbstart.s) | CT60 TOS reports sizes differently: read the `_FRB` and SDRAM size cookies |
| Cache/MMU set-up | `pstartata.s`: DTT0 0-1 GB noncacheable imprecise, DTT1 serialized for I/O; managed ST-RAM pages write-through, FastRAM copyback | untested on a CT60 |
| DMA coherence | Falcon DMA (disk, sound, Videl) is not snooped | `ata_dmasync()` around blitter runs; call it from every DMA driver; bounce FastRAM pages for SCSI |
| FPU | 060 FPU is native; transcendental ops trap to FPSP (vector 11 left to it) | test with an FP probe |
| movep, 64-bit mul/div | vector 61: `ata_isp61` emulates 64-bit `muls.l`/`mulu.l`; other forms panic | no movep, cas2, 64-bit divide in drivers |
| TOS-environment ROM | `ata_romva` returns the TOS start the loader passed when outside the VM region, else 0xE00000; on a CT60 TOS is copied to SDRAM and 0x00FFxxxx is not usable | untested under CT60 TOS; EmuTOS container keeps its own ROM image |
| Real-hardware unknowns | PCR rev 1 quirk, SDRAM cache-line behaviour | friend's tests, hardware-only items |

## 3. Stages

C0 and C1 are built and booted to a login in Hatari with `--ttram 64`; the rest is open.

- **C0 boot to a shell (Hatari `--cpulevel 6 --fpu internal --ttram 0`).** Build `unix-atari060`; `--lilo` boot; 060 PCR/caches on; ISP and FPSP linked. Tests: banner and `#` prompt; `fp060probe`; a movep/cas2/64-bit-mul user program; fsck on RAM root; 60 Hz tick; try the real `ct60tos.bin` once as ROM and record where it stops. Then IDE root through our loader.
- **C1 FastRAM (`--ttram 64`, then 512).** Loader adds the FastRAM chunk; VM in FastRAM, ST-RAM as `chipmem`. Tests: `sysmem` shows both; fork/exec stress (the `stress` dir) and swap with processes above 16 MB; IDE DMA-free PIO plus SCSI bounce when SCSI lands; boot with 0, 4 and 512 MB TT-RAM.
- **C2 TOS environment.** Environment ROM from `_sysbase`; EmuTOS container with FastRAM visible via `_FRB`; ST-RAM low area stays 1:1. Tests: GROUP=tos on Hatari 060; desktop reached; a TOS program allocating FastRAM (Mxalloc mode 1).
- **C3 X.** Falcon X on Videl under the 060 kernel (FPU-using clients, xclock); then the SuperVidel display-service driver (real hardware only; Hatari has no SuperVidel). Tests: GROUP=display on Hatari; friend's CT60 for SuperVidel.

Every stage ends with a run on the friend's machine; Hatari results are necessary, not sufficient.
