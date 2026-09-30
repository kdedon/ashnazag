# Mac boot path and kernel memory map

How A/UX 3.1 boots its kernel on a Mac, what our AMIX-based ELF kernel can reuse, a kernel virtual map for Macs, split-RAM handling, and a bring-up plan (Quadra 800 first).

Sources: the A/UX 3.1 boot partition (HFS, "MacOS" partition) — `A/UX Startup` 3.1 (CODE resources) and the standalone `launch` program; A/UX `/unix` (COFF, unstripped); the AMIX kit and the AMIX 040/060 port; the IIci and Quadra 700/900 ROM images; NetBSD/mac68k `locore.s`, `machdep.c`, `include/cpu.h` (BSD); QEMU `hw/m68k/q800.c` (device addresses, direct-boot protocol). Addresses: `seg:off` = A/UX Startup CODE segment and offset from the code start; `launch` addresses are its link addresses (text at 0x180000); kernel addresses are A/UX `/unix` link addresses. **(unc.)** marks inferences not fully traced.

Tool: [`tools/maccode.py`](../tools/maccode.py) disassembles a Mac application's CODE resources with jump-table calls resolved (`maccode.py <rsrc> list|jt|dis <seg>|grep <hex>`).

## Summary

- The kernel is loaded by `launch`, a 12 KB COFF program that runs **inside** A/UX Startup (the "sash" shell) and calls sash services through a small syscall ABI. `launch` reads the COFF kernel, builds a load list and an info block (magic `'Pigs'`), and calls sash service `0xFB`. Sash places everything in physical memory, turns the MMU off and copies the segments with a position-independent trampoline, then jumps to the COFF entry.
- Kernel entry state: supervisor, **IPL 7, MMU off**, on 68040 **caches off** (CACR = 0, `cpusha`), TT registers and VBR **left as Mac OS set them**, SP **stale** (a Mac logical address). Registers: **`d0` = 0x536D7201** (`'Smr'` + version 1), **`a0` = physical address of the info block**.
- Hand-off data: the info block (slot board IDs/versions, root/swap devices, flags, section layout, machine type, a copy of the Mac drive queue) and **an 8 KB copy of Mac low memory** (vectors and globals: `ROMBase`, `UnivInfoPtr`, `HWCfgFlags`, `ScrnBase`, …). **No RAM size and no video description**: A/UX sizes RAM from the memory controller and finds video through the ROM.
- The A/UX "machine type" is **Gestalt `'mach'` − 2** (= the SysEnvirons numbering): IIci 9, IIfx 11, Quadra 900 18, Quadra 700 20, **Quadra 800 33**. [aux-startmac-boot.md](aux-startmac-boot.md) uses this numbering.
- Our ELF kernel can use the **stock** A/UX Startup + `launch` if it is wrapped in a COFF container with a small `pstart` entry section. Better medium-term: keep A/UX Startup and replace only `launch` with our own sash program that passes RAM ranges and video. A stand-alone booter (NetBSD-Booter-like) is the last step, not the first.
- Kernel VA map: keep AMIX's window (0x40000000–0x49FFFFFF) where it is; map **Mac I/O VA = PA** (0x50F00000) and the **ROM at an alias VA (0x52800000)** through static page tables; supervisor-only ITT0/DTT0 (0–1 GB) and DTT1 (2–4 GB, NuBus and frame buffers) as in the port. User space (A/UX personality) is a separate tree and keeps ROM at 0x40800000.
- Quadra 800 RAM is one contiguous range from 0 (djMEMC), so AMIX's single-region VM needs no change for the first target.
- First milestones run in QEMU `-M q800 -kernel` (ELF, Linux-style bootinfo, **no ROM needed**), serial console on the SCC.

## 1. How A/UX Startup starts the kernel

### 1.1 Pieces

