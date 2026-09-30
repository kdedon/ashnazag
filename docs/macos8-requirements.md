# Mac OS 7.6.1 and 8.1 in the A/UX Mac environment: requirements

What it takes to run Mac OS 7.6.1 and 8.1, instead of A/UX 3.1's System 7.0.1, under `startmac` + `Patch.067C` on the unified SVR4 kernel. Documentation and static analysis only.

Sources: the A/UX 3.1 image (`Patch.067C`, `libmac1_s`, System 7.0.1 in `/mac/sys`), the three `$067C` ROMs, the user's **Mac OS 7.6.1 and 8.1 CDs** (bootable System Folders, installer scripts, read-me files), the other `docs/aux-*.md`, and public Apple documentation.

Markers: *(verify)* = from public documentation or memory, not checked against media or code; *(inferred)* = read from code, meaning inferred. Everything else was checked against the binaries.

## 0. Summary

**Key requirements**

1. `Patch.067C` stays the machine layer. It runs unchanged on the Quadra 800 ROM (§5), and newer Systems chain their patches onto its trap tables as 7.0.1 does.
2. A **Mac OS adaptation layer** of our own, loaded between `Patch.067C` and the System's boot code:
   - re-guard the A/UX sites Apple removed from 8.1's `'boot'` 3 (7.6.1 still has them);
   - force VM off;
   - resolve the File Manager conflict with 8.1's HFS Plus `'ptch'`;
   - hand sound to Sound Manager 3.x through a host `'sdev'` component;
   - answer the Display Manager's video-driver calls.
3. **Process Manager hooks** equivalent to A/UX's `AUX_*` segment: idle blocking in the kernel, Unix processes as Mac tasks, COFF launch (§4). The 7.6.1/8.1 Process Manager is stripped of symbols and restructured.
4. **CFM-68K** under A/UX's Memory Manager and Process Manager: 8.1's Finder is a CFM-68K application.
5. **Open Transport providers** (TCP/UDP/DNS over host sockets, AppleTalk over the A/UX kernel modules) for 8.1, per the networking decision. 7.6.1 can run on `Patch.067C`'s classic drivers if OT is left out *(verify)*.
6. A 68040 host, or a 68060 with trap-and-patch, for 8.1. 7.6.1 also runs on 68030 hosts.

**Biggest risks**

| Risk | Why |
|---|---|
| 8.1 `'boot'` 3 runs code A/UX used to skip | 7 `hwCbAUX` guards in 7.6.1, 0 in 8.1: ROM-version jumps into ROM code, `.Sony` cache control, `'lmgr'` load, a BufPtr adjustment |
| File Manager | `Patch.067C` owns the File Manager (Unix volumes); 8.1's `'ptch'` −20217 grows from 4.5 KB to 138 KB (HFS Plus) and patches the same traps |
| Process Manager graft | no symbols in the 7.6.1/8.1 Process Manager; A/UX's hooks sit partly inside its scheduler |
| Patches that assume ROM managers | 7.5+ patches written after A/UX may lack the `notAUX` condition and reach ROM internals `Patch.067C` replaced |
| CFM-68K, ASLM, Thread Manager | new consumers of Memory Manager, Segment Loader, Deferred Task and `fsave`/`frestore` behaviour that A/UX virtualizes |
| Memory | `libmac1_s` caps Mac RAM at 16 MB; 8.1 + OT + ASLM + Appearance may not fit *(verify, measure)* |

**Media**: both CDs are in hand. Still missing: nothing for 7.6.1/8.1 themselves (§7). A Python `rsrcfork` decompressor is needed to read 7.0.1's compressed resources (the 7.6.1/8.1 resources are uncompressed).

## 1. What the A/UX environment depends on in System 7.0.1

