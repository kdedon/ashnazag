# A/UX Mac process: interrupts, signals and Mac→Unix gateways

Analysis of `/mac/lib/Patches/Patch.067C` (COFF, loaded into the Mac process with text at 0x4000, bss ending about 0x88c00, i.e. inside Mac low RAM), `/shlib/libmac1_s` and `/unix`. Addresses are Patch.067C link addresses unless marked kernel. Signal numbers are A/UX's (`/usr/include/sys/signal.h`).

## 1. Interrupt model

There are no hardware interrupts in the Mac process. Every "interrupt" is a Unix signal, handled on a 64 KB alternate signal stack (`sigstack`; allocated with `NewPtr` in `attach_timer`, `malloc` in `install_handler`). Handlers are installed with `sigvec(..., SV_ONSTACK)`.

### Signals used

| Signal | Installed by | Handler | Mac meaning |
|---|---|---|---|
| 6 SIGIOT | `attach_timer` 0x10618 (`sigvec(6, {alarmCatcher, 0, SV_ONSTACK})`, then `sigsetmask(0)`) | `alarmCatcher` 0x10588 | **Tick.** Sent by the kernel every clock tick. Runs `doVBLTasks` if `VBLQueue` (0x160) is non-empty, `doTimeTasks` if the Time Manager queue (`TimeVars` 0xb30) is non-empty, then `doDTasks` (Deferred Tasks). |
| 31 SIGIO | `register_signal(31, …)` from serial (`queueasync`, `Ser_sigio`), vfs async I/O (`attach_vfs_aio_handler`), TCP/UDP/ICMP, AppleTalk (DDP, ATP, ADSP), Sound Manager (`cSndNewChannel`), `_AUXDispatch` sel 23 | `signal_handler` 0xe7dc | I/O completion / data ready for asynchronous Device Manager and network calls. The kernel sound driver also sends SIGIO to `snd_proc` from `ChannelModifier` 0x1007f6f4 after queuing a sound callback. |
| 30 SIGURG | `register_signal(30, …)` from `tcp_create`, `adspInit`, `adspCLInit` | `signal_handler` | TCP urgent data, ADSP attention |
| 7 SIGEMT | `register_signal(7, fillSustainBuf, 0)` from `cSendToKernel` 0x33b62 (sound) | `signal_handler` → `fillSustainBuf` 0x33346 | **sampled-sound refill**. Sent only by the kernel sound driver's `sampMain` 0x1007cc50 (`psignal(snd_proc, 7)` at 0x1007cfcc and 0x1007d20c) when playback passes a computed point in the current sample buffer and `curSndHdr+4` holds a real buffer pointer (> 0x10000). `snd_proc` is the process that opened `/dev/snd` (`sndopen`). `fillSustainBuf` refills the sustain buffer and unregisters itself when finished. The exact trigger arithmetic is not decoded. |
| 16 SIGUSR1 | `register_signal(16, …)` from `sinOpen` | `signal_handler` | sound input |
| 18 SIGCLD | `InitReaper` 0x8bd4: `signal(18, reapchild)` | `reapchild` 0x8be8 | child Unix process exited (CommandShell, `fork_exec`) |
| 3 SIGQUIT | `start`, `InitMacsBug`: `signal(3, …)` | MacsBug entry | debugger break |
| 13 SIGPIPE | `tcp_create`: `sigvec(13, …)` | — | probably ignore for sockets |
| 14 SIGALRM | libc `sleep`, `syslog` only | — | not a Mac interrupt |

`register_signal(sig, routine, mask)` 0xe930 / `unregister_signal` 0xe95e keep a per-signal list at 0x5af4c (`[sig*4]`: next, routine, pid, mask) through `add_routine` / `rmv_routine`, then (re)install `signal_handler` with `sv_mask` = OR of the registered masks (`install_handler` 0xe872).

`signal_handler` and `alarmCatcher` share a prologue and epilogue:
- save `errno`; if `MMU32bit` (0xcb2) is set and bit 0 of `MMFlags` (0x1efc) is clear, call `sSwapMMUmode(0)` (switch to 24-bit) and swap back afterwards;
- `signal_handler` only runs routines whose recorded pid matches `getpid()`, and only switches MMU mode when the Mac "current process" (0x3ff8) is this pid;
- both finish by calling `doDTasks`, then restore `errno`.

