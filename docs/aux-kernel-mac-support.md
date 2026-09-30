# A/UX kernel support for the Mac environment

Static analysis of A/UX 3.1 `/unix` (COFF, unstripped). Addresses are kernel virtual addresses. The A/UX headers `/usr/include/sys/uinter.h`, `sys/proc.h`, `compat.h`, `mac/*.h` and `apple/slots.h` are on the image and match the code. *(uncertain)* marks inferences not fully traced.

## 1. What makes a process a "Mac process"

### Proc flags (`sys/proc.h`)

| Flag | Value | Meaning |
|---|---|---|
| `SMAC` | 0x02000000 | "process belongs to Mac virtual machine" |
| `SCHKLVL` | 0x04000000 | don't run a Mac process whose interrupt level is below the current one |
| `SMAC24` | 0x00000800 | 24-bit Mac/Toolbox app |
| `SROOT32` | 0x00000040 | 32-bit Mac MMU root installed |
| `SROOT24` | 0x00000080 | 24-bit Mac MMU root installed |

- `SMAC` is set by `UI_createlayer` (0x10084b98) and `UI_attach_layer` (0x10084f64): `bset #1` on byte 0 of `u.u_procp->p_flag`. It is cleared by `ui_unconnect` (0x1008679e), which `ui_exec` and `ui_exit` call. **Joining a uinter layer (ioctl 21 `UI_CREATELAYER` or 37 `UI_ATTACHLAYER`) is what makes a process a Mac process.**
- `setcompat`/`getcompat` (syscalls 127/128, stored at proc+0x72) are only the BSD/POSIX behaviour flags from `compat.h` (`COMPAT_BSDSIGNALS`, `COMPAT_BSDNBIO`…). They are not Mac identity.
- u-area (0x12fff000) Mac fields: +0x528 saved vector $28 (A-line), +0x55c saved vector $20 (privilege violation), +0x52c layer index (0xff = none), +0x534 saved USP, +0x548 virtual SR (word; cleared by `ui_exec`, along with +0x54a/+0x54e).

### Per-process exception vectors

The kernel vector table sits at kernel VA 0 (VBR 0). On every context switch `resume` (pstart section; labels `fp20`/`fp30`/`fp40`, 0x543ce/0x54466/0x544f2) reloads two vectors from the incoming process's u-area:

```
move.l $55c(a0),$20.w   ; privilege violation
move.l $528(a0),$28.w   ; line-A
```

| | Normal process | Mac process after `UI_SET` |
|---|---|---|
| $28 (A-line) | `lineAFault` 0x10012572 → `trap()` → SIGILL | `lineAVector` 0x1001244e |
| $20 (privilege) | 0x54922 (generic trap entry → SIGILL) | `lpriv` 0x10012710 |

`UI_set` (ioctl 1 `UI_SET`, "set the a-line trap handler", 0x10085712) needs a layer. On first use it allocates and locks low memory (`ui_lowaddr`) and calls `ui_setmemmap(1)`. It then stores both vectors in the u-area and in the live table. `UI_clear` (ioctl 2, 0x10085836) restores `lineAFault`/0x54922 and calls `ui_setmemmap(0)`.

`ui_setmemmap` (0x1008814e) records the physical address of each of the first 3 pages (0x0–0x2fff) of the process's Mac low memory in `ui_memmap[]`. It sets `ui_addr24` from `SMAC24`. The kernel uses these to write Mac low memory from interrupt context.

### A-line path (`lineAVector`, 0x1001244e)

It builds a standard 8-byte 68020 exception frame on the **user** stack with `moves`: SR word (virtual IPL merged in), PC, and format/vector word. In the code, the second long pushed combines the SR word with the high half of the PC. It sets the return PC to user location `$28` (read with `moves.l $28.w`) and masks the virtual SR to its IPL bits (`andi #$700`). The real SR is reduced to user mode plus CCR. Then it returns with `rte`. `utrace` does the same for trace exceptions through user `$24`, unless the process is traced or `$28` is `lineAFault`.

### Privileged-instruction emulation (`lpriv`, 0x10012710 – 0x10012f2c)