| Piece | Where | Role |
|---|---|---|
| A/UX Startup 3.1 (creator `SASH`) | HFS root; 13 CODE segments (`Main`, `VBLtask`, `Init`, `Misc`, `salib`, `saio`, `Password`, `specfs`, `common`, `svfs`, `ufs`, `%A5Init`, `STDCLIB`) | Mac application; standalone shell (sash) with its own SVFS/UFS/HFS file access, runs COFF standalone programs |
| `launch` (type `COFF`) | HFS root | kernel loader; sash variable `autolaunch` = `launch` (resource `SASH` 2) |
| `bin/*` | HFS `bin:` | standalone `fsck`, `ls`, `newfs`, … (same COFF/sash ABI) |

`launch` usage (from the shipped help text, `SASH` 3): `launch [options] [pathname]`; `-a` always autoconfigure, `-b vaddr` debug reset, `-d` dump kernel info, `-e` use partition value, `-f` eject floppy, `-k value` OR into the flags word, `-m` pathname is a Macintosh (HFS) path, `-n` never autoconfigure, `-p dspec` swap device, `-r` root is `$ROOT`, `-s` load symbols, `-S` single user, `-v` verbose. Without a pathname it uses the first line of `/nextunix`; if the kernel's `MODULES` section does not match the NuBus boards, it boots `newunix` for autoconfiguration. Device specs are `(scsi-id,0,slice)`; sash's `ROOT` defaults to `(default)/`.

### 1.2 The sash service ABI (what a replacement `launch` must use)

`launch` is linked at 0x180000 (entry 0x182050). Sash puts an externs block just below it:

| Address | Content |
|---|---|
| 0x17FEA0 | service entry (called with `jsr`) |
| 0x17FEA4 | service number (set by the caller) |
| 0x17FEA8 | pointer to the caller's argument list |
| 0x17FEAC | errno out |
| 0x17FEB0 | magic `'eRyK'` (0x6552794B); `launch` refuses to run without it |
| 0x17FEB4/B6 | ABI version word 2 and minor ≥ 4 |

Stubs are `moveq #n,d0; jmp common` (launch 0x182A0C…). Low numbers follow Unix (`exit` 1, `open` 5 …); sash extensions used by `launch`:

| No. | Use in `launch` | Sash side |
|---|---|---|
| 0xF3 | call a Mac routine with arguments (used for `GetTrapAddress`, Gestalt, Slot Manager) | 2:0190 |
| 0xF6 | root device info (ctlr, drive, partition, cluster) | |
| 0xF7 | memory info: {bank0 size, bank0 base, bank1 size, bank1 base, machine type} | 4:3F02 |
| 0xFB | **launch**(nentries, list, entry, flags) — does not return | 1:414E → 4:4878 |
| 0x54 | program space (sbrk-like; "Not enough program space to load kernel") | |

### 1.3 What `launch` does

1. **Info block** (0xBE bytes, magic `'Pigs'` 0x50696773) plus an appended copy of the Mac **drive queue** (`DrvQHdr` $308: 4 flag bytes + 16 bytes per element, links made relative):

   | Off | Field |
   |---|---|
   | 0x00 | magic `'Pigs'` |
   | 0x04 | board ID per slot [16] (slots 9–$E filled from the Slot Manager, others 0xFFFF) |
   | 0x44 | board version per slot [16] (major×100+minor) |
   | 0x84 | autoconfig command (0 OK, 1 RUN) |
   | 0x88/0x8A/0x8B | root device (word, byte) and cluster |
   | 0x8C | text vaddr, paddr, size; 0x98 data; 0xA4 bss (9 longs) |
   | 0xB0 | machine type (word) = Gestalt `'mach'` − 2 |
   | 0xB2 | offset of the drive-queue copy (0xC2) or 0 |
   | 0xB6 | flags: 1 verbose, 2 parity exists, 4 parity on, 8 versioning, 0x10 single user (+ `-k`) |
   | 0xB8 | version byte (1) |
   | 0xB9 | root partition |
   | 0xBA/0xBC/0xBD | swap device (default: root disk, partition 1) |