| Dependency | 7.0.1 mechanism | Source |
|---|---|---|
| Machine layer first | `Patch.067C` overlays both trap tables at `doDispatch` (78 OS, 22 Toolbox replacements), then patches ~40 more traps one at a time; its own Memory Manager, OS Event Manager, VBL/Time Manager/Deferred Task, Slot and SCSI Managers, ADB, Sound (`$A801`–`$A807`), Device and File Manager (Unix vfs), Gestalt | [aux-interrupts-and-gateways.md](aux-interrupts-and-gateways.md) §2 |
| System patch loading | `doMacPatches` runs `'PTCH'` 0 and `'PTCH'` 1660 (`$067C`) via `callPatch`; `BootFromRsrc` loads `'boot'` 2 and jumps to it with a fabricated boot-block environment (stack at MemTop/2, A5 world, `InitGraf`, `$D00` timing constant per BoxFlag) | `Patch.067C` 0x7d82, 0x11850 |
| Linked patches | `'lpch'` entries carry a condition mask; `'boot'` 2 computes the machine's condition word, with a **notAUX** bit (bit 7 in 7.0.1) clear when `hwCbAUX` is set | `'boot'` 2 at 0xf6e *(inferred)* |
| Apple's own A/UX guards | 7.0.1 `'boot'` 2 tests `hwCbAUX` 5 times: `.Sony` track-cache `_Control`, `'lmgr'` load, ROM-version jump table, condition word; `'ptch'` 5 checks Gestalt `'a/ux'`. Stock 7.0.1 has the same guards; A/UX's `'boot'` 2 only adds `_AUXDispatch(36, 1/0)` around script/extension loading | 7.0.1 `'boot'` 2, [AUX_ANALYSIS.md](../AUX_ANALYSIS.md) |
| A/UX-only System parts | Process Manager (`'scod'` −16461 `AUX_*` segment, plus hooks in −16468, −16464), `'boot'` 2, Finder `7.0.1 (A/UX)` (COFF launch, CommandShell) | §4 |
| System version | `Patch.067C` compares `SysVersion` (`$15A`) 6 times, only against `$700`; `startmac` falls back below `$700` | `Patch.067C` |
| Gestalt | `Patch.067C` implements Gestalt and installs `'a/ux'`, `'addr'`, `'snd '`, `'pgsz'`, `'prty'`, `'micn'`, `'mach'` | `InstallMyGestalts` 0x7ebc |
| ROM | Foreign OS table entries 0–4; 16 fixed ROM addresses + `$29A = ROMBase+$1D470`; ROM resources through `Patch.067C`'s Resource Manager | [aux-startmac-boot.md](aux-startmac-boot.md) |
| Kernel | uinter (~50 ioctls), A-line reflection, `lpriv`, SIGIOT tick with virtual IPL, `sysslotmanager`, `UI_VIDEO_*` | [aux-kernel-mac-support.md](aux-kernel-mac-support.md) |

The whole scheme rests on two facts: `Patch.067C` installs its managers before any System code runs, so System patches chain onto them via `GetTrapAddress`; and Apple's System code skipped hardware work when `hwCbAUX` was set.

## 2. Changes from 7.1 to 8.1

Columns 7.6.1 and 8.1 are from the CDs unless marked.