Keyboard and mouse input do **not** use signals. The Event Manager traps (`aGetOSEvent`, `aOSEventAvail`, `aPostEvent`, `aFlushEvents`) talk to the kernel event queue through uinter ioctls (`doOSEvent` = Q19, `cPostEvent` = Q16, `cFlushEvents` = Q18, `cFindEvent` = Q31). Waiting uses `UI_sleep` (Q39) with a deadline checked by the tick (below) and a mouse rectangle (`setSelRect`, Q26). Whether the kernel's event posting also wakes the sleeper directly is not confirmed.

### Kernel side of the tick

- `setTimer(1)` (0x6436) issues uinter ioctl `0xc0045123` (Q35). `startTime` 0x105fa stores `alarmCatcher` at low memory 0x3ff0, calls `attach_timer`, then `setTimer(1)`. `startTime` is reached from `_AUXDispatch` selector 47 via `attach_timer`.
- Kernel `UI_timer` 0x1008629a: for the caller's layer (`ui_layer` + layer×0x6b6), if not already armed (+0x36), `timeout(ui_catch_timer, layer, 1)`. The argument does not set a rate.
- Kernel `ui_catch_timer` 0x100864b2 runs every clock tick. If a sleep deadline (+0x3a) has passed (`lbolt`), it calls `wakeup(+0x2e)`. It always calls `psignal(layer proc, 6)` and re-arms `timeout(…, 1)`. So SIGIOT arrives at the kernel clock rate: `HZ` = 60 in `<sys/param.h>`.

### Virtual interrupt mask and signal delivery (kernel)

- The virtual SR lives in the u-area (0x12fff548). `lpriv` (the privileged-instruction emulator) writes it on `move/ori/andi/eori` to SR and mirrors the IPL into the global byte `ui_SR` (0x11005e48).
- `P_SIGMASK(p)` 0x10027a54, used by signal delivery (`psig`, `sigblock`, …): for a process in the Mac layer (bit 1 of `p_flag` byte 0), if `ui_SR` ≠ 0 the effective mask becomes `ui_sigmask | 0xffffbefb`. That blocks **every signal except 3 SIGQUIT, 9 SIGKILL and 15 SIGTERM**, and it sets `ui_pending` if something was held back. If `ui_SR` = 0, the mask is `ui_sigmask`.
- `ui_sigmask` (0x11005e40) is recomputed by `UI_setsched` 0x100851c8 as the OR of the signal masks of all processes in the layer. `ui_curlevel` is the highest priority value among them.
- When Mac code lowers the IPL to 0, `lpriv` sets `ui_SR` = 0; if `ui_pending` is set, it calls `UI_sigpending` (via `ui_sigpending` pointer) and clears it. `UI_sigpending` 0x10085252 walks the layer's processes with deliverable pending signals and makes them runnable (`setrun`) or requests a reschedule (`runrun`/0x5b05c), so the held signals are delivered on the way back to user mode.
- Net effect: raising the virtual IPL (e.g. `ori #$0700,sr` in ROM code) holds off ticks and I/O signals, as on real hardware. The virtual IPL is a single global for the Mac session, not per process.

### How an "interrupt handler" ends

