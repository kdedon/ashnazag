# AMIX platform-interface map

What in the AMIX 2.1 kernel relink kit is Amiga-specific, what the generic objects expect a platform to provide, and a proposed platform interface for Atari and Mac modules.

Sources: the relink kit (`conf/usr/sys`, below `$S`), its Commodore platform source (`$S/amiga`, `$S/master.d`), the AMIX headers, and the AMIX 040/060 port's docs. Addresses are `.text` offsets inside each relocatable `exp` object (object-relative, not `stand/unix` addresses). Method: relocation cross-reference and immediate-operand scans with [`tools/amixplat.py`](../tools/amixplat.py); disassembly with `tools/elfdis.py` and [`tools/elfrange.py`](../tools/elfrange.py) (decodes the 68851/68030 PMMU instructions that capstone gets wrong).

## Summary

- **The generic objects do not touch Amiga hardware.** No custom-chip, CIA, RTC, Gary/Ramsey, DMAC, autoconfig or Kickstart address appears as an operand in any generic `exp` (`ml os vm exec disp io netinet rpc ktli klm des fs`). The few range hits from the immediate scan are bit masks, netmasks or data misread as code. All register-level hardware access is in the Commodore source under `$S/amiga`.
- **The core imports only 24 symbols from the Amiga layer** (table below), plus configuration tables from `master.d/kernel.c` and `amiga/config/unix.c`. This link-level boundary is the de facto platform interface.
- **Board assumptions baked into the binaries** are not register accesses but policy and layout:
  1. the kernel address map built by `pstart` (`ml/exp`);
  2. a single contiguous RAM region (`mlsetup`, `kvm_init`, `page_init`);
  3. the interrupt-priority policy (`splhi` = IPL 4, inlined everywhere);
  4. `HZ` = 60, compiled in;
  5. the console device identity (`oncons`).
- All exception entry and exit code (vector table, `nullvect`, `intret`, `utraps`/`ktraps`, the signal-frame return path, and the per-trap CACR switching) is Commodore source (`amiga/ml/vec.s`, `ttrap.s`), not binary.

## 1. Link structure

`$S/Makefile` links, in order: `ml/exp amiga/exp io/exp os/exp fs/exp master.d/exp vm/exp exec/exp disp/exp ktli/exp klm/exp rpc/exp des/exp netinet/exp local/exp amiga/config/unix.o`, with `-r` (MACHINE=`reloc`). The Amiga bootstrap relocates the ELF kernel to a RAM address it chooses at boot. Fully linked alternatives use `amiga/config/{A2500,A3000,A3008,A3016}.mapfile` (load at 0x00200000, 0x07C00000, 0x07800000 or 0x07000000).

`amiga/exp` = `ml/{ttrap,vec,syms}.o`, `kernel/{servant,support}.o`, `console/*`, `alien/*` (SCSI), `driver/*`, `floppy/*`.

## 2. Symbols the generic objects import from the Amiga layer

Generated with `amixplat.py $S xref`: undefined symbols of the generic objects that are defined in `amiga/*` objects, with the referencing generic functions.