| Area | 7.1 | 7.5.x | 7.6.1 | 8.1 |
|---|---|---|---|---|
| Boot code | *(verify)* | *(verify)* | `'boot'` 2 = 648-byte stub; `'boot'` 3 (31 KB) does the work; linked-patch loader split out as `'lodr'` −16385 | `'boot'` 3 (35 KB); extra `'boot'` 22460 (16 KB, contains "A Power PC based computer is required"; role unknown) |
| A/UX guards in boot | *(verify)* | *(verify)* | 7 `hwCbAUX` tests, `_AUXDispatch(36, 1/0)` as in A/UX's 7.0.1 | **none**; the same code sites remain, unguarded |
| notAUX patch condition | as 7.0.1 *(verify)* | *(verify)* | `'lodr'` computes it | `'lodr'` still computes it (bit 8); `'ptch'` 5 still checks `'a/ux'` |
| Patch resources | enablers add patches *(verify)* | | `'PTCH'` 0/117/376/630/890/1660/1917, 12 `'ptch'`, 21 `'lpch'`, 52 `'gpch'`, 34 `'gtbl'` | same `'PTCH'` set, 13 `'ptch'`, 21 `'lpch'`, 53 `'gpch'`, 36 `'gtbl'` |
| Machine support | System Enablers; Quadra 800 needs "System Enabler 040" *(verify)* | Q800 native *(verify)* | `'gbly'` −16385 lists Gestalt machine IDs: 35 (Q800), 20, 22, 26, 11 (IIci) and 030 models | `'gbly'` −16385: 040 and PowerPC models only; 35, 20, 22, 26 kept, 11 dropped. Installers add no enabler for the Q800 |
| Process Manager | | | 12 `'scod'`, 154 KB with `'proc'`; stripped; main segment −16463 is 63 KB; no `AUX_*` code | 12 `'scod'`, 219 KB; new 65 KB segment −16465; `'sfvr'` "ProcessMgrSupport" |
| VM | | | `'ptch'` 42 (`LOADVM`, `IsMachineVMable`); read-me: VM is turned on at install | `'ptch'` 42 |
| File Manager | | | `'ptch'` −20217 4.5 KB; large disks up to 2 TB on 040, not bootable | `'ptch'` −20217 138 KB: **HFS Plus**; needs Text Encoding Converter; 68k cannot boot from HFS Plus |
| CFM-68K | | extension *(verify)* | "CFM-68K Runtime Enabler" extension (read-me: re-released for 7.6.1) | in the System: `'cfm '` 0 `CFMac68k`, `'cfm '` 1 `RSEGLoader`, m68k fragments (InterfaceLib, ThreadsLib, DragLib…). **Finder 8.1 is a CFM-68K PEF** |
| Networking | classic; OT 1.x optional (OT supports 7.1+) | OT 1.1 from 7.5.3 *(verify)* | installs OT 1.1.1 and removes MacTCP; OT shims `'otdr'`/`'otlm'` 9 `.MPP` in System | OT 1.3 (1.3.1 update on CD): ASLM libraries "Open Transport Library", "Open Tpt AppleTalk/Internet Library" + "Shared Library Manager"; System still carries `DRVR` `.MPP`/`.ATP`/`.XPP`/`.DSP`/`.ENET` |
| Sound | Sound Manager 2 *(verify)* | Sound Manager 3.x *(verify)* | Component Manager `'thng'`s in System: `'sdev'` (`asc `, `awac`, `sing`, …), `'sift'`, `'sdec'`, `'mixr'` | same, plus `'sind'`, `'soph'` |
| Video | | Display Manager *(verify version)* | System `DRVR` 123 `.Display_Video_Apple_DAFB` | same |
| Look and Finder | | Thread/Drag Managers, AppleScript *(verify)* | Finder 7.5.6 (classic `CODE`) | Finder 8.1 (threaded *(verify)*), "Appearance Extension" (`INIT`/`appr`), Charcoal |
| CPU, addressing | 24/32-bit | | 68030+; 32-bit only *(verify)* | 68040 (from `'gbly'`); 68LC040 models listed, so no FPU requirement *(verify)* |

`'lodr'` condition word in 8.1 *(inferred from code)*: bit 8 notAUX, bit 7 VM off (Gestalt `'vm  '` bit 0 clear), bits 20/21 ROM `$067C` release (Q700 `$15F1` and Q800 `$23F2` → 21, IIci → 20), bits 16/17 release ≥ `$12F1`, bit 11 MemoryDispatch present, bits 9/10 MMU type, bits 12–14, 18/19, 22/23 low-memory and driver checks. 7.0.1 used the same scheme with different bit numbers.

## 3. `Patch.067C`'s replacements: keep, break, new