Emulated opcodes:
- `ori`/`andi`/`eori #,SR` (0x007C/0x027C/0x0A7C)
- `move <ea>,SR` / `move SR,<ea>` (0x46C0/0x40C0; tables `lpriv_tosrtbl`, `lpriv_frsrtbl`)
- `movec` both directions (0x4E7A/0x4E7B; CACR 0x002, USP 0x800, CAAR 0x802, SFC/DFC and others)
- `rte` with frame formats 0, 2, 3, 9 (`rteloop1/2`)
- `move USP` (0x4E60/0x4E68), `moves` (0x0Exx)
- `fsave`/`frestore` (0xF300/0xF340)
- 040 `cinv`/`cpush` (0xF4xx/0xF5xx → `cpushall`/`cpushpage`/`cpushline`)
- MMU-related stubs (`tomsmmutc`, `tomsmmusr`, `tomsdfc`) *(uncertain: exact semantics)*

The virtual SR is kept in u+0x548. Its IPL is mirrored into the global `ui_SR` (0x11005e48). When a write lowers the IPL, `lpriv_chkpend` calls through `ui_sigpending` if `ui_pending` is set. Anything else goes to `lpriv_sig` (signal).

### Virtual interrupts (signals)

- `ui_open` (0x100882c4) installs two hooks: `ui_callout = ui_update` (0x1008651a) and `ui_sigpending = UI_sigpending` (0x10085252).
- `ui_update` runs from `clock()` and from `swtch`/`uiswtch` once per tick for a process that has a layer. When the current process is a Mac process with virtual IPL 0, it writes the Mac globals `Time` ($20C), `Ticks` ($16A, from `lbolt`) and the alarm bits ($21F/$208/$200) directly into the process's low memory.
- **`UI_TIMER`** (ioctl 35, `UI_timer` 0x1008629a) arms a 1-tick periodic kernel `timeout(ui_catch_timer)`. Each tick `ui_catch_timer` (0x100864b2) does `wakeup` and **`psignal(p, SIGIOT)`** (6). `Patch.067C:attach_timer` installs the handler for signal 6. This is the Mac tick interrupt (VBL / Time Manager). *(uncertain: exact split of VBL vs Time Manager work on the Mac side)*
- I/O completion arrives as `SIGIO` (31; `Ser_sigio`, `snd_sigio`, `adsp_sigio`…). `sleep` uses `SIGALRM`.
- While the virtual IPL is non-zero, signals in `ui_sigmask` are held. `UI_sigpending` releases them per attached process.
- `sendsig` (0x10000000), for `SMAC` processes: every signal except SIGQUIT and SIGALRM increments the global `ui_curlevel`, stores it in proc+0x87, and clears `SCHKLVL`. It saves the virtual SR into the signal frame and clears its trace bits (`andi #$3fff`), then loops on `uiswtch` until that level is current. This gives nested interrupt levels across all processes of the Mac VM.
- Signal return: `_sigcode` calls `sysm68k(2)`, which restores the frame, including the virtual SR.

### Multiple processes per Mac VM

A layer (`ui_layer[]`, 0x6b6 bytes each, `NATTACHES` 16) groups the Mac processes that share one low-memory shm: startmac, CommandShell, Login, and COFF apps launched from the Finder. `UI_SWITCH` (38) and `UI_SLEEP` (39) with `ui_findtask`, `ui_wakeup` and `ui_sleep` hand the CPU between them cooperatively. This backs `AUX_SWITCH` in the A/UX Process Manager.

### `swapmmumode` (syscall 67, 0x1002d69e)

This only acts for `SMAC24` processes; others get the answer "already 32-bit". `swapMMUMode` (0x1002d6de) switches the process MMU root between proc+0x7e (32-bit table) and proc+0x82 (24-bit table), sets `SROOT32`/`SROOT24`, and reloads the MMU (`jsr 0x56106`). 24-bit mode is a second page table per process, presumably mapping the 16 MB space with the top address byte ignored *(uncertain)*. `map_screen` also calls `swapMMUMode`.

### `sysm68k` subcommands used by Mac binaries

Syscall 38/50, 0x1002d7cc; the subcommand is the first argument.