2. **Kernel file checks**: COFF magic 0x150; at most 8 sections; loads every section except `STYP` DSECT/NOLOAD/GROUP/PAD/COPY; optional `MODULES` (COPY) section checked against slot board IDs.
3. **Load list**, 12-byte entries `{size, source, physical destination}`:
   - entry 0: info block → **`pstart.s_paddr − 0x400`**;
   - entry 1: **0x2000 bytes from logical 0 (Mac low memory) → `pstart.s_paddr − 0x4000`**;
   - one entry per loaded section at its `s_paddr`. If the Mac reports a second bank and bank 0 is ≤ 2 MB, `.text`/`.data`/`.bss` destinations are offset by bank 1's base (the `pstart` section is not). Special case: `pstart` at 0x500 or 0x2000 → info at 0x400 (older kernels).
   - The COFF section **named `pstart`** marks the entry area; the entry itself is the a.out entry.
4. `sash 0xFB(n, list, entry, (b8 << 8) & mask)`; sash takes version = `(flags >> 8) & 0xFF`, or 1 if zero.

For A/UX 3.1 `/unix`: `pstart` 0x54000 (VA = PA, 0x7D90 bytes), `.text` VA 0x10000000 at PA 0x5C000, `.data` 0x11000000 at 0xED000, `.bss` 0x12000000 at 0xFF000, entry 0x54000. So the info block lands at 0x53C00 and the low-memory copy at 0x50000.

### 1.4 What sash does in service 0xFB (1:414E, 4:4878)

- Checks (dialogs on failure), shows "Launching", closes Mac drivers — unit-table walk and slot sResources — except boards listed in resource `List` 0 ("un-Close()-able board ids") (4:49E4). **(unc.)** exact driver set.
- Physical memory picture: `GetPhysical` over logical RAM (0 … Gestalt `'lram'`, retrying 8 KB smaller on `paramErr`) when `MemoryDispatch` exists; otherwise one range {0, `MemTop`} (4:38F2, 4:3D08).
- Orders copies so sources and destinations do not collide and finds a free physical area with a logical alias for the trampoline and list (4:4030–4:476C; **(unc.)** not traced in detail).
- On IIci/IIsi (RBV) sets VIA1 DDRB/ORB bit 6 before the jump (4:48E8). **(unc.)** purpose.
- Trampoline (2:01EE–2:02BC, copied to physical RAM): `move #$2700,sr`; builds `{CRP = 0x7FFF0001:00000000, TC = 0}`; turns translation off by one of four methods; byte-copies the list (entry 0 last); `a0` = info block PA, `d0` = 0x536D7200 | version; `jmp entry`.

| Method | When | MMU / cache effect |
|---|---|---|
| 3 | Gestalt `'proc'` = 5 (68040) | `CACR = 0`, `cpusha`, `TC = 0`. TT registers, VBR untouched |
| 2 | Gestalt `'mach'` = 13 (IIfx) | `jmp ROMBase+$44` **(unc.)**: on the $067C ROMs this is the Foreign-OS table itself, so it only makes sense for a different IIfx ROM layout |
| 1 | 020/030 with `MemoryDispatch` | ROM `SwitchMMU` (Foreign-OS table entry 5, ROM $5C): clears CACR enables, `pmove TC` off, `pflusha`, `pmove CRP` = identity, `pmove TC = 0`, restores enables with both caches cleared, `jmp (a1)` |
| 0 | 020/030 without it | `pmove TC = 0`; CACR untouched |

### 1.5 A/UX kernel's first instructions (`_start`, 0x54000)