| Area | 7.6.1 | 8.1 | Work / host service |
|---|---|---|---|
| Trap-table overlay, one-at-a-time patches | keep | keep | none |
| Boot entry (`BootFromRsrc` → `'boot'` 2) | keep: the stub loads `'boot'` 3 *(verify at run time)* | keep, plus **re-guard** the 7 sites (ROM-version jumps, `.Sony` csCode 9, `'lmgr'`, `$AD` trap probe, BufPtr −= `$1800`, `_AUXDispatch(36)`) | adaptation layer: patch `'boot'` 3 in memory after `GetResource`, by byte signature |
| Memory Manager (own) | keep | keep | force VM off: PRAM VM flag, Gestalt `'vm  '`, keep `'ptch'` 42 from loading; audit `'gpch'`/`'lpch'` without notAUX for MM internals |
| OS Event Manager, cursor, keyboard (uinter) | keep | keep | check `'ADBS'` (3 in 7.6.1/8.1 vs 1 in 7.0.1) against A/UX's ADB traps |
| Time Manager, VBL, Deferred Tasks (SIGIOT tick) | keep | keep | OT and ASLM run at deferred-task time: verify `DTInstall` and interrupt-time rules under the signal model |
| Slot Manager (syscall 66), video (`UI_VIDEO_*`) | keep | keep | Display Manager Status/Control codes (GetConnection, GetModeTiming, GetNextResolution, SwitchMode *(verify list)*) on the display service; present only the fake NuBus card so the System's DAFB driver never binds |
| SCSI (`$A815`) | keep | keep | whether 7.6.1/8.1 install SCSI Manager 4.3 on a Q800 *(verify)*; it must not replace A/UX's `$A815` |
| File Manager (Unix vfs, `$A260`) | keep (small `'ptch'` −20217) | **conflict** with the HFS Plus `'ptch'` −20217 | decision needed: suppress it (no HFS Plus), or let it own HFS/HFS Plus block devices and keep Unix volumes on A/UX's code. HFS/HFS Plus images then need a Mac block driver over a Unix file (`_AUXSysCall` read/write) and `UI_PUTDQEL` |
| Sound (`$A801`–`$A807`, `/dev/snd`) | conflict with Sound Manager 3.x *(verify)* | same | let Sound Manager 3.x run; a host `'sdev'` component (Component Manager) over the kernel sound service |
| Classic AppleTalk, MacTCP `.ipp` | work if OT is not installed *(verify)* | replaced by OT | OT `tcp`/`udp`/`dnr` providers over host sockets; AppleTalk providers over the A/UX kernel modules; disable the classic drivers when OT loads |
| ADB, Egret, HWPriv, PowerOff, Shutdown | keep | keep | covered by the `'boot'` 3 re-guard |
| Gestalt | keep | keep | `'mach'` must be in `'gbly'`: 35 (Q800) or 22 (Q700) |
| Process Manager, Finder | graft (§4); Finder 7.5.6 stock | graft; Finder 8.1 stock (CFM-68K) | COFF launch and CommandShell move from the A/UX Finder into the Process Manager hooks |
| MacsBug hooks | keep | MacsBug 6.5.4a3+ required (8.x read-me) | only when debugging |
| Memory size | 16 MB cap in `libmac1_s` | same | measure; raising it needs a `libmac1_s` change |

## 4. The A/UX Process Manager graft

### 4.1 In A/UX's 7.0.1

A/UX's Process Manager (86 KB against stock 59 KB, per [AUX_ANALYSIS.md](../AUX_ANALYSIS.md)) keeps MacsBug symbols. The A/UX code is one extra segment, `'scod'` −16461 (2.4 KB, 18 routines), plus hooks named `C_GETAUXMENUITEM` (−16468) and `C_LGETAUXWIN` (−16464). Roles are *(inferred)* from names and `_AUXDispatch` selectors ([aux-interrupts-and-gateways.md](aux-interrupts-and-gateways.md) §2):

| Routine | `_AUXDispatch` | Role |
|---|---|---|
| `AUX_Setup` | 0 (version = 61) | start-up; looks up `$ABF9` and `$A971` (EventAvail) |
| `AUX_DISPATCH_PATCH`, `AUX_GETNEXTEVENT`, `AUX_EVENTAVAIL` | | patches on `_AUXDispatch` and the event calls |
| `AUX_GetAnyEventGlue`, `AUX_WaitForEvent`, `AUX_GetMinTimeout`, `AUX_GetMouseBounds`, `AUX_IdleProc` | 15, 21 | **idle blocking**: when no process needs the CPU, wait in the kernel (`UI_GETOSEVENT` BLOCK) with the smallest sleep time and the mouse region |
| `AUX_KernelEvents`, `AUX_HandleEvent` | | kernel events: `attachEvt`, `exitEvt`, `selEvt` |
| `AUX_MakeLayer`, `AUX_KillLayer`, `AUX_FindProc`, `AUX_GetTaskGlue` | 37, 21, 16, 14 | **Unix processes as Mac tasks**: a Process Manager entry per attached COFF process; teardown |
| `AUX_SwitchGlue`, `AUX_MakeRunnable`, `AUX_Unblock`, `AUX_ProcessMgrTask` | 13, 14 | switching between tasks in different Unix processes (`UI_SWITCH`) |
| `AUX_Launch`, `AUX_ForkExecGlue` | 21, 22 | launching COFF binaries (`fork_exec`) |