- The kernel does **not** raise the virtual IPL when it delivers a signal (`ui_SR` is not referenced by `sendsig`/`psig`), so the handler runs at the IPL that was current, with the signal itself blocked by normal BSD semantics.
- The Mac environment uses BSD signals (`startmac` sets `COMPAT_BSDSIGNALS` via `set42sig`), so Mac handlers return through an on-stack stub (`trap #15`, d0 = 150) into the kernel's `sigcleanup` ([aux-syscall-translation.md](aux-syscall-translation.md) §4). The SVR3-style path, unused here, is the libc trampoline `_sigcode` 0x526b4: it calls the handler, then restores the full frame with `sysm68k(2, …)` (syscall 38; kernel case 2 does `copyinframe`) or, for a simple frame, pops it and returns with `rtr`. Neither involves `rte` emulation.
- **A-line traps do not return through `rte`.** `lineAVector` builds a standard 68020 format-0 frame on the user stack (SR.w, PC.l, format.w; 8 bytes; the SR word is the hardware SR ORed with the virtual SR) and resumes at the handler in `$28`. That handler is the ROM's A-trap dispatcher (Foreign OS table entry 1), installed by the ROM's dispatcher-initialisation entry (entry 0). It consumes exactly that 8-byte frame and always finishes with `rts` or `rtd #4`, never `rte`: for Toolbox traps it overwrites the frame with the routine address and updated return PC; for OS traps it calls the routine and pops the frame. So the SR word in the frame is discarded, and a host only has to produce this 8-byte frame; no `rte` emulation is needed for trap dispatch.
- `lpriv` does handle `rte` for format 0 (and 2, 3, 9), reading the format word at user SP+6. In `Patch.067C` only `doMacsBug`, `entermacsbug` and `SysErrExit` execute `rte`; ROM exception paths and MacsBug may too.

## 2. Mac→Unix gateways and trap patching

### How A-traps are dispatched

- The kernel reflects A-line exceptions to the handler address at Mac low memory `$28` (`lineAVector`). The handler there is the ROM's A-trap dispatcher, installed by the ROM's dispatcher-initialisation entry (called in user mode by `doDispatch`; see docs/aux-startmac-boot.md). `Patch.067C` doesn't replace it except under the MacsBug debug option (`installAline(inmacsbug)`). The dispatcher uses the standard trap tables in low memory: OS table at `$400` (256 entries) and Toolbox table at `$E00` (1024 entries).
- `installAline(new)` 0x1354c saves `$28` in 0x55bc4 and installs `new`. `doDispatch` uses it to install `inmacsbug` when bit 3 of `startInfo+0x40` (a debug option) is set.

### Patch.067C trap tables (`doDispatch` 0x7b02)

`doDispatch` calls `patchTable(dst, src, count, unimpl)` 0x7b74 twice:
- `ToolPatch` (0x545bc, 1024 entries) → Toolbox table `$E00`
- `OSPatch` (0x555bc, 256 entries) → OS table `$400`

For each entry: a non-zero `src` replaces the table slot; a slot that still points at `_Unimplemented` (from `NGetTrapAddress($A89F)`) is set to `noSupport` 0x1238c. Real replacements (78 OS, 22 Toolbox):

- Toolbox: `$A801–$A807` Sound Manager, `$A815` `aSCSIDispatch`, `$A833` `aGetScrnBits`, `$A860` `aWaitNextEvent`, `$A895` shutdown, `$A96F` `aEnqueue`, `$A973` `aStillDown`, `$A974` `aButton`, `$A976` `aGetKeys`, `$A977` `aWaitMouseUp`, `$A9C8` `aSysBeep`, `$A9C9` `aSysError`, `$A9FF` `aDebugger`, `$ABFF` `aDebugStr`, **`$ABF9` `aAUXDispatch`**, **`$ABFA` `aAUXSysCall`**.
- OS: the whole Memory Manager (`$A019–$A02D`, `$A036`, `$A040`, `$A048–$A04D`, `$A057`, `$A05C`, `$A061–$A06A`), OS Event Manager (`$A02F–$A032`), VBL (`$A033/$A034`, `$A06F–$A072`), Time Manager (`$A058–$A05A`), `$A03B` Delay, `$A038/$A039/$A03A` param RAM and clock, `$A051/$A052` XPRAM, `$A055` StripAddress, `$A05B` PowerOff, `$A05D` SwapMMUMode, `$A06E` SlotManager, `$A077–$A07C` ADB, `$A082` DTInstall, `$A091` Translate24To32, `$A092` EgretDispatch, `$A098` HWPriv, `$A0B8` SoundDead, `$A0BB` IAZPostInit.

Other traps are patched individually with `NSetTrapAddress`, chaining to the old routine where needed (tool: `tools/trapinstalls.py`):