1. `move #$2700,sr`; if `d0 == 0x536D7201`: `kernelinfoptr = a0`, copy the 9 section longs (+0x8C) and machine type (+0xB0 → `machineID` 0x5AFA6); else assume the old layout at 0x400.
2. Clear BSS by **physical** address; look for a COFF symbol table on the page after BSS; SP = end of that.
3. `VBR = 0x50000` (the low-memory copy); CPU detection by F-line/`movec` probes (cputype 3/4/5 = 020/030/040), FPU probe (saves the Mac's FPU vectors $C0–$D8 on 040).
4. `cpuinit`, `boardinit` (switch on `machineID`), `memsize` (per controller: `banksize` for the Mac II family probes 0 and 0x04000000; `orwellsize`; `memcdjsize` reads **djMEMC 0x50F0E02C**, `(reg & 0xFF) << 22` = end of RAM), copies 0x50000–0x53FFF down to **PA 0** (the kernel's Mac low memory, unless RBV video owns bank A), `mmusetup`, `vadrspace`, SP = 0x13000000, `main`, `rte` to init.
5. A/UX's own 68040 map (for contrast): VBR 0; `ITT0/DTT0 = 0x403FA040` and `ITT1/DTT1 = 0x807FA040` — 1–4 GB identity, supervisor only, non-cacheable serialized (ROM, I/O, NuBus); RAM and kernel sections through page tables. The kernel's ROM is at VA 0x40800000.

### 1.6 Reuse options

| | (a) Keep A/UX Startup + stock `launch` | (a′) Keep A/UX Startup, own `launch` | (b) Own Mac booter |
|---|---|---|---|
| Work | `elf2coff` wrapper + entry shim | a sash COFF program (≈ `launch` size) using §1.2 | full Mac application |
| Kernel file | COFF 0x150 with `pstart` section; on HFS (`-m`) or A/UX UFS/SVFS | ELF directly | ELF |
| RAM info | none: probe controller (djMEMC etc.) | GetPhysical ranges via 0xF3 | same |
| Video info | only low-memory `ScrnBase`/`ScreenRow` (depth/size unknown) | `GetMainDevice` PixMap via 0xF3 | same |
| MMU-off copy, placement | sash | sash | ours |
| Distribution | needs the user's A/UX Startup | same | self-contained |
| Risks | layout rules below; 8-section limit | undocumented ABI (§1.2) | reimplementing placement for 030/040/IIfx cases |

**Rules for (a)**: a COFF container (file magic 0x150, a.out magic 0x108/0x10B) whose sections are the ELF `PT_LOAD` segments with `s_paddr = s_vaddr` (AMIX runs identity-mapped below 1 GB), plus a `pstart` section holding the entry shim. Put `pstart` at **PA 0x4000**: the low-memory copy then lands on PA 0 (Mac low memory copied onto itself) and the info block at 0x3C00. Refuse to run if `.text` was relocated (bank-offset case). The shim must: set SP; set VBR; clear ITT0/1, DTT0/1 (040/060) or TT0/1 (030), as NetBSD does before enabling its MMU; disable caches; mask VIA1/VIA2 (`IER = 0x7F`) and the SCC; then call AMIX `stext` with `d1 = a0` (`stext` passes only `d0`/`d1` to `config`).

**NetBSD/mac68k Booter, for comparison** (public design, from the kernel side): the booter leaves **Mac OS's MMU mapping on**, copies the kernel to logical 0, and passes `a1` = an environment buffer `"VAR=value\0…\0\0"` and `d4` = flags (`locore.s` `start`). Variables include `MACHINEID`, `PROCESSOR`, `MEMSIZE`, `ROMBASE`, `VIDEO_ADDR`, `ROW_BYTES`, `SCREEN_DEPTH`, `DIMENSIONS`, `BOOTHOWTO`/`SINGLE_USER`, `SERIALCONSOLE`, `END_SYM`, `BOOTTIME`, `GMTBIAS`, `HWCFGFLAGS`, `ADBDELAY`. The kernel walks the Mac page tables (`get_mapping`) to find physical RAM ranges and the frame buffer's PA, then remaps ROM to a new kernel VA (`mrg_fixupROMBase`) and I/O VA = PA ("some of the ROM routines have hard-coded addresses for e.g. VIA1"). On djMEMC machines (Quadra 800 and relatives) it fakes one range `0 … MEMSIZE`. Lesson: A/UX's MMU-off hand-off is simpler for an identity-mapped kernel; NetBSD's variable list is a good checklist of what a booter should pass.

**Recommendation**: define one kernel-internal boot record (Linux/m68k bootinfo tag format: {u16 tag, u16 size, data}, as QEMU and Linux booters already produce: `BI_MACHTYPE`, `BI_CPUTYPE`, `BI_MEMCHUNK`, `BI_MAC_MODEL`, `BI_MAC_VADDR`/`VDEPTH`/`VDIM`/`VROW`, `BI_MAC_SCCBASE`, `BI_COMMAND_LINE`). The entry shim converts: QEMU bootinfo (as is), A/UX `'Pigs'` + low memory (a), our own `launch` (a′) and later booter (b) emit it directly.

## 2. Kernel virtual memory map

### 2.1 Constraints

- AMIX (and the port) run the kernel identity-mapped below 1 GB; kernel window 0x40000000–0x49FFFFFF (`u` 0x40000000, `syssegs` 0x40040000, `kvsegmap` 0x40440000, `kvsegu` 0x48440000) through the supervisor tree; the port's `pstart040.s` points **all** root entries 32–63 (0x40000000–0x7FFFFFFF) at its flat `kptr040` pointer-table array, so static mappings anywhere in quadrant 1 are just more descriptors in that array.
- TT registers win over page tables, so no TT may cover the kernel window. Budget: 040/060 ITT0/1 + DTT0/1; 030 TT0/TT1 (unused by AMIX); 68851 none.
- Mac ROM PA 0x40800000 (mirrored through 0x40000000–0x4FFFFFFF) sits inside the window. ROM code runs at 0x00800000 in 24-bit mode and NetBSD relocates it to an arbitrary kernel VA, so it does not need VA = PA; it does need I/O at VA = PA and Mac low memory at VA 0.
- Quadra 800 devices (QEMU `q800.c`, consistent with A/UX and NetBSD): VIA1 0x50F00000, VIA2 0x50F02000, SONIC 0x50F0A000 (PROM 0x50F08000), SCC 0x50F0C020, djMEMC 0x50F0E000, ESP (53C96) 0x50F10000 / pseudo-DMA 0x50F10100, ASC 0x50F14000, IOSB 0x50F18000, SWIM 0x50F1E000; I/O decodes in 256 KB slices mirrored over 0x50000000–0x53FFFFFF; machine-ID register 0x5FFFFFFC; built-in video 0xF9000000 (NetBSD `intvid_info`: 1 MB); NuBus standard slots 0xF9000000–0xFEFFFFFF, super slots 0x90000000–0xEFFFFFFF. IIci/IIsi RBV 0x50F26000, IIfx 0x50F1A000 (A/UX Startup 3:0CF6).
- Device registers need non-cacheable **serialized** access on 040/060 (CM = 10); frame buffers can be non-serialized (CM = 11).
- User space is a separate tree (URP / CRP with SRE); the A/UX personality needs Mac RAM at 0, ui page 0x3000, `Patch.067C` 0x4000, A/UX text 0x10000000, ROM 0x40800000, `/shlib` 0x47C00000–0x47FC8000, frame buffers VA = PA (16 MB per card), plus AMIX natives at 0x80800000/0xC0800000. The supervisor-only TT fix ([aux-kernel-design.md §1a](aux-kernel-design.md)) keeps these away from physical memory.

### 2.2 Proposed kernel (supervisor) map

| Kernel VA | Maps | Mechanism (040/060) | Cache | 030 / 68851 |
|---|---|---|---|---|
| 0x00000000–0x3FFFFFFF | PA identity: RAM (Q800 ≤ 136 MB; QEMU ≤ 1 GB), Mac low memory at VA 0 (PA 0–0x3FFF reserved), kernel image from PA 0x4000/0x10000 | ITT0 0x003FA000, DTT0 0x003FA060 (S only) | I: WT, D: CI (port policy) | root slot 0 early-termination, S only |
| 0x40000000–0x49FFFFFF | AMIX kernel window (unchanged) | root 32–36 | per PTE | `st_top1` entries 0–0x4FF |
| 0x4A000000–0x4FFFFFFF | unmapped guard | | | |
| 0x50000000–0x50FFFFFF | **Mac I/O, VA = PA**: page tables for 0x50F00000–0x50FFFFFF (4 × 256 KB tables); optional 0x50000000–0x5003FFFF | root 40 → `kptr040[8]` | CM 10, S, supervisor | **TT0 = 0x50008543** (16 MB, S, CI) or `st_top1` 128 KB entries |
| 0x52000000–0x52FFFFFF | **ROM alias**: 0x52800000 → PA 0x40800000, ROM size (header +0x40; 1 MB on Q700/800) | root 41, read-only PTEs | WT cacheable (immutable) | `st_top1` entries (raise its limit from 0x4FF) |
| 0x5FFFF000 | machine-ID register page, VA = PA | root 47 | CM 10 | same |
| 0x80000000–0xFFFFFFFF | PA identity: NuBus super slots, slot space, built-in video 0xF9000000 | DTT1 0x807FA060 (S only; port value) | CI non-serialized | root slots 2/3 early-termination CI |
| ITT1 | unused (spare) | | | TT1 spare |

Why 0x52800000: it keeps the ROM's low 24 bits (0x800000) as in 24-bit mode, and quadrant-1 addresses above 0x4A000000 are outside every AMIX VM range. Kernel low-memory `ROMBase` ($2AE) = 0x52800000 for kernel-mode ROM calls.

Open points:
- Confirm AMIX/port `kvm_init`/`segkmem` never allocate above 0x49FFFFFF (the `xclosef` check accepts ≤ 0x48FFFFFF). **(verify)**
- DTT1 CM: device registers on NuBus (e.g. video CLUT, DAFB 0xF9800000) would prefer serialized (0x807FA040, A/UX's choice); frame buffer speed prefers 0x60. Start with 0x40, measure.
- Kernel-mode ROM calls (Slot Manager, video Control/Status, PRAM) raise A-line traps; AMIX's VBR is `M68Kvec`, whose slot 10 goes to `nullvect`. Either the A/UX-personality gate for slot 10 also handles kernel-mode A-line by jumping to the ROM dispatcher, or ROM calls run with a private vector table. Not needed before video work; serial/SCSI/clock drivers talk to hardware directly.

### 2.3 What to override in AMIX

| Item | Change |
|---|---|
| entry shim (new `pstart` section / ELF entry) | §1.6 rules; convert boot protocol to bootinfo; `jmp stext` |
| `config(d0,d1)` (Amiga `support.c`, replaced) | parse bootinfo; `MAINSTORE = 0`, `VSIZOFMEM` = end of the RAM range holding the kernel; `kernel_load_address`; `chipmem = 0`; save video/ROM/model; quiesce VIAs/SCC |
| `pstart040.s` (port) and a Mac `pstart` for 030/68851 | supervisor-only ITT0/DTT0; I/O and ROM-alias page tables; machine-ID page; reserve PA 0–0x3FFF; on 030 TT0 for I/O |
| `mlsetup`/`kvm_init`/`page_init` | none for Q800 (one region). Pages below the kernel stay out of VM (`page_init` starts at `firstfree`) |
| `vtop040`, `valid_usr_range` | as in [aux-kernel-design.md](aux-kernel-design.md) §1a (A/UX user buffers below 0x80000000) |
| `haltsys`/`rtnfirm` | MMU off, jump to ROM reset (or VIA-based restart) |
| `putchar`/`getchar`, `hw_clkstart`, `clkreld` | SCC polled; VIA1 T1 at 60 Hz (783.36 kHz / 13056 **(verify on Q800)**, IPL 1) |

## 3. Split RAM

| Machines | Physical RAM | Source |
|---|---|---|
| Quadra 800, 610, 650, Centris 610/650 (djMEMC) | one range 0 … `(0x50F0E02C & 0xFF) << 22` | A/UX `memcdjsize`; NetBSD fakes one range here |
| Quadra 700/900/950 (Orwell) | per A/UX `orwellsize` **(unc.)** | |
| Mac II family (II, IIx, IIcx, SE/30, IIci, IIfx) | bank A at 0, bank B at 0x04000000 | A/UX `banksize(0)`, `banksize(0x4000000)` |
| IIci/IIsi with built-in video | bank A also frame buffer | A/UX `rbv_monitor` |

Steps:
1. **Quadra 800 (first)**: nothing to split. The booter reports one `BI_MEMCHUNK`; `config` cross-checks against djMEMC and sets `MAINSTORE = 0`, `VSIZOFMEM = end`.
2. **Two banks, minimal**: report all ranges in bootinfo (GetPhysical list, like NetBSD's `low[]/high[]`); `config` uses the range that contains the kernel (as the Amiga `config` grows the region containing `end`) and prints what it ignores. Load the kernel in the larger bank.
3. **Hole covering**: `MAINSTORE = 0`, `VSIZOFMEM` = end of bank B, then withdraw the hole's pages before they reach the free list. Cost: `page_t`s for the hole (≤ 64 MB → 16 K pages); needs `page_init`/`memialloc` semantics checked (open in [amix-platform-interface.md](amix-platform-interface.md) §10).
4. **Real multi-segment VM**: `plat_memsegs[]` from the proposed platform interface.

## 4. Bring-up plan

| # | Milestone | Where | Test |
|---|---|---|---|
| B0 | Tooling: fully linked Mac kernel (port build, text at PA 0x10000, entry shim), `elf2coff` wrapper for path (a) | host | `coffhdr.py` accepts it; section count ≤ 8 |
| B1 | Shim prints on SCC channel (polled, MMU off) | QEMU `-M q800 -m 128 -kernel unix.elf -serial stdio` (no ROM needed; QEMU loads the ELF, puts bootinfo after the image end, starts at the ELF entry via the reset vector; SP is not meaningful) | banner + parsed bootinfo (model, memchunk, video) |
| B2 | `stext` → Mac `config` → `pstart` with the §2.2 map, MMU on, console through VA = PA I/O | QEMU | print TC/TT/root dump; read VIA1 through the mapping |
| B3 | Exceptions and clock: `M68Kvec` live, VIA1 T1 60 Hz, `clock_int`; NMI (level 7) → debugger print | QEMU | `lbolt` advances; `timeout` fires |
| B4 | `mlsetup`/`main` to a RAM-disk root (small in-kernel s5 image) and `/sbin/init` → `sh` on serial | QEMU | shell prompt; `ps`, `date` |
| B5 | 53C96 SCSI (ESP + pseudo-DMA) driver, Apple partition map, root on disk | QEMU (`scsi-hd`) | boot from disk image |
| B6 | Real Quadra 800 via A/UX Startup, path (a) (`launch -m -v (mac):unix.coff`) | hardware, serial cable on the modem port | same banner as QEMU; cache and TT behaviour on a real 040; compare `-S`/`-v` flags |
| B7 | Frame-buffer console (built-in video at 0xF9000000, depth/rowbytes from bootinfo or `ScrnBase`/`ScreenRow`) | both | text on screen |
| B8 | Own `launch` (path a′) with RAM ranges and video; dump the Q800 ROM (F1ACAD13) for QEMU | both | QEMU with ROM + A/UX Startup boots our kernel the same way as hardware **(verify QEMU's A/UX Startup support)** |
| B9 | SONIC Ethernet, ADB, then the A/UX personality milestones (M0–M9 of aux-kernel-design.md) | both | `startmac` with ROM at user 0x40800000 |

Debug aids: early pixel writes to 0xF9000000 before the SCC works; keep a copy of the Mac low memory at PA 0 for ROM calls later; leave ITT1 free for a temporary debug window.

## 5. Uncertainties

- Sash placement logic (4:4030–4:476C) and the IIfx method-2 jump are not traced.
- The VIA1 bit-6 write on RBV machines before the jump: purpose unknown.
- Whether ROM routines the kernel will call have any absolute self-references (NetBSD's relocation suggests none that matter).
- Quadra 800 VIA clock rate, SCC channel used by QEMU's `-serial`, and djMEMC register decode on real hardware.
- Orwell (Quadra 700/900/950) bank layout.