### 4.2 What an 8.1 equivalent must provide

1. Idle blocking in the kernel instead of a busy loop, with the timeout taken from all processes' `WaitNextEvent` sleep values and Time Manager deadlines.
2. Unix processes as Process Manager processes: create on `attachEvt`, remove on `exitEvt`, schedule through `UI_SWITCH`/`UI_SLEEP`, wake on `selEvt`.
3. COFF launch from `_Launch`, the Finder and CommandShell, with Finder 8.1 launching through its own (CFM-68K) path.
4. Coexistence with 8.1's per-process state: Thread Manager threads, CFM-68K fragment contexts and their `ExitToShell` cleanup ("ProcessMgrSupport").
5. Shutdown and logout: terminate the layer's Unix processes.

### 4.3 How

The 7.6.1 and 8.1 Process Managers have no symbols and a different segment layout (one 63 KB main segment), so the 7.0.1 names do not carry over directly. Order of work:

1. Put what sits at public boundaries into trap patches in the adaptation layer: `WaitNextEvent`/`GetNextEvent`/`EventAvail`, `_Launch`, `ExitToShell`, `_AUXDispatch`.
2. Map the call sites of `AUX_*` in the 7.0.1 A/UX Process Manager (jump-table references), and diff it against stock 7.0.1 to get the internal hooks (scheduler idle point, process creation).

## 5. ROM

- **Quadra 800 ROM**: checksum `F1ACAD13`, version `$067C`, release `$23F2`, 1 MB, same Foreign OS table as the IIci and Quadra 700 ROMs (`$9A96`, `$99B0`, `$9AE6`, `$5DE0`, `$CC60`, `$5C`). Its dispatch table is at `$D3370` (Q700 `$CA0E0`).
- **`Patch.067C` runs on it unchanged**: all 16 fixed ROM addresses and `$1D470` hold byte-identical code in the Q700 and Q800 ROMs (`$F04C` differs only from the IIci). This closes the open point in [aux-startmac-boot.md](aux-startmac-boot.md).
- Differences from the Q700 ROM are in resources: the Q800 ROM adds `'ecfg'`/`'pslt'` for Wombat/WLCD/Vail models, `'enet'`, `'accl'`, DiskMode `'PICT'`s, and moves `PACK` 4/5 (FPU versions at `$E9A20`/`$EDCA0`).
- **8.1 supports the Q800 without an enabler**: Gestalt 35 is in the System's own `'gbly'`, the installers add no enabler for it, and the patches for `$067C` ROMs are in the System (`'PTCH'` 1660, `'lpch'`/`'gpch'` by condition). By the `'lodr'` ROM bits, the Q700 and Q800 ROMs get the same patch selection *(inferred)*.
- **What we produce instead of a new Apple patch file**: nothing replaces `Patch.067C`. The adaptation layer (§0) is our own code, loaded as a second COFF after `Patch.067C` or built into the launcher *(choose)*, and applies:
  - the `'boot'` 3 re-guard and VM-off settings;
  - suppression lists for `'ptch'`/`'lpch'`/`'gpch'` entries that fight `Patch.067C` (from the notAUX audit);
  - the Process Manager hooks;
  - the `'sdev'` and Display Manager glue;
  - the OT providers, as separate OT modules.

## 6. 68040 and 68060