| Where | Traps |
|---|---|
| `doAUXPrePatches` 0x83a4 | `$A047` SetTrapAddress, `$A002` Read, `$A9A1` GetNamedResource, `$A9C9` SysError, `$A08D` DebugUtil, `$A995` InitResources, `$A94C` FlashMenuBar; low-mem 0x6c4 hook → `aDtrmV2Patch` |
| `doAUXPostPatches` 0x850a | `$A05C` MemoryDispatch, `$A00E`/`$A00F` Unmount/MountVol, `$A8B5` ScriptUtil, `$A260` HFSDispatch (`aKFSDispatch`, the Unix-filesystem pass-through), `$A093` MicroSeconds, `$A895` Shutdown, `$A0B6`/`$A0B7` WaitUntil/SyncWait, `$A11E`/`$A040`/`$A036` NewPtr/ResrvMem/MoreMasters (040 variants), `$A99C` CountResources, `$A0E4` RfNCall |
| `install_HybridPatches` | `$A8FD` PrGlue, `$A9B4` SystemTask, `$A9EA` Pack3, `$A82E` Pack12 |
| others | `$A823` FindFolder, `$A02E` BlockMove (040), `$A9B5`/`$A931` script menu patches, `$A003` Write (`vfs_PBWrite_patch`, async I/O), `$A9A0`/`$A81F` GetResource/Get1Resource (`initlmgr`), `$A033` VInstall (`__DDP_Open`) |

`doMacPatches` 0x7d82 also loads the System file's own `'PTCH'` 0 and `'PTCH'` <ROM version> resources through `callPatch`.

### `_AUXSysCall` ($ABFA) → `aAUXSysCall` 0x13940

```
movea.l (a7)+,a0   ; return address
move.l  (a7),d0    ; syscall number (first argument)
move.l  a0,(a7)    ; put return address where the number was
trap    #0         ; remaining arguments are on the stack, C order
bcc     ok
jmp     cerror     ; 0x526a8: errno, return -1
```

Mac code can make any `trap #0` A/UX system call directly: `AUXSysCall(number, args…)`.

### `_AUXDispatch` ($ABF9) → `aAUXDispatch` 0x146f4 → `cAUXDispatch` 0x40ec

Pascal `FUNCTION AUXDispatch(selector: INTEGER; p: Ptr): LONGINT`. `cAUXDispatch` switches on selectors 0–61 (jump table at 0x4128). Out of range, and selector 49, log `warning("Invalid AUXDispatch selector: %d")` and return −1. On entry, if `hookstate` (0x52f12) is 1 it first calls `install_queue_hooks` 0x879c. Selector names are not shipped with A/UX; the table gives each selector's implementation in `Patch.067C` (names from its own symbols).