| Sub | Used by | Kernel action |
|---|---|---|
| 2 | `_sigcode` (all) | signal return: copy in frame, restore regs, SR, virtual SR |
| 0x69 (105) | `CacheFlush` (`libmac1_s`, `Patch.067C cBlockMove`) | cache flush `(addr, len, which)` via 0x5abee, only when `cputype == 5` (040); other CPUs get EINVAL (see [aux-syscall-translation.md](aux-syscall-translation.md)). A host should treat the 68060 like the 68040 |
| 0x6a (106) | `Patch.067C vfs_trap_handler` | `tclrFileMgrFlag` |
| 0x6b (107) | `get_cache_info` | returns buffer-cache parameters |
| 0x6c (108) | `clear_cache_info` | `suser()` only; zeroes two counters |
| 0x6d (109) | `getBufCache` | returns buffer-cache geometry (`v` fields) |

## 2. Kernel use of the Mac ROM

The kernel keeps **its own Mac low-memory world at kernel VA 0**, which is also the exception vector table. It runs ROM code in supervisor mode for hardware services.

### `initMacEnvironment` (0x10046148), at boot

1. `initROMArray` (0x10046292) reads the ROM header long at 0x40800016, the **Foreign OS table**. It stores `ROMArray[0..]` = Foreign OS entries 0 (trap-table init), 1 (A-line handler), 2, then `StartSDeclMgr`/secondary init.
   - ROM $067C: entry 3 comes from the table and `romaline` is used.
   - ROM $0178 (Mac II, no table): hardcoded 0x408064ba and 0x40804152.
   - Other ROM versions: table entries 3 and 4.
2. Sets kernel low memory: `ROMBase` ($2AE) = 0x40800000, `JVBLTask` ($D28) = `vblTask`, `JIODone` ($8FC) = `ioDone`, `UTableBase` ($11C) = `unitTable`, `UnitNtryCnt` ($1D2) = 0x4E, `MemTop` ($108) and others.
3. Sets `userAline` = `lineAVector`, calls `ROMArray[0]` (trap-table init), and saves the resulting `$28` (the ROM A-trap dispatcher) as **`kernelAline`**.
4. Fills the Toolbox trap table ($E00, 0x400 entries) with `noSupport` and the OS trap table ($400) from `OSPatch[]`.
5. `doPatches` (0x10046346): OS trap $6E SlotManager → `aSlotManagerPatch` (original kept in `realSlotManager`). Also `aGetResource`, `aTickCount`, `aGetDeviceList`, `aGestalt`, `noSupport1`. Selects `secondaryInit` for ROM $067C by box type.
6. With `$28 = kernelAline`, calls `callSecondaryInit` (slot/video secondary init), then sets `$28 = userAline`.

### ROM code run later, on behalf of the Mac process

Each wrapper temporarily sets vector `$28 = kernelAline` so A-traps inside ROM code go to the ROM dispatcher.

| Kernel entry | Called from | ROM code run |
|---|---|---|
| `callOpen` (0x10041784) | `video_init` | opens NuBus/built-in video drivers (DRVR from declaration ROM) |
| `kallDriver` (0x10041c34) → `callDriver` | `callControl`/`callStatus`, `setDefaultMode` (console) | video driver Control/Status |
| `callControlStatus` (0x10041838) | **uinter ioctls 47/48 `UI_VIDEO_CONTROL`/`UI_VIDEO_STATUS`** (`struct CntrlParam`), `disp_ioctl` | Mac-side video Control/Status forwarded to the kernel-resident driver; +4 = video index (≤14, `video_index[]`), +0x1a csCode, +0x1c csParam; result in +4 (−17 if no device) |
| `slotmanager` (0x100470c2) | `callSlotManager`, `getDevBase`, `getVideoDriver`, `getVPBlock`, `fixVideoAddress` | ROM Slot Manager |
| `sysslotmanager` (**syscall 66**, 0x10046df4) | Mac process `aSlotManager` | copies in a 0x38-byte `SpBlock`, runs `callSlotManager(selector ≤ 0x30)`, copies out, result in u.u_rval |
| `callSlotInt` (0x10049cf4) | `video_intr` | slot interrupt handlers (VBL of video cards) from the declaration ROM |
| `ReadXPRam`, `WriteXPRam`, `ReadDateTime`, `SetDateTime` (0x10049d44…) | kernel PRAM/clock (behind `UI_READPRAM`/`UI_WRITEPRAM`, `/dev/nvram`) | ROM PRAM and clock routines |

So ROM code runs in kernel mode **both at boot and at run time**, for video (open, Control/Status, slot interrupts), the Slot Manager, and PRAM/clock. The Mac process reaches it through uinter 28/29/47/48 and syscall 66. Everything else (Toolbox, OS traps) runs in the Mac process in user mode, from the shm copy of the ROM.