- 8.1 needs a 68040: its `'gbly'` lists only 040 and PowerPC machines. Hosts report `CPUFlag` 4 on 040 and 060.
- FPU: 68LC040 machines are in the list, so 8.1 does not need an FPU *(verify)*. On a 68LC060 report no FPU, as [rom-060.md](rom-060.md) says.
- Caches: CFM-68K, the Segment Loader and the OT/ASLM loaders flush the caches on every code load (ROM `$6F4` routine uses `cpusha` when `CPUFlag` = 4). Each flush is an `lpriv` exception; a fast path or `sysm68k` 0x69 glue is needed.
- `fsave`/`frestore`: the Thread Manager saves FPU state on every switch, and Finder 8 uses threads. These are privileged, so each is an `lpriv` exception *(measure)*.
- 060: trap-and-patch as planned. The ROM patch table in [rom-060.md](rom-060.md) was built for the Q700 ROM; the Q800 ROM needs its own entries (`PACK` 4/5 moved). 8.1's System and CFM-68K code need a `scan060.py` pass *(to do)*.
- 030 hosts: 7.6.1 with the IIci ROM (Gestalt 11 is in 7.6.1's `'gbly'`).

## 7. Staged plan (QEMU `q800`)

Reference runs: QEMU `q800` boots the same 7.6.1 and 8.1 CDs natively with the Q800 ROM. Those runs give known-good behaviour and resource-load traces.

| Stage | Content | Pass criteria | Media |
|---|---|---|---|
| **S0** = M3/M4 | A/UX's 7.0.1 on our kernel | [aux-kernel-design.md](aux-kernel-design.md) M3, M4 | A/UX 3.1, Q800 or Q700 ROM (have) |
| **S1** | 7.6.1 boots | a MacBinary→AppleDouble copy of the CD's System Folder on a Unix volume; `startmac -s` reaches `'boot'` 3's two `_AUXDispatch(36)` calls (`TBVERBOSE`); Finder 7.5.6 desktop with Unix volumes; About This Macintosh shows 7.6.1, Quadra 800; VM off | 7.6.1 CD (have) |
| **S2** | 7.6.1 complete | CFM-68K Runtime Enabler, AppleScript and Apple Guide load; a CFM-68K application runs; a MacTCP application reaches a host with OT absent; Process Manager hooks: CommandShell runs `ls`, a COFF tool launches, CPU idle when the Mac is idle | 7.6.1 CD |
| **S3** | 8.1 boots | `'boot'` 3 re-guard applied; Extensions load (Shared Library Manager, Appearance Extension, Text Encoding Converter); no fault in ROM-version jumps or `.Sony` | 8.1 CD (have) |
| **S4** | 8.1 Finder | Finder 8.1 (CFM-68K) desktop at 1 and 8 bpp; Appearance control panel; threaded Finder copy between Unix volumes; Monitors/Display Manager lists the fake card's modes; sound through the host `'sdev'` | 8.1 CD |
| **S5** | 8.1 networking | TCP/IP control panel shows the host address; OT and MacTCP-compatibility clients reach a host; with M9: Chooser lists an AFP server | 8.1 CD (OT 1.3.1 update on it) |
| **S6** | HFS/HFS Plus images | if chosen (§3): an HFS Plus image made by 8.1's Drive Setup in the reference run mounts next to Unix volumes | none |

Media still needed:
- 7.1 only if the pending 7.1 decision keeps that step (PLAN.md).
- Public documents: *Inside Macintosh: Networking with Open Transport*, the OT Module Developer Note and SDK (OT 68K port and provider registration), the Display Manager and video driver csCode documentation, the Sound Manager 3.x component documentation, the Mac OS 8/8.1 and 7.6 technotes (TN1102, TN1121 *(verify numbers)*).
- Tooling: `rsrcfork` (Python) to decompress 7.0.1 `'lpch'`/`'scod'`/Finder `'CODE'`; a MacBinary→AppleDouble converter to put CD files on Unix volumes.

## 8. Open points

- notAUX audit: decode the `'lpch'`/`'gpch'` entry format from `'lodr'` (clean-side, from Apple's binary) and list the 8.1 entries without notAUX that patch managers `Patch.067C` owns.
- Whether `Patch.067C`'s `doMacPatches` and 7.6.1/8.1 `'boot'` 3 both apply `'PTCH'` 0/1660 (double install).
- `'boot'` 22460's role; the CPU check that enforces 68040 in 8.1.
- SCSI Manager 4.3 on the Q800 under 7.6.1/8.1; `'ADBS'` handling.
- Sound Manager 3.x against `Patch.067C`'s `$A801`–`$A807` replacements.
- Whether 8.1 runs without OT installed (classic `.MPP` from the System or `Patch.067C`).
- 8.1 RAM needs under the 16 MB cap.
