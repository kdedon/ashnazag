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
| 060 base kernel | `relink-atari.sh` builds the 030 image; no `unix-atari060` yet | link Atari objects over `unix-040` (4 KB pages, `pstart040.s`) |
| Access-error frames | port reads FSLW from format-4 frames; the Atari I/O at 0xFFFFxxxx and 0x00FFxxxx aliases fault differently (060 bus errors on missing hardware vs 030 retry) | probe unmapped I/O in C0; keep DTT1 over 0x80000000+ |
| FastRAM above 16 MB | `config` takes one region; ST-RAM must become `chipmem`, VM = FastRAM (§3) | `config` region choice from the boot record; 4 KB page tables over 0x01000000+ |
| Loader hand-over | `axbload.c` passes two `BI_MEMCHUNK`s; FastRAM size comes from `_FRB`/`ramtop`/TT-RAM probe, which CT60 TOS reports differently | read `_FRB` cookie and the SDRAM size cookie; add a FastRAM chunk; `cpusha %bc` already before the jump (axbstart.s) |
| Cache/MMU set-up | ITT0/DTT0 0-0x7FFFFFFF, DTT1 0x80000000+ inhibited; ST-RAM write-through, FastRAM copyback | `cputype060.s` values; ST-RAM write-through needs per-page modes or DTT0 split (CT60 TOS: PMMU tree, ST-RAM WT) |
| DMA coherence | Falcon DMA (disk, sound, Videl) is not snooped | `dma_cache040.s` around every ST-RAM transfer; bounce FastRAM pages for SCSI |
| FPU | 060 FPU is native; transcendental ops trap to FPSP | link `fpsp060_glue.s`; test with `fp060probe` |
| movep, 64-bit mul/div | trap to ISP | no movep in drivers; ISP via vector 61 |
| TOS-environment ROM | `ata_romva(n)` maps 0xE00000 and allows 1 MB only when cputype is 060; on a CT60 TOS is copied to SDRAM and 0xE00000 may be flash behind slower access, and 0x00FFxxxx is not usable | map the copy the loader saw (`_sysbase`), not a fixed address; keep 0xE00000 for Hatari/real ROM; EmuTOS container keeps its own ROM image |
| Real-hardware unknowns | PCR rev 1 quirk, SDRAM cache-line behaviour | friend's tests, hardware-only items |

## 3. Stages

- **C0 boot to a shell (Hatari `--cpulevel 6 --fpu internal --ttram 0`).** Build `unix-atari060`; `--lilo` boot; 060 PCR/caches on; ISP and FPSP linked. Tests: banner and `#` prompt; `fp060probe`; a movep/cas2/64-bit-mul user program; fsck on RAM root; 60 Hz tick; try the real `ct60tos.bin` once as ROM and record where it stops. Then IDE root through our loader.
- **C1 FastRAM (`--ttram 64`, then 512).** Loader adds the FastRAM chunk; VM in FastRAM, ST-RAM as `chipmem`. Tests: `sysmem` shows both; fork/exec stress (the `stress` dir) and swap with processes above 16 MB; IDE DMA-free PIO plus SCSI bounce when SCSI lands; boot with 0, 4 and 512 MB TT-RAM.
- **C2 TOS environment.** Environment ROM from `_sysbase`; EmuTOS container with FastRAM visible via `_FRB`; ST-RAM low area stays 1:1. Tests: GROUP=tos on Hatari 060; desktop reached; a TOS program allocating FastRAM (Mxalloc mode 1).
- **C3 X.** Falcon X on Videl under the 060 kernel (FPU-using clients, xclock); then the SuperVidel display-service driver (real hardware only; Hatari has no SuperVidel). Tests: GROUP=display on Hatari; friend's CT60 for SuperVidel.

Every stage ends with a run on the friend's machine; Hatari results are necessary, not sufficient.