### The `$28` restore in `kallDriver` / `callOpen` (latent bug)

Facts from the code:

- `userAline` (0x1202e004) is written once, in `initMacEnvironment` (`move.l #lineAVector,userAline` at 0x1004617e), and never changes. So `userAline` is always `lineAVector`.
- `kernelAline` (0x1202e000) is the ROM A-trap dispatcher, captured after `ROMArray[0]` runs at boot.
- The per-process value lives at u+0x528. `setregs` (exec) and `main` (proc 0) set it to `lineAFault`; `UI_SET` sets it to `lineAVector`; `UI_CLEAR` puts back `lineAFault`. The CPU's vector `$28` (kernel VA 0) is reloaded from u+0x528 only in `resume` (`fp20`/`fp30`/`fp40`, the three CPU variants), i.e. on a context switch.
- Every other kernel→ROM helper **saves and restores** `$28`: `slotmanager` (to a local), `callSlotInt`, `ReadXPRam`, `WriteXPRam`, `ReadDateTime`, `SetDateTime` (push/pop). `initMacEnvironment` sets `userAline` explicitly at the end of boot.
- `kallDriver` (0x10041cc0 / 0x10041d20) and `callOpen` (0x100417fc / 0x10041826) set `$28 = kernelAline`, call `callDriver`, then set **`$28 = userAline` unconditionally**. No save, no `spl`.

Who reaches them:

| Path | Current process | Effect |
|---|---|---|
| `ui_ioctl` #47/#48 → `callControlStatus` → `callControl`/`callStatus` → `kallDriver` | the Mac process | harmless: its own vector is `lineAVector` too |
| `disp_ioctl` → `setDefaultMode` or `callControlStatus` (console display ioctls) | **any process**, e.g. `screenrestore` run by `mac32` after `startmac` exits | leaves `lineAVector` installed for a non-Mac process |
| `dispinit`, `disp_onebit`, `video_sbitmap` → `setDefaultMode` | whoever is current (boot, console mode switches) | same |
| `video_init` → `callOpen` | proc 0 at boot | harmless in practice (no user code runs before the next `resume`) |

The reverse case can't happen: nothing in these helpers installs `lineAFault`, so a Mac process never loses `lineAVector` this way.

Consequence for a non-Mac process, until its next context switch: the kernel treats it as a Mac process, because several paths test `$28 == lineAFault` to mean "not Mac":
- an A-line opcode is reflected through the "vector" read from the process's own user address `$28` (inside its text; `/bin/sh` text starts at 0xa8) instead of raising SIGILL;
- `utrace`: trace exceptions (ptrace single-step) are reflected to user `$24` instead of SIGTRAP;
- `userhandler` (via `fault2`): faults are reflected Mac-style through user low-memory vectors instead of posting a signal;
- `ffs_sig`: a pending SIGQUIT is suppressed if the byte at user `$c2c` has bit 7 set (the MacsBug check).

**Real bug, low impact.** It needs a non-Mac process to make a console video call and then execute an A-line opcode, take a fault, be single-stepped, or get SIGQUIT before it's next switched out. The fix is to save and restore `$28` like the other helpers; a host reimplementation should not copy this behaviour.

## 3. Implications for another host

- **Mac identity**: a per-process "Mac" state entered via uinter `UI_CREATELAYER`/`UI_ATTACHLAYER` + `UI_SET`, left on exec/exit.
- **CPU**: A-line reflection to user `$28` with a virtual-SR frame; privileged-op emulation for the list above; trace reflection to `$24`.
- **Interrupts**: a 1-tick SIGIOT (`UI_TIMER`), SIGIO for I/O, signals held while virtual IPL > 0, nested levels via `ui_curlevel`, and `Ticks`/`Time` written into low memory each tick.
- **Scheduling**: cooperative hand-off among all processes of a layer (`UI_SWITCH`/`UI_SLEEP`).
- **24-bit mode**: second address map per process (`swapmmumode`); only needed for 24-bit sessions (`startmac24`).
- **ROM services in the kernel**: video driver open/Control/Status, slot interrupts, Slot Manager, PRAM/clock. A host without Mac hardware must emulate these (for example a virtual video driver answering `UI_VIDEO_CONTROL`/`STATUS` and `sysslotmanager`), rather than run ROM code in its kernel.