| Symbol | Defined in | Used by (object:function) | Purpose |
|---|---|---|---|
| `M68Kvec` | amiga/ml/vec.o | ml:`stext` (+0x0e) | vector table; `stext` loads it into VBR |
| `config` | amiga/kernel/support.o | ml:`stext` (+0x24) | first platform hook, `config(d0,d1)` with the loader's registers; MMU off, before BSS is zeroed |
| `tc_on` | amiga/ml/ttrap.o | ml:`pstart` (+0xfde) | MMU TC value 0x82B02D60 (2 KB pages, TIA/B/C/D = 2/13/6/11, SRE) |
| `u`, `syssegs`, `kvsegmap`, `kvsegu` | amiga/ml/syms.o (absolute) | `u`: about 1,070 relocations in about 500 functions of all objects; others: os:`mlsetup`, `sysseginit`, `kvm_init`, vm:`kseg`, `unkseg`, `sptfree`, `hat_pt2ptdat` | fixed kernel virtual layout: 0x40000000, 0x40040000, 0x40440000, 0x48440000 |
| `MAINSTORE`, `VSIZOFMEM` | amiga/kernel/support.o | os:`mlsetup`, `kvm_init`, `xclosef` | base and size of the one RAM region the VM manages |
| `chipmem` | support.o | os:`main` (+0x1da40) | printed only ("Graphics (chip) memory = %d") |
| `kernel_load_address` | support.o | os:`sysm68k` (+0x1a4c8) | returned to user by `sysm68k` command 99 (0x63) |
| `putchar` | support.o | os:`printn`, `xprintf` | polled console output for `printf`/`cmn_err`/`panic` |
| `callrom` | support.o | os:`call_demon` (+0x1a5b4) | "enter ROM monitor" on panic; Amiga version waits for Return |
| `sysdump` | support.o (empty stub) | os:`xpanic` (cmn_err.c, +0x284a) | crash-dump hook |
| `haltsys` | amiga/kernel/servant.o | os:`mdboot` (+0x1a574) | halt (0) or reboot (1) |
| `rtnfirm` | servant.o | ml:`pstart`, os:`mdboot`, `xpanic`, `xcmn_err` | reboot ("return to firmware") |
| `hw_clkstart` | amiga/driver/cl.o | io:`clkstart` (+0x2770) | start the periodic tick source |
| `clkreld` | cl.o | io:`clock_int` (on panic), os:`xpanic` | stop the tick source |
| `coinfo` | amiga/console/c0.o | os:`oncons` (+0x7c46) | console streamtab; `oncons` tests `cdevsw[major].d_str == &coinfo` |
| `scropen` | amiga/console/scrdev.o | os:`oncons` (+0x7c64) | graphics screen device open; `oncons` also treats this major as the console |

Configuration symbols the core reads (source available, per-platform content):

| Symbol(s) | Where | Used by | Platform relevance |
|---|---|---|---|
| `io_init[]`, `io_start[]` | master.d/kernel.c | os:`main` | driver init lists (Amiga: `parinit`) |
| `io_halt[]` | kernel.c | os:`dhalt` (from `mdboot`) | driver shutdown list |
| `io_poll[]` | kernel.c | os:`clock` (every tick) | per-tick polling (Amiga: `qlintr`, `slpoll`) |
| `init_tbl[]` | kernel.c | os:`main` | subsystem init (`fpuinit`, `cinit`, `binit`, …) |
| `cdevsw`, `bdevsw` (+ `shadow*`, counts) | kernel.c | specfs, os, io | driver switch; console is major 0 |
| `utsname` (`MACH` "Amiga"), `hw_provider`, `architecture` | kernel.c, kernel.h | `uname`, `systeminfo` | identity strings |
| `timer_resolution` (= `HZ`) | master.d/hrt.c | io:`clkstart`, hrt code | must equal the tick rate |
| `dmainit`, `mtcrchk`, `hdeexit` | master.d/stubs.c (empty) | os:`startup`, `xpanic`, `exit` | unused hooks; `dmainit` is a free per-platform init point |
| `rootdev`, `dumpdev`, `swapfile` | amiga/config/unix.c | s5/ufs mountroot, `swapconf` | boot devices |
| `end`, `edata`, `etext` | linker | `pstart`, `mlsetup`, `getcaller` | kernel image bounds |

Amiga-only tables with no generic user: `int2_tbl[]` (level-2 handler chain, used by `ttrap.s:p2int`), `congetc`/`conputc` (used by `support.c`), `vbinttab`/`addvbint`/`remvbint` (level-3 vertical-blank servers, `ttrap.s`).

Existing generic hook variables (common, default 0): `trap_hook` (called first in `k_trap` +0x1e2e2 and in `u_trap`), `panic_hook`, `clock_hook`. Possible attachment points for a platform or the A/UX-compat layer.

## 3. What the platform code calls in the core

From `amixplat.py $S rev` (platform layer → generic definitions), limited to the machine interface:

| Caller (source) | Generic symbol | Contract |
|---|---|---|
| `ttrap.s:ktraps` | `k_trap(usp)` (os, trap.c, +0x1e2b0) | kernel-mode exception; frame = USP, D0–D7/A0–A6, exception frame (`pcb_t` in `sys/pcb.h`); nonzero return → rte through a 4-word frame |
| `ttrap.s:utraps` | `u_trap(usp)` (+0x1e646) | user-mode exception or syscall (TRAP #0 goes through `nullvect` too) |
| `ttrap.s:trap_ret3` | `s_trap`, `curproc`, class `cl_trapret` | return-to-user path; hard-coded offsets `p_clproc` 0xe8, `p_clfuncs` 0xec, `cl_trapret` 0x4c |
| `ttrap.s` | `runrun`, `qrunflag`, `queueflag`, `queuerun`, `sigflag`, `sigsave`, `framesz` | preemption, STREAMS service, signal-frame restore |
| `ttrap.s` | `cacr` (0x3919), `sup_cacr` (0x1019) (os data, sysm68k.c) | CACR loaded on every exception entry (`sup_cacr`) and user return (`cacr`) |
| `driver/acia.c:aciaaintr` | `clock_int(pcb_t *)` (io, hrtimers.c, +0xba4), `addupc_clk(pcb_t *)` (os) | per-tick entry; `addupc_clk` when `clock_int` returns nonzero (profiling) |
| `support.c:config` | `bcopy`, `panic` | callable before BSS zeroing |
| drivers | DDI/STREAMS set (`vtop`, `lbolt`, `sleep`/`wakeup`, `timeout`, `uiomove`, …) | ordinary driver interface |

## 4. Start-up path and its Amiga dependencies

1. **Loader** (`amiga/boot/boot2.c`, `copyit.s`; the AMIX port uses `unix_boot040`): relocates the ELF kernel into the largest non-chip RAM chunk, turns the MMU off (030 `pmove`), jumps to the entry with supervisor mode, IPL 7, `d0` = boot method (1 = A2620 ROM, 2 = EXEC0, 3 = EXEC1), `d1` = pointer to `struct bootinfo` (`amiga/boot/bootinfo.h`: 16 `ConfigDev`, 16 `MemHeader`, keyboard state, text/data layout).
2. **`stext`** (ml/exp +0x0): IPL 7; saves the loader's VBR in `bugvbr`; VBR = `M68Kvec`; stack = `pstack` (8 KB in ml data); **`config(d0,d1)`**; `pstart()`; stores the returned u-area descriptor via `ublksde`; `pflusha`; SP = `u`+0x1FC0; `main()`; builds an RTE frame to `icode` (user PC in d0).
3. **`config`** (support.c, Amiga): saves `boot_arg0/1`, `kernel_load_address`; sets the A3000 Gary bit (`0xDE0002 |= 0x80`); converts the boot method's data into `bootinfo`; enables INTENA master (`0xDFF09A`); sizes chip RAM by alias probing (0x3FFFC…0x1FFFFC) into `chipmem`; picks **`MAINSTORE`/`VSIZOFMEM`** by growing the `bootinfo.memory[]` region that contains `end` (with a special case above 0x07000000 for A3000 motherboard RAM); `figuredisplaytype()` (PAL/NTSC and chipset via CIA-A timer against VBL).
4. **`pstart`** (ml/exp +0xd44, 724 bytes, pstart.c): crash re-entry check (`crashsw`: second entry → `cdump`, `rtnfirm`); builds the MMU tables at page-rounded `end`: root (long format, TIA = 2 bits):

   | Root slot | VA range | Mapping |
   |---|---|---|
   | 0 | 0x00000000–0x3FFFFFFF | early-termination page → PA 0, **cacheable**, supervisor |
   | 1 | 0x40000000–0x49FFFFFF (limit 0x4FF) | table `st_top1`: kernel virtual (u-area, `syssegs`, `kvsegmap`, `kvsegu`) |
   | 2 | 0x80000000–0xBFFFFFFF | early-termination page → PA 0x80000000, **cache-inhibited** |
   | 3 | 0xC0000000–0xFFFFFFFF | early-termination page → PA 0xC0000000, cache-inhibited |

   It allocates the u-area page table (`kuptr`, 4 × 2 KB) and its descriptor `ublksde`, then `pmove (a0),srp` (+0xfd6), `pflusha`, `pmove tc_on,tc` (+0xfde); `vstart`; `mlsetup(firstfree)`; returns `svirtophys(proc_sched…)`.
5. **`mlsetup`** (os startup.c +0xcc90): zeroes BSS; `physmem = min(VSIZOFMEM/2K, v.v_maxpmem)`; `maxclick = MAINSTORE/2K + physmem`; **zeroes all RAM from `firstfree` to `maxclick`**; `sysseginit`; `sptmap`; **`kvm_init`** (+0xcdf6, local): `kpseg` = VA 0…`MAINSTORE+VSIZOFMEM` (the identity window), `kvseg` = `syssegs` + 4 MB, sizes `segkmap`/`segu`, `page_init(pp, maxclick−first, first)` and `memialloc(first, maxclick)`, both over **one contiguous physical range**; then `dispinit`, `hrtinit`, `itinit`, `p0init`, `pid_init`.
6. **`main`** (os +0x1d974): `startup` (→ `dmainit` stub), `clkstart` (→ `hw_clkstart`), `io_init[]`, `init_tbl[]`, `io_start[]`, `setupclock`, `vfs_mountroot`, banner with `chipmem`, `physmem`, `freemem`.

Boot protocol details specific to the Amiga: the `d0`/`d1` meaning, `struct bootinfo`, chip-RAM sizing, Zorro `ConfigDev` table, relocation by the loader, PAL/NTSC detection. The generic part only needs "entry at `stext` with MMU off and two register arguments".

## 5. Address-space and memory assumptions

- **Identity mapping for the kernel image.** The kernel runs at its physical load address through root slot 0; `end`, page tables and `MAINSTORE` are used as both physical and virtual addresses. The kernel must be loaded at PA < 0x40000000.
- **Kernel virtual window 0x40000000–0x49FFFFFF** (constants in `syms.s`, table limit in `pstart`, pointer sanity check in `xclosef`, os +0x40e8: accepts `[MAINSTORE, MAINSTORE+VSIZOFMEM)` or 0x40000000–0x48FFFFFF). Physical devices in 0x40000000–0x7FFFFFFF are not reachable through the identity map. On the Amiga this hides Zorro III boards allocated from 0x40000000 (the AMIX port's `devkvmap040.s` and `docs/AMIGA-PHYSICAL-MEMORY-MAP.md` deal with it). **On Macs this covers the ROM (0x40800000) and the I/O space at 0x50000000**, which will need explicit kernel mappings.
- **I/O caching.** 0–1 GB is mapped cacheable; 2–4 GB cache-inhibited. On the 030 this is harmless for the kernel because `sup_cacr` (0x1019) runs the supervisor with the **data cache disabled**; `cacr` (0x3919) enables it for user mode and clears both caches on every return to user. (The reason is probably DMA coherency without bus snooping; not confirmed.) The 040/060 port replaces this with DTT0/DTT1 (0–1 GB non-cacheable, 2–4 GB supervisor-only).
- **One RAM region.** `page_init` records a single `pages_base…pages_end` range; the VM cannot use memory outside `[MAINSTORE, MAINSTORE+VSIZOFMEM)`. Amiga chip RAM is used only by drivers (console, audio). This matters for the Atari (ST-RAM plus TT/Fast RAM at 0x01000000) and for Macs with discontiguous banks.
- **Page size 2 KB**, TC layout, descriptor format: CPU-specific; the 040/060 port has already moved to 4 KB.

## 6. Interrupts, priority levels, clock

- **Vector table** (`amiga/ml/vec.s`): slots 0x64–0x78 (autovectors 1–6) → `p1int`…`p6int`; every other vector, including bus error, TRAP #0 syscalls, spurious and level-7 NMI, → `nullvect` → `k_trap`/`u_trap`, which dispatch on the frame's vector number (`k_trap` jump table for vectors 2–24).
- **Amiga interrupt decode** (`ttrap.s`): each `pNint` saves D0–D7/A0–A6, loads `sup_cacr`, reads INTREQR (`0xDFF01E`), clears the bit in INTREQ (`0xDFF09C`), calls the handler with a pointer to a `pcb_t` (a fake USP slot is pushed first), and leaves through `intret`. L1 TBE → `sltint`; L2 PORTS → `int2_tbl[]` (`aciaaintr`: keyboard, parallel, **clock**; `jbintr`, `a2091intr`, `a3091intr`, `aenintr`); L3 VERTB → `vbinttab`; L4 audio → `audiointr`; L5 RBF → `slrint` (plain `rte`, bypasses `intret`); L6 EXTER → `aciabintr`.
- **IPL policy is compiled into every generic object**: `sys/inline.h` defines `spl5`, `spl6`, `spl7` as `_spl4` (`FAST_INTERRUPTS`), so `splhi`, `spltty`, `splimp`, `splvm`, `splstr` all mean IPL 4. The objects contain only `move.w #$2x00,sr` with x = 0, 1 or 4 (os 158 × IPL 4, io 93, vm 29, netinet 25, disp 18, fs 11, rpc 4, ktli 3), plus IPL 7 in `ml:stext` and `ml:resume`. Consequence: **any interrupt at IPL 5–6 is never masked by the core**, so its handler must not touch kernel data (the Amiga uses L5 for a fast serial-receive path and L6 for CIA-B).
- **Clock**: `HZ` = 60 (`sys/param.h`, `CLOCK == 1`), compiled into the objects (`clkstart` divides `timer_resolution` by 0x3C). Source on the Amiga: CIA-A timer A at level 2 (`cl.c`, E-clock 709379/715909 Hz / 60). `clock_int` → `clock()` → `timepoke` → `timein` runs callouts **at clock-interrupt level**, and `io_poll[]` runs every tick, so the tick interrupt must be at IPL ≤ 4.
- **Time of day**: no RTC access in the kernel (`clkset` only stores `hrestime`); the battery clock at 0xDC0000 is not used by the kernel (presumably set from user space; not checked).

## 7. CPU-specific versus board-specific

| Item | Location | CPU | Board | Notes |
|---|---|---|---|---|
| MMU enable, descriptor format, TC, 2 KB pages | ml:`pstart`, vm hat (`hat_*`, `vatopte`, `svirtophys`), `tc_on` | ✔ | address map only | port: `pstart040.s`, `hat040.s`, 4 KB pages |
| `pmove crp`/`pflusha` | disp:`swtch` +0x378, vm +0xde9e/+0xf6c2/+0xfa4a/+0xfea8 (`flushmmu`), ml:`resume` +0xb2, `stext` +0x38 | ✔ | | port |
| `ptestr` + `pmove mmusr` | ml:`ptest`/`ptest0` (+0x3a8/+0x3c0), used by os trap.c | ✔ | | 68851: same encoding, PSR |
| Exception frames, bus-error decoding, signal frames | os trap.c (`k_trap`, `u_trap`, `get_fault`, `userspace`, `stackfault`), machdep.c (`sendsig`, `savecontext`, `restorecontext`, `setregs`), `framesz` | ✔ | | port |
| FPU detection and save/restore | ml `chk_fpu`/`fpu_save`/`fpu_restore`, io:`fpuinit` | ✔ | | port adds FPSP/060SP |
| CACR values and switching | os `cacr`/`sup_cacr`, `cache_on`/`cache_off`; `ttrap.s` | ✔ | (coherency policy) | |
| `moves` user access (SFC/DFC) | ml `copyin`/`copyout`/`fubyte`… | generic 68k | | |
| MMU/cache off before reboot | servant.s `haltsys` | ✔ | ✔ | |
| Vector table content, autovector decode | vec.s, ttrap.s `p1int`…`p6int` | | ✔ | |
| Trap entry/exit (`nullvect`, `intret`, `utraps`, `stkclear`, `stkrestore`) | ttrap.s | ✔ (frame formats) | | common code in platform source; move to common |
| Kernel VA constants `u`, `syssegs`, … | syms.s | layout | | keep common |
| Boot protocol, memory discovery, chip RAM | support.c `config` | | ✔ | |
| Tick source | cl.c, acia.c | | ✔ | |
| Console | support.c `putchar`/`getchar`, console/* | | ✔ | |
| Halt/reboot | servant.s | ✔ | ✔ | Gary 0xDE0002, CIA-B 0xBFEE01, A2620 ROM 0xF80028 |
| IPL policy (splhi = 4), HZ = 60 | inlined in all objects | | ✔ | fixed unless objects are patched |
| Identity/CI address map | ml:`pstart` | | ✔ | |
| Single RAM region | os `mlsetup`/`kvm_init`, vm `page_init` | | ✔ | |

## 8. Proposed platform interface

Names are the existing symbols where the core already calls them, so a platform module can be linked without patching the generic objects. New names (`plat_*`) are for things currently hard-coded.

### 8.1 Early boot and memory

| Symbol | Contract |
|---|---|
| `config(arg0, arg1)` | Called from `stext` with the loader's `d0`/`d1`, MMU off, IPL 7, stack `pstack`, before BSS is zeroed (use initialised data only). Parse boot information; set `MAINSTORE`, `VSIZOFMEM`, `kernel_load_address`, platform RAM for devices (e.g. `chipmem`); quiesce all interrupt sources. |
| `MAINSTORE`, `VSIZOFMEM` | Physical base and size of the RAM region containing the kernel; must lie below 0x40000000 and be identity-mappable. |
| `chipmem` | Size of device-only RAM (Amiga chip RAM, Atari ST-RAM; 0 on Macs). Printed by `main`. |
| `kernel_load_address` | Physical load address (`sysm68k` 99). |
| `plat_iomap[]` (new) | Physical ranges the new `pstart` must map, each with a cache mode; needed because `pstart` must be replaced anyway (CPU) and the Mac I/O space falls in the kernel window. |
| `plat_memsegs[]` (new, later) | Extra RAM ranges, if `page_init`/`kvm_init` are overridden to handle more than one region. |

### 8.2 Traps and interrupts

| Symbol | Contract |
|---|---|
| `M68Kvec` | 256-entry vector table: all exceptions → common `nullvect`; autovectors (and the platform's vectored interrupts) → platform handlers. |
| `p1int` … `p6int` (+ level 7) | Save registers in `pcb_t` order (push fake USP), load `sup_cacr` equivalent, identify and acknowledge the source, call the driver handler with a `pcb_t *`, exit through `intret`. Sources above IPL 4 may only touch data the core never locks (or must defer to a lower level). |
| `intret`, `nullvect`, `ktraps`, `utraps` | Common code (today in `ttrap.s`); not per platform. |

### 8.3 Clock

| Symbol | Contract |
|---|---|
| `hw_clkstart()` | Start a periodic interrupt at exactly `HZ` (60) at IPL ≤ 4; called at IPL 4 from `clkstart`. |
| tick handler | On each tick call `clock_int(pcbp)` and, if it returns nonzero, `addupc_clk(pcbp)`. |
| `clkreld()` | Stop the tick (panic path). |
| `timer_resolution` | = `HZ`. |

### 8.4 Console, monitor, halt

| Symbol | Contract |
|---|---|
| `putchar(c)` | Polled, synchronous output; any IPL, including panic and before interrupts are enabled. |
| `getchar()` | Polled input (used by `callrom`). |
| `callrom()` | Debugger/monitor entry on panic; returning continues. |
| `coinfo`, `scropen` | The console streamtab and the graphics-screen open routine that `oncons` compares against `cdevsw[]` (names fixed by the binary; alias to the platform's console drivers). |
| `haltsys(how)` | 0: print halt message and stop; 1: reboot. Interrupts off, MMU and caches off as the CPU requires. |
| `rtnfirm()` | Reboot (`haltsys(1)`). |
| `sysdump()` | Crash-dump hook (may be empty). |

### 8.5 Configuration (per-platform `master.d/kernel.c`)

`io_init[]`, `io_start[]`, `io_halt[]`, `io_poll[]`, `init_tbl[]`, `cdevsw[]`/`bdevsw[]` (console at major 0), `utsname.machine`, `hw_provider`, `rootdev`/`dumpdev`/`swapfile`, optional `dmainit()`.

### 8.6 Services for platform drivers (not used by the core)

Generalise the Amiga-only helpers: `autocon()` (Zorro board lookup) → a bus-probe interface (Zorro, VME, NuBus); `int2_tbl[]` and `vbinttab`/`addvbint` → per-level shared handler chains; `delayus()` (Amiga beam counter) → calibrated delay; a DMA-safe allocator (Amiga chip RAM, Atari ST-RAM and 24-bit DMA).

## 9. Amiga-specific code inside binary objects

What has to be replaced or overridden (by weakening the stock symbol and linking a new one, as the AMIX port does) for Atari and Mac. Everything in `amiga/*` is source and is simply replaced per platform.

| Binary site | Amiga assumption | Atari | Mac |
|---|---|---|---|
| ml:`pstart` (+0xd44) | root slots: 0–1 GB identity cacheable, 2–4 GB identity CI; kernel window 1–1.16 GB | TT-RAM at 0x01000000 fits slot 0; I/O at 0xFFFF8000/0x00FF8000 and VME at 0xFE000000 need CI (0x00FF8000 is in the cacheable slot; harmless on 030 with `sup_cacr`, needs CI on 040/060) | ROM at 0x40800000 and I/O at 0x50000000 fall in the kernel window: map them explicitly in kernel VA (or move the kernel window, which touches `syms.s` constants and every `u` reference); NuBus 0x6/0x7xxxxxxx super-slot space is also unmapped |
| ml:`pstart` (whole) | 030 MMU | replaced anyway per CPU (port: `pstart040.s`) | same |
| ml:`stext` | calls `config(d0,d1)`; VBR = `M68Kvec` | fine; Atari loader defines d0/d1 | fine; Mac booter defines d0/d1 |
| ml:`pstart` crash re-entry (`crashsw`, `cdump`) | memory survives reset and the kernel is re-entered | check reset behaviour | check reset behaviour |
| os:`mlsetup`/`kvm_init`, vm:`page_init`, os:`memialloc` | one contiguous RAM region | ST-RAM + TT/Fast RAM: either VM uses only TT/Fast RAM (ST-RAM like `chipmem`) or override these for a second region | banks may be discontiguous (e.g. 0x0 and 0x04000000 on some models): override, or cover the span and withdraw the hole pages (cost about 60 bytes per 2 KB page; uncertain whether `page_deladd` can do this) |
| os:`xclosef` (+0x40cc…) | pointer check `[MAINSTORE, +VSIZOFMEM)` ∪ 0x40000000–0x48FFFFFF | ok if kernel data stays there | same; must be revisited if kernel data moves |
| all objects: `spl*` inlined as IPL 4 | Amiga-chosen IPL policy | **MFP (timers, keyboard ACIA) and SCC are at IPL 6/5 (public hardware docs; verify for TT/Falcon)**: not masked by `splhi`. The tick must come from IPL ≤ 4 (e.g. VBL autovector 4, not exactly 60 Hz) or an IPL-6 stub must defer to a lower level; alternatively patch the `move.w #$2400,sr` sites (about 340) to IPL 6 | VIA1 (IPL 1), VIA2/slots (IPL 2), SCC (IPL 4) fit; NMI at 7 unaffected |
| all objects: `HZ` = 60 | compiled constant | program an MFP timer to 60 Hz, or deliver 60 Hz from a faster source | VIA1 timer 1 at 60 Hz (VBL is 60.15 Hz) |
| os:`clock` callouts at tick level, `io_poll[]` | tick at IPL 2 | see IPL row | fine |
| os:`oncons` | console = `coinfo` or `scropen` majors | provide aliases | provide aliases |
| os:`main` | prints `chipmem` | ST-RAM size, or 0 | 0 |
| os:`sysm68k` 99 | returns load address | keep | keep |
| master.d/kernel.c (source) | `MACH` "Amiga", `int2_tbl`, Amiga majors | per platform | per platform |

Nothing else in `os`, `vm`, `io`, `disp`, `exec`, `fs`, `netinet`, `rpc`, `ktli`, `klm`, `des` is board-specific according to the relocation and immediate scans.

## 10. Uncertainties and open checks

- Why the supervisor data cache is off (`sup_cacr` 0x1019 lacks ED; `cacr` 0x3919 also clears both caches on every user return): coherency with chip-RAM/Zorro DMA is assumed, not confirmed.
- Whether the core ever reads the battery clock or relies on user space to set the time (`clkset` only stores `hrestime`).
- `sysm68k` 99 consumer (probably `ps`/crash tools reading `/dev/kmem` of a relocated kernel).
- `page_deladd`/`page_delmem`/`page_addmem` semantics (possible hole removal for multi-bank RAM) not analysed.
- Atari and Mac interrupt levels and address maps above are from general hardware knowledge; verify against public hardware documentation before design.
- The immediate scan catches operands only. Hardware addresses built at run time from table data would be missed; none were found, and all generic→platform references go through the symbols in section 2.

## Tools

- `tools/amixplat.py <sys> xref` — generic → platform/master.d imports with referencing functions; `rev` — platform → generic; `hw [g|p|m]` — immediate operands in Amiga hardware ranges; `files <obj>` — source-file boundaries recovered from `STT_FILE` symbols.
- `tools/elfrange.py <elf> <start> <end> [section]` — disassembly of an address range with relocations and hand-decoded PMMU instructions.