| Sel | Implementation |
|---|---|
| 0 | returns 61 |
| 1 | `*p = &errno` (0x5a0cc) |
| 2 | `*p = printf` (0x46210) |
| 3 | `*p = signal` (0x51cd8) |
| 4 | `getIOTimeOut` |
| 5 | `setSelRect(p)` (uinter Q26) |
| 6 | `checkProcKids(*p)` (Q27) |
| 7 | `cPostModified(p)` (Q30) |
| 8 | `cFindEvent` (Q31) |
| 9 | `*p = startInfo->uinter fd` (+0x38) |
| 10 | `*p = startInfo->console fd` (+0x34) |
| 11 | `*p = environ` |
| 12 | `cLogout` |
| 13 | `ui_switch` (Q38/Q39) |
| 14 | `ui_gettask` |
| 15 | inline 0x4276. `p` is a `GetAnyEventRec` (`event` 16 bytes, `timeout` +0x10, `mouseBounds` +0x14, `pullIt` +0x18, `found` +0x19, `mask` +0x1a). Blocking mode = `pullIt` ? (`timeout` ? BLOCK 1 : NOBLOCK 0) : (`timeout` ? AVBLOCK 3 : AVAIL 2). Mouse rect = `(*mouseBounds)->rgnBBox`, or (−32767,−32767,32767,32767) via `_SetRect` if nil. Calls `doOSEvent(mode, mask, &event, 1 /*auxevents*/, timeout, rect)` (uinter Q19) and returns its result. `found` is not written here |
| 16 | stores byte at 0x87b08, returns old value |
| 17 | **no-op, returns 0** |
| 18 | **no-op, returns 0** |
| 19 | stores at 0x87b0c |
| 20 | stores at 0x87b10 |
| 21 | `kill(*(short*)p, 15)` |
| 22 | `fork_exec` |
| 23 | `register_signal(31, …)` |
| 24 | `unregister_signal(31, …)` |
| 25 | returns word at 0x87b14 |
| 26 | `cCleanFS` |
| 27 | `trim_cache` |
| 28 | `GetDirName` |
| 29 | `GetFSFreeSpace` |
| 30 | `CheckTBLaunch` |
| 31 | `GetFSUsedSpace` |
| 32 | `CheckSameFS` |
| 33 | `check_4Insert` |
| 34 | `shutDownDialog(…, 1, 1)` |
| 35 | `shutDownDialog(…, 0, 1)` |
| 36 | `aux_2nd_init` |
| 37 | `ui_getcoffname` (Q64), `ctop`, `PBMakeFSSpec` |
| 38 | `get_homedir_cnid` |
| 39 | `geteuid` |
| 40 | `cPostEvtRec` (Q55) |
| 41 | `get_fstype` |
| 42 | `MapID` |
| 43 | `MapName` |
| 44 | `CheckSameDirs` |
| 45 | `vdir_paranoia_ck` |
| 46 | **returns physical memory size**, not an application hook: `uvar(&v)` (`utssys` type 33, fills `struct var`), then `v_maxpmem << v_pageshift` (offsets 0xc4, 0x70 in `<sys/var.h>`); 0 if `uvar` fails |
| 47 | late init: `install_HybridPatches`, `attach_timer` (installs `alarmCatcher`, starts the SIGIOT tick), `attach_vfs_aio_handler` (SIGIO for vfs async I/O); returns 0 |
| 48 | same code as 46 (physical memory size) |
| 49 | not implemented: same path as out-of-range (warning, −1) |
| 50 | returns −1 without a warning |
| 51 | `GetVolFreeSpace(*(short *)p)` (vRefNum) |
| 52 | `GetPathID(p)` |
| 53 | `GetPathFreeSpace(*(long *)p, p+4, p+8)`; result returned directly |
| 54 | `FreePathID(*(long *)p)` |
| 55 | `vf_fcb_addr(*(short *)p, &err)`: FCB address for a file refnum; on success stores it at `p+2`; returns `err` |
| 56 | `get_cache_info(p)` (File Manager cache statistics) |
| 57 | `clear_cache_info()` |
| 58 | inline 0x45c2: returns `vfs_max_opens` (long at 0x55d18), the maximum number of Unix files the vfs layer keeps open |
| 59 | `getBufCache(p)` |
| 60 | `setBufCache(p)` |
| 61 | `get_mnt_dirIDs(p)` (dirIDs of mount points) |

All selectors are decoded from code. Remaining uncertainty: the argument structures behind 52, 56, 59–61 (passed straight to the named functions) are not laid out here; A/UX 3.1 uses selector 46 for the memory-size query.

## Implications for a host other than A/UX

- The tick is one periodic signal. Any host with `setitimer` or a timer driver can provide it, as long as it can be **held back while the virtual IPL is raised** and released when it drops. On a host without A/UX's `P_SIGMASK` hook, the privileged-instruction emulator would have to block and unblock signals (`sigprocmask`) as it emulates SR writes.
- I/O completion uses SIGIO (31) and SIGURG (30) on the host's descriptors; sound uses SIGEMT (7) and SIGUSR1 (16) from A/UX-specific drivers.
- Signal handlers return through `sigcleanup` via the BSD on-stack stub, so the host needs the A/UX signal-frame layout, or the trampoline must be replaced.
- `_AUXSysCall` passes raw A/UX system-call numbers from Mac code, so the syscall translation layer also covers Mac applications that use it.

Tools: `tools/trapinstalls.py` (NSetTrapAddress call sites with trap and routine).
