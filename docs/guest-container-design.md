# 68k guest containers: design

Paper design. Generalises the A/UX Mac-environment mechanism ([aux-kernel-design.md](aux-kernel-design.md)) into a framework that runs other 68k operating systems as ordinary SVR4 processes. A profile is the **user's own system ROM** plus a **machine layer** supplied by the container: A/UX's model (Mac ROM + `Patch.067C`) applied to Amiga Kickstart and Atari TOS. The machine layer either passes the real chips through (matching host only) or replaces the hardware-bound parts with host services (any host). Real OS installs (AmigaOS 3.2, TOS/GEM hard-disk setups) boot as on the real machine. EmuTOS and AROS are the free fallback for users without ROMs.

Inputs: `docs/aux-*.md`, [dlm-impl-spec.md](dlm-impl-spec.md), [rom-060.md](rom-060.md), and public documentation: Amiga Hardware Reference Manual and ROM Kernel Manuals, AmigaOS 3.2 release notes, the public Kickstart 3.2.x module inventory, LoadModule and P96 documentation, Atari Compendium, published TOS boot-sequence descriptions, EmuTOS/AROS/ARAnyM project documentation. *(verify)* marks a fact to check before implementing.

## 0. Summary of decisions

| Topic | Decision |
|---|---|
| Unit | A **guest process** is an SVR4 process with a `guest_proc` (the A/UX `p_evpdp` marker). A **container** is one guest session's shared state: guest RAM, ROM copy, virtual CPU, virtual interrupts, surfaces, capabilities |
| Profile | **User ROM + machine layer.** Mac: ROM file + `Patch.067C` (existing). Amiga: Kickstart file + our resident modules. Atari: TOS file + our cartridge-image drivers. Free fallback: EmuTOS, AROS 68k |
| Machine layer | **Replacement** (default, any host): host-backed versions of the hardware-bound OS parts + a few trapped register pages. **Passthrough** (matching host, root grant, exclusive): real chips mapped in; only host-owned devices stay replaced |
| ROM handling | Loaded from the user's file into a private, per-container copy at its native address; identified by hash; never written back, never distributed. Injection uses the OS's own hooks first (Amiga extension ROM area at `$F00000`, KickTags as fallback; TOS cartridge/reset hooks); in-memory patching is a bounded fallback |
| Register emulation | Trap-and-emulate on a fixed, small set of pages: interrupt controllers, timers, chip-ID and probe registers, the Atari keyboard ACIA, frame-level display registers. Writes elsewhere absorbed. No copper, bitplane/sprite DMA, audio DMA, disk DMA, raster timing |
| CPU model | Guest code in real user mode; supervisor state virtual (`vSR`, `vVBR`, `vUSP`/`vSSP`, `vCACR`). Privileged instructions emulated in virtual supervisor mode, reflected otherwise |
| Exceptions | Per-profile disposition table per vector: native / reflect / emulate / syscall / hostcall / filter |
| Interrupts | Virtual lines with levels. Mac: Unix signals (as A/UX). TOS, Amiga: real 68k frames through the guest vector table, raised by the emulated interrupt controllers (Paula `INTREQ`, MFP) |
| Host services | Plain SVR4 syscalls from guest code; `guestcall` (NatFeats `$7300/$7301`) for what Unix lacks; optional kernel fast paths that stand down when a vector is hooked |
| Hot sites | Frequent trapped accesses in ROM code (Kickstart's `INTENA` writes in `Disable`/`Enable`) are trap-and-patched in the ROM copy into paravirtual helper calls, as [rom-060.md](rom-060.md) does for 060 instructions; undone on guest reset before the ROM's checksum runs |
| Display | Surfaces. Amiga: P96 board driver (P96 user-supplied) for Workbench on RTG. Atari: frame-level Shifter/Videl surface, RTG VDI later. Concurrent guests share the screen by hotkey switching |
| Timers | Kernel `HZ` stays 60; an `hrtimer` module programs a spare hardware timer (VIA, CIA, MFP) as a one-shot for the earliest guest deadline; guests run in the SVR4 real-time class |
| Concurrency | Mac, TOS and Amiga containers run at once as separate processes; idle guests block in the host; one guest at most holds passthrough hardware |
| Security | Unix credentials bound the guest. Capabilities are configuration, except raw hardware: root-granted, exclusive |
| Kernel pieces | DLM modules `guestcore`, `guestdev`, `guestsvc`, per profile `auxcore`/`auxexec`/`uinter`, `amigaguest`/`amigaexec`, `tosguest`/`tosexec`. Static shims as designed for A/UX, named `guest_*` |
| Order | A/UX personality on `guestcore`; Kickstart 3.2 on the replacement layer; TOS; three concurrent guests; passthrough; EmuTOS/AROS fallback. Replacement-layer milestones run on QEMU `q800` |

## 1. Profiles: ROM + machine layer

```
            user supplies                       container supplies
   +-----------------------------+   +--------------------------------------------+
   | ROM file (OS core)          |   | machine layer                              |
   |  Mac:   Quadra/IIci ROM     |   |  replacement: host-backed drivers/modules, |
   |  Amiga: Kickstart 3.x       |   |    trapped register pages, surfaces        |
   |  Atari: TOS 2.06/3.06/4.04  |   |  passthrough: real chips + replacements    |
   | OS install (System folder,  |   |    only for host-owned devices             |
   |  AmigaOS 3.2 HDF, TOS HD)   |   | guestcore/guestsvc (all profiles)          |
   +-----------------------------+   +--------------------------------------------+
```

| Profile | ROM | Replacement layer | Passthrough | Fallback |
|---|---|---|---|---|
| Mac | Quadra/IIci ROM file | `Patch.067C` + uinter + fake NuBus card (existing A/UX design) | on Mac hosts the physical ROM and frame buffer; devices stay with the A/UX kernel, as in A/UX | — |
| Amiga | Kickstart 3.x (target 3.2, A1200 image first) | resident modules injected as RomTags at `$F00000` (KickTags as fallback) + custom/CIA/probe pages (§6) | AMIX on A3000/A2500 with the matching Kickstart image (§8) | AROS 68k |
| Atari | TOS 2.06 (ST/STE), 3.06 (TT), 4.04 (Falcon) | cartridge-image drivers at TOS's hook points + MFP/ACIA/Shifter pages (§7) | ASV on TT030 with TOS 3.06 (§8) | EmuTOS |

In the Mac profile the ROM is the user's, `Patch.067C` is the machine layer, and on non-Mac hosts the ROM is a private copy and video is a fake NuBus card. Amiga and Atari use the same shape with our own machine layer in `Patch.067C`'s place.

### 1.1 ROM files

- The launcher reads the ROM file, checks its hash against a table of known images (hash, model, version, layout, patch-site list), and maps a private copy at the native address: Amiga `$F80000` (512 KB; `$E00000` for 1 MB images), TOS 2–4 `$E00000`, TOS 1.x `$FC0000`, Mac `0x40800000`.
- Unknown hashes run with injection by documented hooks only (no patch sites).
- Guest reset (`reset` instruction, `ColdReboot()`, TOS warm boot): the kernel restores patched pages from the file before re-entry (Kickstart sums the whole ROM at reset and reset-loops on a mismatch), keeps guest RAM (reset-resident KickTags and TOS reset hooks survive), re-enters the ROM's reset PC. AmigaOS 3.2 on older ROMs and SetPatch/LoadModule rely on this.

### 1.2 Levels of ROM adaptation (in order of preference)

1. **None**: machine layer attached through documented OS hooks (Amiga RomTags at `$F00000`, or `KickTagPtr`/`KickMemPtr`; TOS cartridge `CA_INIT`, `resvector`, `hdv_*`, BIOS device vectors, warm-boot system variables).
2. **Hot-site patching at run time**: a trapped access at a known site is rewritten in the private copy to `jsr helper` (paravirtual, no exception). Pure performance; the site list comes from the first-fault log, not from ROM contents shipped by us.
3. **Load-time patching**: a known image gets a per-hash patch list (disable a resident tag, skip a probe), with the ROM checksum recomputed in memory (Kickstart: the fix-up long at file offset `$7FFE8`). Used only where 1 cannot work.

Nothing is written back to the user's file or distributed.

## 2. Model

### 2.1 Guest process and container

```
 +----------------------- guest process (SVR4 proc, uid of the user) -------------------+
 |  user mode, real CPU                                                                  |
 |   guest RAM at 0 (shm)   ROM copy   machine-layer image   OS install on volumes      |
 |   launcher (native ELF, dormant after enter)   vCPU page (shared, locked)             |
 +------------------|-------------------------------|------------------------------------+
     exceptions: A-line, F-line, TRAP #n,           |  trap #0: SVR4 syscalls (per profile)
     privilege, illegal ($7300/$7301), faults on    |
     trapped register pages, 060                    |
 --------------------v------------------------------v------------------- kernel ----------
  static gates  ->  guestcore: disposition table of the profile
      reflect | emulate (vSR, rte, movec, cache) | host service | chain to host kernel
      register pages: emulate / absorb / forward (passthrough)
  profile module (auxcore / amigaguest / tosguest)      guestsvc (surfaces, input, timers,
  virtual interrupts -> fsig hold -> sendsig frame       metadata, sockets, audio)  guestdev
```

- `struct guest_proc` (per process, off `p_evpdp`): profile, container, flags, the profile's private block (A/UX: `aux_proc` becomes its extension).
- `struct guest_ctr` (per container, refcounted): RAM shm id, ROM copy and patch list, vCPU page, sentinels (§3.6), register-page state (§5), virtual interrupt lines, timers, surfaces, focus, capabilities, `mod_hold` on the profile module.
- Lifecycle: `ev_exec` creates or drops `guest_proc`; `ev_fork` copies it; `ev_exit` detaches and drops the container with its last member.
- Entering: the launcher calls `GIOC_ENTER` on `/dev/guest` with the ROM reset PC, vSR and stack. Mac enters through A/UX's `UI_SET` (uinter), which calls the same `guestcore` entry.

### 2.2 Virtual supervisor mode

| Field | Meaning |
|---|---|
| `vSR` | S, M, T1/T0, IPL; CCR stays in the real SR |
| `vUSP`, `vSSP` | inactive guest stack pointer; the active one is the real `A7` |
| `vVBR` | guest vector base, default 0 |
| `vCACR`, `vSFC`/`vDFC`, MMU registers | read back by `movec`/`pmove`; cache effects performed for real; MMU state recorded only |
| `vPEND` | pending virtual interrupt levels and vector numbers |

1. **Privilege violation**: `vSR.S` = 1 → emulate (`ori/andi/eori #,SR`, `move to/from SR`, `movec`, `rte` formats 0/2/3/9, `move USP`, `moves`, `fsave`/`frestore`, `cinv`/`cpush`, `stop`, `reset`); `vSR.S` = 0 → reflect to guest vector 8. Mac pins `vSR.S` = 1.
2. **Stack switch** on `vSR.S` change. Mac: `single_stack`.
3. **`stop #imm`**: set vSR, block in the host until a virtual interrupt above the new IPL is pending.
4. **Guest RAM is flat** (no bus error at `$0`–`$7FF` for TOS user code).
5. **MMU**: the kernel keeps the real MMU. Guest MMU setup (TOS 3.06/4.04 `pmove` to TC/TT0/TT1/CRP, Amiga `mmu.library`/`68040.library`) is recorded and read back, never applied. 030 `pmove` on an 040/060 arrives as F-line and is emulated. Guest tables must be identity-like *(verify for each ROM/library)*. Kickstart 3.2 itself uses no PMMU instructions; on an 040 it sets DTT0/DTT1/ITT0/ITT1 by `movec`, and exec derives AttnFlags from which `CACR` bits stick, so `vCACR` keeps the host CPU's writable-bit mask.
6. **FPU/060 support**: Amiga reflects unimplemented-FP and 060 vectors to the guest (its `68040.library`/`68060.library` handles them, as on real hardware); TOS and Mac use the host's FPSP/ISP (`NATIVE`) plus trap-and-patch.

### 2.3 Virtual interrupts and timers

- 7 virtual levels per container. Sources: virtual timers, VBL, input rings, host-call completions, audio buffers.
- **Holding**: the `fsig` override ([aux-kernel-design.md](aux-kernel-design.md) §4.7) holds a signal assigned to a level while `vIPL ≥ level`; Mac keeps A/UX's rule.
- **Delivery**: *signal mode* (Mac, as A/UX); *vector mode* (TOS, Amiga): one carrier signal per container; the `sendsig` wrapper builds the CPU's frame, sets vS and vIPL, and jumps through `vVBR + 4·vec`. For user ROMs the vector and level come from the emulated interrupt controller (Paula levels 1–6; MFP vectored interrupts at `VR` base).
- **Timers**: `guestsvc` virtual timer channels, each expiry a virtual interrupt at its own time from the high-resolution timer service (§10.6), so unmodified ROM handlers see one interrupt per period at the right moment. Without that service they fall back to 60 Hz callouts with bursts and spreads.

### 2.4 The vCPU page

One locked 4 KB page per container, kernel alias (A/UX `UI_MAP` precedent):

| Offset | Field |
|---|---|
| 0x00 | magic, version, profile id |
| 0x08 | `vsr`, `vipl_pending_max`, `in_pv_region` |
| 0x10 | `vusp`, `vssp`, `vvbr` |
| 0x20 | tick catch-up counters per timer channel |
| 0x40 | pending bitmap per level, vector numbers; emulated `INTENA`/`INTREQ` (Amiga) or MFP `IER`/`IPR`/`IMR`/`ISR` (Atari) |
| 0x100 | input event ring |
| 0x800 | async completion ring |

- **Paravirtual helpers**: machine-layer code, EmuTOS/AROS builds and hot-site patches mask and unmask by writing the page, then call `nf_call(GUEST_SYNC)` only if `vipl_pending_max` exceeds the new level. Keeping the Amiga interrupt-controller state here lets a patched `move.w #$4000,$DFF09A` become a user-mode helper with zero exceptions.
- **Paravirtual `rte`** in a registered critical region (treated as `vIPL = 7`).
- 030: page mapped cache-inhibited. Mac keeps A/UX's ui page at 0x3000.

### 2.5 Address spaces per profile

Guest windows sit in quadrant 0 (and 1 for the Mac ROM, `/shlib` and the Amiga RTG surface), using supervisor-only ITT0/DTT0 and the `valid_usr_range` wrapper. Launchers link above the largest guest window *(pick an address)*.

**Mac**: unchanged ([aux-startmac-boot.md](aux-startmac-boot.md)): RAM at 0 (`'tLOW'`), `startmac` 0x10000000, ROM 0x40800000, `/shlib` 0x47c00000, fake NuBus slot `$E` 0xFE000000 on non-Mac hosts, 060 helper page 0xFFFF8000.

**Amiga** (A1200-style map):

| Range | Content |
|---|---|
| 0 … 0x1FFFFF | chip RAM (2 MB shm; physical chip RAM under passthrough) |
| 0x00A00000, 0x00BFA000, 0x00C00000–0x00D9FFFF | PCMCIA attribute, CIA-A alias, slow-RAM probe windows (trapped: boot-phase rule, §5) |
| 0x00BFD000, 0x00BFE000 | CIA-B, CIA-A register pages (trapped, §6.4) |
| 0x00DA0000–0x00DEFFFF | Gayle/IDE/PCMCIA, RTC, Gary/Ramsey probe windows (trapped: boot-phase rule, §5) |
| 0x00DFF000 | custom-chip page (trapped, §6.4) |
| 0x00E00000 | upper half of a 1 MB ROM |
| 0x00E80000 | Zorro II autoconfig page (trapped: "no board") |
| 0x00F00000 | machine-layer image (our RomTags; scanned by every 3.2 image) |
| 0x00F80000 | Kickstart copy (512 KB) |
| 0x07F00000, end of fast RAM | memory-probe pages (trapped: boot-phase rule) |
| 0x08000000 … | container fast RAM, found and added by the ROM's expansion.library (A1200/A3000/A4000 images; `containerinit` for A500/A600) |
| 0x40000000 | RTG surface (P96 board memory) *(address to choose)* |
| thunk stub page, vCPU page | fixed pages *(choose)* |

**Atari** (public TOS memory map):

| Range | Content |
|---|---|
| 0 … 0x7FF | vectors and system variables (written by TOS; warm-boot values preset, §7.3) |
| 0x800 … phystop | ST-RAM (up to 14 MB); screen memory is surface pages |
| 0x00E00000 / 0x00FC0000 | TOS copy (2–4.x / 1.x) |
| 0x00FA0000 | machine-layer image as an application cartridge (128 KB) |
| 0x00FF8000–0x00FFFFFF, 0xFFFF8000–0xFFFFFFFF | ST I/O, both aliases (ST code uses `abs.w`): trapped pages (§7.4) |
| 0x01000000 … | TT-RAM (`Mxalloc`), size from configuration |
| vCPU page | unused gap below 0x00E00000 *(choose)* |

Under passthrough on an Atari host the 060 helper page moves off 0xFFFF8000 *(address to determine)*.

### 2.6 How A/UX's mechanism maps onto the core

| A/UX mechanism | Generic core |
|---|---|
| `p_evpdp` → `aux_proc` | `p_evpdp` → `guest_proc` with profile extension |
| layer, `l_vipl`, `l_pending` | container, `vSR.IPL`, `vPEND` |
| gates $20/$24/$28/$80/$BC/faults/$F4 | same gates → `guest_dispatch(vec)` |
| A-line fast path | disposition `REFLECT_ALINE_MAC` |
| `aux_priv` | `guest_priv` with `vS_pinned`, `single_stack` |
| fault reflection via Mac vectors | reflection through `vVBR`; trapped register pages (new) |
| `fsig` override, `sendsig` wrapper | same, profile hold policy and frame builder |
| SIGIOT tick | virtual timer channel in signal mode |
| 060 trap-and-patch, helper page | generic; also used for hot-site patching of user ROMs |
| ROM private copy on non-Macs | generic ROM copy with patch list and reset restore |
| trap #0/#15 A/UX syscalls | profile-owned trap vectors (`auxcore`) |

## 3. Entry mechanisms and dispatch

### 3.1 Dispositions

| Disposition | Action |
|---|---|
| `NATIVE` | host kernel handler (FPSP/ISP, `nullvect`) |
| `REFLECT` | CPU frame, continue at the guest vector |
| `EMULATE` | `guest_priv`, 060 unimplemented instructions |
| `SYSCALL` | host SVR4 syscall, or the profile's translator (A/UX) |
| `HOSTCALL` | `nf_id`/`nf_call` or thunk stubs into `guestsvc`/profile |
| `FILTER` | per-function: `HOST`, `REFLECT`, `HOST_ELSE_REFLECT`; only while the guest vector equals its sentinel (§3.6) |

Access faults on trapped register pages go to the register-page handler (§5) before any reflection.

### 3.2 Mac (A-line)

As A/UX: `$Axxx` reflected to the ROM dispatcher with the 8-byte frame; `Patch.067C` calls Unix through `trap #0`/`#15`. Later, off by default: kernel-serviced File Manager traps on Unix volumes (guarded by the trap-table sentinel), and Mac-side drivers calling `$7301` (window video, host audio, clipboard).

### 3.3 TOS (TRAP #1/#2/#13/#14, line-A)

| Entry | Default | Notes |
|---|---|---|
| TRAP #1 GEMDOS | `REFLECT` to TOS | host-directory drives serviced by our guest-side GEMDOS layer (§7.5); kernel `FILTER` for file calls as an optional speed-up |
| TRAP #2 AES/VDI | `REFLECT` | TOS's AES/VDI; RTG through a VDI driver later |
| TRAP #13 BIOS | `REFLECT` | devices and disks reach the machine layer through TOS's own vectors (`hdv_*`, `xconstat`…) |
| TRAP #14 XBIOS | `REFLECT`; `FILTER` for `Floprd`/`Flopwr`/`Flopfmt`, `Gettime`/`Settime`, Falcon `VsetMode`/sound | functions whose ROM code drives absent hardware |
| line-A | `REFLECT` | TOS line-A draws into the surface |
| TRAP #0 | `SYSCALL` | for machine-layer code; reflected if a program owns trap #0 |
| illegal | `HOSTCALL` for `$7300/$7301`, else `REFLECT` | |

`_MCH`, `_CPU`, `_FPU` cookies are TOS's own; the container sets `_longframe` and presents the matching hardware so TOS's probes pick the right values.

### 3.4 AmigaOS (library vectors)

Library calls are `JSR -LVO(a6)`: no exception. Kickstart libraries run unchanged; the machine layer consists of resident modules (§6). Host APIs by nature (`bsdsocket.library`, host clipboard) are **thunk libraries**: jump-table entries `JMP` to `$Axxx; rts` stubs; AmigaOS does not use line-A, so the A-line vector is the thunk gate (4096 slots). `SetFunction` works unchanged. Blocking host calls are asynchronous (completion ring + virtual interrupt + `Signal()`), because all Exec tasks share one process.

### 3.5 The generic host call

`$7300` `nf_id(name)`, `$7301` `nf_call(id+sub, args…)`, decoded by the illegal-instruction gate. Names: `GUEST_VCPU`, `GUEST_TIMER`, `GUEST_SURFACE`, `GUEST_INPUT`, `GUEST_META`, `GUEST_SOCK`, `GUEST_AUDIO`, `GUEST_CAPS`, `GUEST_BLK`, plus ARAnyM-compatible `NF_NAME`, `NF_VERSION`, `NF_STDERR`, `NF_SHUTDOWN`. Also reachable from native code as the `guestcall` syscall module (slot e.g. 70, [dlm-impl-spec.md](dlm-impl-spec.md) §9.2).

### 3.6 Sentinels

At "OS ready" (registered by the machine layer) the kernel records the guest vectors in the profile's `FILTER` rows; a filtered exception whose vector changed since (XBRA chain, TSR, debugger) is reflected. Mac trap tables use the same check.

### 3.7 Comparison with earlier systems (public descriptions)

| System | Guest code | OS calls reach the host by | Hardware |
|---|---|---|---|
| A/UX Mac environment | user's ROM + System, native, user mode | `Patch.067C` making Unix syscalls | via A/UX drivers |
| Basilisk II (m68k) | real ROM, native | ROM patched with `$71xx` host calls | none |
| ShapeShifter | real Mac ROM on AmigaOS | patched ROM, replacement drivers | Amiga screen/RTG, files |
| MagiCMac | MagiC on 68k Macs | MagiC's hardware layer calls Mac OS | Mac screen, files |
| WinUAE | real Kickstart, emulated CPU/chipset | host-backed RTG board (P96 driver), bootable host-directory filesystem | emulated |
| Amithlon | real Kickstart 3.1 + OS 3.9 on a JIT, no chipset | Linux drivers behind Amiga devices, P96, AHI | RTG only |
| ARAnyM | TOS/EmuTOS on emulated 040 | NatFeats | mostly none |
| CT60 | real TOS 4.04, patched for 060 | — | real Falcon |

Our design: A/UX's model, native execution, the user's ROM, a machine layer in the OS's own module/hook format (Amithlon/WinUAE precedents for Kickstart without a chipset), ARAnyM's call convention.

## 4. Host services

### 4.1 Services

| Service | Mechanism | Used by |
|---|---|---|
| Files | SVR4 calls from guest drivers; `GUEST_META` sidecars | `Patch.067C`, Amiga host-directory handler, TOS GEMDOS layer |
| Block devices | `GUEST_BLK`: image file per unit (HDF, ADF, ST/MSA, AHDI image), read/write/size/change, async | Amiga `scsi.device`/`trackdisk.device` replacements, TOS `hdv_rw` driver |
| Display | `GUEST_SURFACE`: pages, format, damage, palette, mode set, cursor | fake NuBus card, P96 board driver, TOS Shifter/Videl surface |
| Input | `GUEST_INPUT`: host events → ring + virtual interrupt; focus | uinter, Amiga keyboard/gameport replacements, TOS ACIA page |
| Sound | `GUEST_AUDIO` buffer rings | `audio.device` replacement, AHI driver, TOS XBIOS sound |
| Serial, parallel | host ttys/files | replacement drivers |
| Networking | in-kernel TLI/`sockmod` binding as `GUEST_SOCK` | A/UX sockets, `bsdsocket.library` thunks, STiK/STinG layer (later) |
| Time | virtual timers; wall clock | all; Amiga `battclock`, TOS RTC/IKBD clock |

### 4.2 Display surfaces

- Surface = kernel pages + descriptor {width, height, depth, layout (chunky, Atari interleaved planes, Amiga planes), row bytes, palette}, mapped where the profile wants.
- Backends: **zero-copy** when the host frame buffer format equals the guest's; **converted** (write-protect change tracking, conversion ≤ 50 Hz); **windowed** under a host window system (§10.3).
- First targets: Mac 1 bpp and TOS ST-high on the Q800 frame buffer (zero-copy); P96 8-bit chunky converted.

### 4.3 Input

Host sources: ADB (Mac), Amiga keyboard, IKBD, later X. Events carry host-neutral codes; the profile maps them (ADB, Amiga raw keys, IKBD scancodes). The container owning the screen gets them (§10.4).

### 4.4 Files and metadata

AppleDouble `%name` sidecars for all profiles: Finder info, resource fork, comment (Amiga comments), MS-DOS info (TOS attributes), short name (TOS 8.3), dates; Amiga protection bits in a private entry *(choose id)*. `GUEST_META` keeps sidecars in step. Defaults without sidecar: read-only ← no write permission; hidden ← leading dot; RWED ← owner bits.

### 4.5 Networking, serial, time

Sockets are real descriptors in the guest process; `bsdsocket.library` is a thunk library with async completion (`WaitSelect` on Exec signals). Serial replacements open host ttys from the capability list. RTC reads use the wall clock and the container's time zone.

### 4.6 Capabilities

Set by the launcher before `GIOC_ENTER`; can only narrow afterwards.

| Capability | Content | Enforced by |
|---|---|---|
| rom | ROM file, expected hash | launcher (read with the user's rights) |
| volumes | image files and host directories → guest units/volumes, ro/rw | Unix permissions |
| display, input | surface / own screen; focus / grab | `guestsvc` |
| audio, serial, parallel | device list | device permissions |
| net | allowed families | `guestsvc` |
| syscalls | native `trap #0` allowed | `guestcore` |
| raw hardware | physical ranges and interrupt sources, grade (§8) | `guestdev`: root grant, exclusive system-wide, host drivers detached |

## 5. Trapped register pages

The only register-level mechanism. A page in a guest I/O window is mapped absent; the access fault goes to the profile's page handler, which decodes the faulting instruction (all data-movement forms compilers and ROMs emit, including `movep`, `bset`/`bclr`, `tst`, `andi.b`/`ori.b` read-modify-write, and long accesses spanning two registers such as `move.l $DFF004,d0` and `move.l a0,$DFF080`), performs it against a register model, and resumes after the instruction.

| Kind | Behaviour |
|---|---|
| **emulate** | a small register model (interrupt controller, timer counters, IDs, ACIA, frame-level display registers) |
| **absorb** | writes kept in a register file (for read-back and frame-level display), reads return the register file or a fixed "idle" value |
| **absent** | bus error reflected to the guest (TOS hardware probes find "no blitter", "no DMA sound") |
| **forward** | passthrough: the access is performed on the real device after an ownership check (§8) |

Rules:

- **Boot phase**: until the machine layer registers "OS ready", unlisted addresses in the profile's probe windows read as open bus and discard writes, so ROM hardware probes complete. After it, they are `absent`. Memory probes must never find RAM there: a page that keeps a write reads as memory (Kickstart sizes slow RAM, fast RAM and A3000/A4000 mainboard RAM by write/read-back), and a fault before exec's vectors are set is a reset loop.
- **Hot sites**: a site trapping more than N times is trap-and-patched to a helper call when the instruction is ≥ 6 bytes (`move.w #imm,abs.l` is 8); others stay trapped. Patches are undone before a guest reset reaches the ROM checksum.
- **Logging**: every absorbed access to a register outside the profile's expected set is counted per site; the counts decide whether a stage needs more emulation.
- Bound: the per-profile register lists in §6.4 and §7.4 are the whole set. Adding a register is a design change.

## 6. Amiga: Kickstart on the replacement layer

Target: the user's **Kickstart 3.2** (A1200 image first, then A4000, A500/A600), booting a full **AmigaOS 3.2** install to Workbench on an RTG surface, on any host.

### 6.1 Kickstart 3.2 resident modules and their fate

Module list, versions, priorities and hardware use per image: [kickstart32-analysis.md](kickstart32-analysis.md) §1.

| Module | Touches | Replacement layer |
|---|---|---|
| exec.library | CIA overlay, `INTENA`/`INTREQ`/`DMACON`, chip RAM sizing, CPU probes, ROM checksum | **keep**; probes answered by pages (§6.4); `Disable`/`Enable` sites (160 inline in the A1200 image) and the dispatcher hot-patched |
| expansion.library | autoconfig at `$E80000`; RAM probes up from `$08000000`, down from `$07F00000` (A3000/A4000); Gary `$DE0000` | keep; autoconfig page reads a constant ("no board"); adds container fast RAM at `$08000000` itself |
| diag init, syscheck, ramdrive.device, syslog, system-startup | board DiagAreas; mouse buttons (Early Startup menu) | keep |
| A3000/A4000 bonus | Ramsey `$DE0043`, `$DE0003` | keep; Ramsey revision reads $7F, so it skips its control-register loop |
| cia.resource | CIA ICR, interrupt levels 2/6 | keep over the emulated CIAs |
| timer.device | CIA timers, VBL | **replace**: `GUEST_TIMER` units (MICROHZ, VBLANK, ECLOCK, WAITUNTIL, WAITECLOCK), `ReadEClock` from host monotonic time; sets ExecBase `ex_EClockFrequency` and `PowerSupplyFrequency` as the ROM's does |
| keyboard.device | CIA-A serial port, level 2 | **replace**: `GUEST_INPUT` raw keys; reset handlers kept (`KBD_ADDRESETHANDLER`) |
| gameport.device | `JOYxDAT`, `POTGOR`, CIA-A fire bits | **replace**: host mouse on unit 0, host joystick/keys on unit 1 |
| input.device, console.device, keymap | none | keep |
| trackdisk.device, disk.resource | Paula disk DMA, CIA-B drive control | **replace** trackdisk by an ADF-backed driver (DF0:–DF3:, `GUEST_BLK`, disk-change); disk.resource: stub owner |
| scsi.device (IDE on A1200/A600/A4000/A4000T, SDMAC on A3000); `NCR scsi.device` (A4000T, 53C710) | Gayle/IDE/SCSI chips | **replace** both names by `GUEST_BLK` units (HDF files): RDB scan, ConfigDev with a DiagArea boot point, `MakeDosNode`/`AddBootNode` (§9.1; analysis §7) |
| card.resource, carddisk.device | Gayle PCMCIA | keep: without a Gayle ID card.resource adds nothing |
| filesystem, FileSystem.resource | none | keep (3.2 FFS); PFS3 etc. from the RDB or `L:` |
| audio.device | Paula audio DMA, level 4 | **replace**: channels mixed and resampled to `GUEST_AUDIO` |
| battclock.resource, battmem.resource | RTC at `$DC0000` | **replace**: host time; battmem backed by a per-container file |
| potgo.resource, misc.resource | `POTGO`; allocation only | keep (absorbed) |
| graphics.library | chip IDs, copper, blitter, VBL | **keep**; see §6.5 |
| layers, intuition, gadtools, workbench, icon, dos, ram-handler, con-handler, shell, ramlib, romboot, strap, bootmenu, utility, math | none directly | keep |
| alert.hook | takes over the display | keep; `containerinit` `SetFunction`s `Alert()` to also report to the host tty |
| serial.device, parallel.device (disk-based) | Paula UART, CIAs | replace by host-backed versions in `DEVS:` via the setup tool, or as resident modules |

Replacements carry the ROM module's name, version 47 and its priority: exec replaces a listed module by a later tag of the same name with a higher version, or equal version and priority ≥, so init order is unchanged.

Added modules: `containerinit` (`RTF_COLDSTART`, priority 106–109, before `diag init`; runs in user mode; a `RTF_SINGLETASK` module outside the ROM never initialises): adds fast RAM on the A500/A600 image (`AddMemList`), maps the vCPU page, installs the VBL/timer interrupt servers, registers OS ready after dos is up. `guestfs-handler`: host-directory filesystem (§9.1).

### 6.2 Injection

```
 launcher                          guest RAM                          Kickstart
 --------                          ---------                          ---------
 load ROM copy at $F80000
 load machine-layer image  ---->   RomTags at $F00000 (first word
                                   not $1111)
 set $0 (not 'LOWM'/'HELP'),
 answer $DE0002 bit 7 = 0
 GIOC_ENTER(ROM reset PC)  ------------------------------------------> exec scans $F80000-$FFFFFF
                                                                       and $F00000-$F7FFFF; our
                                                                       tags win by name + version
```

- **Default: extension area.** Every 3.2 image scans `$F00000–$F7FFFF` for RomTags after its own range; `$E00000` is never scanned. No ExecBase stub, no RAM reservation, survives guest resets. A $1111 first word would instead make the ROM jump to `$F00002` right after its checksum.
- **Fallback: KickTags** (e.g. a ROM that does not scan `$F00000`). The launcher builds what `LoadModule` leaves: `$4` → ExecBase with `ChkBase` = ~ExecBase and words `$22–$52` summing to $FFFF (nothing else is checked), `KickMemPtr`/`KickTagPtr`, `KickCheckSum` as `SumKickData()`. KickMem lies at the top of chip RAM or in `$08000000` fast RAM (added before exec's `AllocAbs`); one failed `AllocAbs` drops all tags.
- **A3000/A4000 power-on rule**: these ROMs clear `$0` and `$4` (forcing a cold start, discarding KickTags) when `$DE0002` bit 7 reads 1 on a 020+. The Gary page answers 0.
- **Fallback: load-time patching** (§1.2.3) for a module that must not initialise and cannot be outranked.
- 3.2 installs booted on 3.1/3.1.4 ROM images run their own `LoadModule` lines and reboot; guest reset keeps the KickTags (§1.1).

### 6.3 Boot sequence

```
 ROM reset -> exec (probes -> pages) -> resident init: containerinit, our devices, graphics,
 intuition ... -> strap: highest-priority boot node (HDF partition / host directory / ADF)
 -> dos -> S:Startup-Sequence (SetPatch, LoadMonDrvs -> P96, IPrefs, LoadWB) -> Workbench
```

### 6.4 Minimal register behaviour

| Page | Emulated | Absorbed | Reads |
|---|---|---|---|
| custom `$DFF000` | `INTENA`/`INTENAR`, `INTREQ`/`INTREQR` (Paula levels 1–6 → virtual lines; VERTB from the virtual VBL; a software `INTREQ` SET write raises its level: `Cause`, blitter queue, timer), `DMACON`/`DMACONR` (BBUSY always 0) | copper, bitplane, sprite, colour, audio, disk, serial registers (register file kept) | `VPOSR` ID bits 14–8: $22/$32 (Alice PAL/NTSC) for A1200/A4000, $20/$30 for A3000; beam position from host time, frozen while `BPLCON0` ERSY is set (no genlock); `DENISEID` $F8 (Lisa) or $FC (ECS); `POTINP` released, `SERDATR` transmit empty |
| CIA-A `$BFE001` (alias page `$BFA000` absorbed), CIA-B `$BFD000` | ICR mask/flags, timers A/B counting from host time at the E-clock rate in one-shot and continuous modes (graphics polls CIA-A TA at init), underflow interrupts from the timer service (§10.6), TOD (A: VBL count, B: line count) | ports (overlay bit ignored: ROM and RAM already mapped; LED; drive control) | fire buttons released, no drive |
| autoconfig `$E80000` | — | writes | any constant ("no board") |
| Gayle `$DA0000`/`$DE1000`, RTC `$DC0000`, Gary/Ramsey `$DE0000` | Gary/Ramsey register file with read-back (`$DE0000/1/3`) | all others | Gayle ID `$DE1000` constant ("no Gayle"), `$DE0002` bit 7 = 0, Ramsey revision `$DE0043` = $7F |

- `BLTSIZE`/`BLTSIZV`/`BLTSIZH` writes are counted, not performed (§6.5).
- Exec's `Disable`/`Enable` write `INTENA` on every call: these ROM sites are the first hot-patch targets.
- Values per image and the hang or reset loop each wrong answer causes: [kickstart32-analysis.md](kickstart32-analysis.md) §2.

### 6.5 graphics.library and RTG

- At init graphics reads the chip IDs, builds the native display database, allocates copper lists and `LoadView`s a blank view; its VBL server runs on the virtual VERTB. The copper and DMA writes are absorbed: the native display is invisible.
- **Workbench on RTG**: P96 (commercial, user-supplied, installed in the user's 3.2 install) with our **P96 board driver** (`container.card`, written against P96's board-driver interface): `FindCard`/`InitCard` report a board whose memory is a `GUEST_SURFACE` window; mode set, palette, panning and sprite functions become surface calls. The setup tool installs the driver and monitor file, sets `ENVARC:Picasso96/DisableAmigaBlitter=Yes` (P96's CPU fallback for blitter work, bitmaps in fast RAM) and the Workbench ScreenMode to the container mode. P96 screen-mode promotion moves native-mode screens onto RTG.
- **Before P96 loads** (early startup, requesters, the 3.2 installer) rendering uses the native display and the blitter. Milestone G6 adds, if its logs show the need: (a) a **frame-level native display**: once per frame, walk the active copper list executing only `MOVE`s into a shadow register file, then convert the bitplanes (OCS/ECS/AGA, EHB, HAM) to the surface; no raster timing; (b) a **synchronous blitter**: a blit runs completely when its size register is written (HRM minterms, shifts, masks, fill, line mode). Both are bounded, memory-to-memory, and off by default.

### 6.6 Compatibility matrix

| Software | Replacement (any host) | Passthrough, shared (AMIX Amiga) | Passthrough, take-over |
|---|---|---|---|
| Shell, dos/exec tools, OS-friendly apps | yes | yes | yes |
| Workbench | yes, on RTG (native display after G6) | yes, native chipset | yes |
| RTG apps (P96/CGX API) | yes | with an RTG board | with an RTG board |
| native-screen OS-friendly apps | promoted to RTG; native after G6 | yes | yes |
| audio.device / AHI apps | host audio | real Paula | real Paula |
| AGA/OCS chip-banging games and demos | no (bus error or silent) | slow, timing-sensitive code fails (writes trapped) | yes |
| trackdisk-level copy protection, raw MFM | no (ADF only) | yes, real floppy | yes |
| direct serial/parallel hardware | no | yes | yes |
| networking (`bsdsocket`) | yes | yes | while interrupts enabled |

3.2 requirements: 2 MB RAM, a 3.1/3.1.4/3.2 ROM. On an 040/060 host CPU the install needs its `68040.library`/`68060.library` (3.2's MMU libraries *(verify)*); their MMU setup is recorded, not applied (§2.2).

## 7. Atari: TOS on the replacement layer

Target: the user's **TOS 2.06** (ST/STE), then **3.06** (TT) and **4.04** (Falcon), booting a TOS/GEM hard-disk install with AUTO and ACC.

### 7.1 Hardware-bound parts of TOS

TOS is one image, not modules; the machine layer attaches at TOS's documented hooks, and the few chips the ROM drives inline get register pages.

| Hardware | TOS use | Replacement |
|---|---|---|
| memory controller `$FF8001`, RAM sizing | cold boot | skipped: warm-boot variables preset (§7.3) |
| MFP 68901 `$FFFA01` | interrupt controller, Timer C 200 Hz, Timers A/B/D, GPIP (ACIA, FDC, monochrome detect), USART | **page**: interrupt controller + timers + GPIP (§7.4); USART absorbed, serial via BIOS device vectors |
| ACIA IKBD `$FFFC00` | keyboard, mouse, joystick, IKBD clock | **page**: host input encoded as IKBD packets; TOS's own keyboard code, `Kbdvbase` hooks and games reading the ACIA work |
| ACIA MIDI `$FFFC04` | MIDI | page: never ready (host MIDI later) |
| Shifter `$FF8200`, TT shifter, Falcon Videl | screen base, resolution, palette | **frame-level surface** from the register file (ST/STE/TT); Falcon modes from `VsetMode` (`FILTER`), Videl writes absorbed |
| FDC WD1772 + DMA `$FF8604` | floppy, ACSI | absorbed, status "not ready"; disks through `hdv_*` and XBIOS `FILTER` (§7.5) |
| TT/Falcon SCSI, Falcon IDE, SCC | disks, serial | absent or absorbed; `hdv_*` driver instead |
| blitter `$FF8A00` | optional | **absent**: TOS's probe finds none, line-A/VDI use the CPU |
| YM2149 `$FF8800` | keyclick, bell, `Dosound`, floppy select, strobe | absorbed (silent); bell via BIOS console to the host |
| STE/TT/Falcon DMA sound, Falcon codec/crossbar | XBIOS sound | absent on 2.06; 4.04: absorbed, XBIOS sound `FILTER`ed to `GUEST_AUDIO` (later) |
| Falcon DSP `$FFA200` | XBIOS `Dsp_*` | "no DSP" answers *(verify 4.04 boot behaviour)* |
| RTC (Mega ST `$FFFC21`, TT/Falcon `$FF8961`) | time, NVRAM | page: register pair over host time and a per-container NVRAM file |
| cartridge port `$FA0000` | application cartridges | **machine-layer image** |

### 7.2 Machine-layer image

An application cartridge (magic `$ABCDEF42`) in the cartridge window. Its entries run where TOS calls cartridges during boot (the `CA_INIT` stage bits, "after GEMDOS init, before disk boot" for the drivers *(verify bit meanings across 2.06/3.06/4.04)*). It installs:

- `hdv_init`/`hdv_bpb`/`hdv_rw`/`hdv_mediach`/`hdv_boot` over `GUEST_BLK`: A:/B: from ST/MSA images, C:… from AHDI/ICD partitions of hard-disk images; `_drvbits`, `_bootdev`, `pun_ptr` (AHDI-compatible), `XHDI` cookie;
- BIOS device vectors (`xconstat`… for AUX, PRN, and console out) → host tty/printer file;
- the guest-side GEMDOS layer for host directories (§7.5);
- OS ready registration, `Alert`-style crash reporting (bombs to the host tty).

The `resvector` hook (`resvalid` = `$31415926`) is available for work needed before cartridge init.

### 7.3 Boot sequence

```
 launcher: ROM copy, cartridge image, sysvars preset (memvalid/memval2/memval3, memctrl,
           phystop, ramtop/ramvalid) -> GIOC_ENTER(ROM reset PC)
 TOS: warm path (no RAM sizing) -> resvector -> MFP/IKBD/screen init (pages) -> GEMDOS
   -> cartridge init: our drivers -> floppy/DMA boot (our hdv_boot, no hardware)
   -> reset-resident $12123456 pages -> AUTO folder of the boot drive -> GEM: ACCs -> desktop
```

Timeouts in TOS's floppy/ACSI probing run on `_hz_200`, which advances in bursts, so absent drives cost little wall time *(verify total)*.

### 7.4 Minimal register behaviour

| Page | Emulated | Absorbed / reads |
|---|---|---|
| MFP | `IERA/B`, `IPRA/B`, `ISRA/B`, `IMRA/B`, `VR` (vector base, end-of-interrupt modes), timers A–D (delay mode from the timer service, §10.6), `GPIP` inputs (ACIA and monochrome-detect lines from the container state) | USART; `AER`/`DDR` kept |
| ACIA IKBD | status/data, receive interrupt through MFP GPIP4; commands answered (reset, mouse modes, clock set/read) | MIDI ACIA never ready |
| video | base, resolution, palette, STE scroll/line-width registers → surface descriptor | Videl timing registers (4.04) |
| RTC | index/data pair | — |
| everything else in the two I/O windows | — | boot phase: open bus; after OS ready: bus error (TOS "bombs") |

All ST I/O is decoded at both aliases (`$00FF8xxx`, `$FFFF8xxx`).

### 7.5 Host directories and floppies

- TOS's GEMDOS knows only FAT, so host directories are a GEMDOS layer in the machine layer: an XBRA-chained TRAP #1 handler that services path and handle calls for host drives with SVR4 calls and passes the rest on. `Pexec` from a host drive is serviced by the layer (create basepage via TOS, load and relocate with host reads). The kernel `FILTER` path (§3.3) is an optional speed-up of the same calls.
- `Floprd`/`Flopwr`/`Flopfmt` go to the drive's image. Raw FDC access (copy protection) needs passthrough.

### 7.6 Compatibility matrix

| Software | Replacement (any host) | Passthrough on TT/ASV, shared | Take-over |
|---|---|---|---|
| GEM desktop and GEM apps | yes | yes | yes |
| TOS/line-A apps writing screen memory in standard modes | yes (frame-level surface) | yes | yes |
| AUTO programs, ACCs, TSRs hooking vectors | yes | yes | yes |
| IKBD-reading games | yes (ACIA page) | yes | yes |
| raster effects, overscan, sync scroll | no | partial (register writes trapped) | yes |
| YM chip music, STE/Falcon DMA sound | silent; XBIOS sound later | real | real |
| FDC copy protection | no | yes | yes |
| blitter-requiring software | no | yes (TT has none) | — |
| DSP (Falcon) | no | no | no |

Passthrough exists only on the TT under ASV, with TOS 3.06; TOS 2.06 and 4.04 always use the replacement layer. TOS 3.06/4.04 on an 040/060 need their MMU and cache instructions emulated (§2.2) and 060 instructions trap-and-patched (`movep` in TOS I/O code among them).

## 8. Hardware passthrough

Only on the matching machine, with a root-granted, exclusive raw-hardware capability, and a ROM image for that model (A3000 Kickstart on an A3000; TOS 3.06 on a TT).

```
 host (AMIX / ASV)                     guest container
 -----------------                     ---------------
 keeps: disk controller, network,      gets: display chips, audio, floppy, keyboard/mouse
        its clock source, memory             ports, serial/parallel as granted
 guestdev: device ownership table,     guest RAM at 0..chip/ST-RAM size = physical RAM
           interrupt demultiplexing          (DMA sees guest addresses)
```

- **Memory**: chip RAM (Amiga) or ST-RAM (Atari) is mapped 1:1 at guest address 0, because the chips' DMA uses those addresses. The host must boot with that RAM reserved *(verify AMIX/ASV use of chip RAM/ST-RAM and their vector base)*. Fast/TT RAM stays container shm.
- **ROM**: the user's image copy at the native address; the machine's physical ROM is not used.
- **Host-owned devices stay replaced**: the guest's `scsi.device` (A3000 SDMAC) or TOS's SCSI/ACSI must not touch the host's disk controller, so disks remain `GUEST_BLK` images even under passthrough.
- **Grades**:
  - *shared*: register pages forwarded (§5): reads direct where a page holds only guest-owned registers, writes trapped and checked. `INTENA` (Amiga) and the MFP (Atari) stay virtual because host interrupts share them (A3000 SCSI and Zorro cards on level 2, host clock). CIA ICR reads clear the flags, so the kernel reads them and keeps a latch for the guest. OS-friendly native-display software runs; blitter-heavy code is slow.
  - *take-over*: custom-chip (or ST I/O) pages mapped directly, guest owns the interrupt enables. Unix stalls whenever the guest masks all interrupts; the host resynchronises its clock from the RTC when the guest exits. For games and demos only, by explicit choice.
- Atari I/O shares pages between devices (`$FF8000` page: video, DMA, YM, sound), so the shared grade forwards per register with an owner table *(verify ASV page size)*.
- Amiga passthrough uses the ROM's own drivers for guest-owned hardware (keyboard.device, trackdisk.device, audio.device); only host-owned ones are replaced.
- Timers: the guest uses the real timers the host does not own. If the ROM's timer.device needs a host-owned one (AMIX's CIA-A timer A), timer.device stays replaced.

## 9. Real installs

### 9.1 Amiga volumes

| Volume | Format | Handled by |
|---|---|---|
| HDF with RDB | `RDSK`/`PART`/`FSHD`/`LSEG` blocks | our `scsi.device` replacement: RDB scan at init, filesystems from `FileSystem.resource` (ROM FFS) or loaded from `LSEG` (PFS3, SFS), `MakeDosNode`/`AddBootNode` with the partition's boot priority |
| partition HDF (no RDB) | one FFS partition, fixed geometry | same driver, one boot node from container configuration |
| ADF | 880 KB/1.76 MB sector image | trackdisk replacement; disk change from the launcher or a host command |
| host directory | Unix directory + sidecars | `guestfs-handler`: DOS packets over SVR4 calls and `GUEST_META`; can be a boot node |

Filesystems on images are the install's own (3.2 FFS with long names, PFS3 from the RDB): the container reads blocks only.

### 9.2 Atari volumes

Hard-disk images with AHDI/ICD root-sector partition tables (GEM/BGM/XGM), FAT12/16 partitions; ST/MSA floppy images; host directories. The image's own hard-disk driver (boot-sector or AUTO) is not run: it would probe absent hardware; our `hdv_*` driver serves the partitions and exports `XHDI`.

### 9.3 Setup tools (our userland, native)

| Tool | Does |
|---|---|
| `guestmk hdf` | create an HDF with an RDB, partitions, DosTypes (`DOS\7` for 3.2 long names), empty FFS root/bitmap |
| `guestmk ahdi` | create an Atari hard-disk image with AHDI/ICD table and FAT16 partitions |
| `guestmk adf`, `st` | blank floppy images |
| `guestcfg` | container descriptor: ROM file, volumes, boot order, RAM, display, capabilities; checks the ROM hash |
| `guestp96` | copy our P96 driver and monitor file into an install, set the environment variables and ScreenMode |

Install routes: (1) boot an **existing** 3.2 or TOS install (HDF or image from another machine or emulator): first milestone. (2) Run the **official 3.2 installer** from the user's ADFs in DF0: onto a new HDF: needs the native display (G6). (3) TOS: copy AUTO/ACC/desktop files into a partition or host directory (TOS has no installer).

### 9.4 Launching one program

`tosexec`/`amigaexec` redirect PRG/hunk files to `/usr/lib/guest/<profile>/run`, which boots the configured install and hands the program to the machine layer at OS ready (Amiga: `SystemTags` from a task of `containerinit`; TOS: a generated `Pexec` after GEM init *(design)*). Exit status returns through `NF_SHUTDOWN`.

## 10. Concurrent guests

Mac, TOS and Amiga containers can run at once.

### 10.1 Address spaces

Each container is its own process (or process group, Mac) with its own page tables. Mac low memory, TOS system variables and Amiga `AbsExecBase` all sit at guest address 0 of different address spaces, so they never collide. Shared in the kernel: the host vector table (gates dispatch by the current process's profile), `guestsvc`, the screen, input devices.

### 10.2 Memory

Guest RAM is pageable shm; only vCPU pages, surfaces in use and passthrough RAM are locked. Budgets come from each descriptor and are checked against physical memory at create time. Example on a 128 MB Q800:

| Consumer | MB |
|---|---|
| host kernel and daemons | 16 |
| Mac (A/UX layout) | 32 |
| TOS: 14 ST-RAM + 16 TT-RAM | 30 |
| Amiga: 2 chip + 32 fast + 4 RTG surface | 38 |
| free for Unix | 12 |

### 10.3 Display

- **Default: full-screen switching** like virtual consoles: one container owns the frame buffer; a host-reserved hotkey (taken by `guestsvc` before routing) cycles owners. Switch-out copies the screen into the container's backing pages and remaps its surface to them; switch-in copies back and remaps. A hidden converted surface is not converted: background guests cost no display work.
- **Optional windowed mode** under a host window system: a user-space helper per container converts surfaces to the window depth (planar→chunky, palette, 1 bpp dither). Cost is per written page per refresh; on an 030 expect only small or low-depth guest screens to be usable.
- A passthrough guest's display is the chipset: switching away saves and stops its display DMA (shared grade); take-over has no switch.

### 10.4 Input focus

Keyboard and mouse go to the container owning the screen (windowed: the focused window). The hotkey never reaches a guest. Unfocused guests see no input; their timers keep running.

### 10.5 CPU sharing

Idle guests must block in the host, never spin:

| Guest | Idle path | Host action |
|---|---|---|
| Mac | `WaitNextEvent` with sleep | block in uinter until an event or the sleep expires (A/UX behaviour *(verify)*) |
| Amiga Kickstart | exec with no ready task | `stop #$2000` in exec's dispatcher → host sleep until a virtual interrupt |
| TOS | AES/BIOS polling loops (`Bconstat`, `evnt_multi` idle) | a polling detector: the same no-input BIOS/XBIOS call or ACIA status read repeated with nothing pending → sleep until the next virtual interrupt |
| EmuTOS, AROS | our builds | `stop` |

Guests that busy-wait anyway (games) get ordinary Unix scheduling; `nice` applies.

### 10.6 Timer service

Kernel `HZ` stays 60. A small platform module, `hrtimer`, programs one spare hardware timer as a one-shot for the earliest pending guest deadline of all containers:

| Host | Timer | Clock | Resolution / longest shot | Host's own use |
|---|---|---|---|---|
| Mac (Q800) | VIA1 T2, IPL 1 | 783.36 kHz | 1.28 µs / 83.7 ms | port tick on VIA1 T1; A/UX leaves T2 unused; VIA2 T1 runs A/UX's square wave, VIA2 T2 spare *(verify ADB shift-register mode needs no T2)* |
| Amiga (AMIX) | CIA-A timer B, IPL 2 | E-clock 709.379 kHz PAL, 715.909 kHz NTSC | 1.4 µs / 92 ms | AMIX tick on CIA-A timer A; CIA-B is IPL 6, above `splhi` *(verify AMIX's CIA-B timer use)* |
| Atari (ASV, TT) | spare MFP timer A or B (D clocks the ST MFP USART) | 2.4576 MHz ÷ 4…200 | 1.6 µs / 20.8 ms at ÷200 | ASV's use of both MFPs to check; MFP is IPL 6, above `splhi`, so the handler only latches and defers to IPL ≤ 4 |

- **Queue**: deadlines {container, virtual line or vector, absolute time}, sorted. Deadlines within 100 µs coalesce *(tune)*; a per-container minimum interval (e.g. 250 µs) caps a runaway guest. Longer waits chain shots or ride the 60 Hz callout.
- **Expiry**: pop due entries, raise their virtual lines (set `vPEND`, post the carrier or Mac signal), program the next shot.
- **Latency**: guest processes run in the SVR4 real-time class (`priocntl`, RT), so a raised line preempts time-sharing work at the next preemption point; the bound is the kernel's longest non-preemptible path (measure). RT is a capability (root or group grant); others run time-sharing. AMIX 2.1 ships both (`rt` in the link kit's `master.d`, `/usr/bin/priocntl`, `RTpriocntl`).
- **Guest sources**:

| Source | Deadline |
|---|---|
| Amiga `timer.device` replacement: MICROHZ, ECLOCK, WAITUNTIL, WAITECLOCK | request time; `ReadEClock` from the host's free-running count |
| Amiga VBLANK unit, virtual VERTB | exact 50/60 Hz frame period |
| Amiga CIA timers on the emulated page (CIA-timed music players via cia.resource) | underflow time from latch and E-clock |
| TOS MFP Timer C (200 Hz), timers A/B/D in delay mode | period from prescaler and data register |
| Mac Time Manager | A/UX interval timers used by `Patch.067C` *(verify the path)*; the 60 Hz SIGIOT tick stays on the callout |

- **Cost per expiry** on a 25 MHz 040 (estimate, measured in G9): interrupt and queue ≈ 10 µs, carrier signal plus RT preemption and context switch ≈ 60–100 µs, frame build ≈ 15 µs; ≈ 100–130 µs total: ≈ 2.5 % CPU for TOS's 200 Hz, ≈ 0.6 % per 50 Hz VBL.
- **Passthrough** guests use real timers (§8).
- **Fallback** without a spare timer: 60 Hz callouts, rates above 60 Hz as bursts (Timer C 3–4 per tick), below as spreads (PAL VBL 5 per 6); counts exact, intervals not.

### 10.7 Passthrough with other guests

At most one container holds any raw-hardware range system-wide, and only on matching hardware; others run on the replacement layer at the same time (an A3000 running a passthrough Kickstart 3.2 next to a Mac and a TOS container). Replacement guests then need a display the passthrough guest does not own (an RTG board, or windowed mode on a remote X server), or switch in only while the passthrough guest's display is stopped.

## 11. Mac

Unchanged from the A/UX design: `startmac` on `guestcore` (A-line reflect, `vS` pinned, single stack, trap #0/#15 → `auxcore`, signal-mode interrupts), fake NuBus card and ADB on `guestsvc`, optional kernel-serviced File Manager traps and `$7301` Mac-side drivers. The generalisation copies its shape: user ROM (private copy on non-Macs, restored on reset), a machine layer injected into the OS's own structures (`Patch.067C` into trap tables; our modules into Exec's resident list; our drivers into TOS's vectors), host devices behind it.

## 12. Free fallback: EmuTOS and AROS

For users without ROMs, after the ROM milestones:

- **EmuTOS** (GPL-2.0-or-later) "container" machine target: console/drives/XBIOS screen/IKBD/timers over host services directly, paravirtual SR writes, NatFeats debug and shutdown. No register pages needed.
- **AROS m68k** (APL 1.1) "container" target: Exec in-process, `timer.device` on virtual timers, host-directory handler, graphics HIDD over a surface, input HIDDs, `bsdsocket` thunks, paravirtual `Disable()`/`Enable()`. Its `INTENA` page is the Amiga custom page of §6.4.
- Each boots the same volumes and uses the same setup tools; they replace the ROM, not the install.

## 13. Kernel pieces

### 13.1 Static part

The A/UX shims ([aux-kernel-design.md](aux-kernel-design.md) §1/§9.2) renamed `guest_*`: vector gates (2–11, 32–47, 48–55, 60/61, gate tail `jmp ureturn`), `ev_*`, `sendsig` wrapper, `fsig` override, `valid_usr_range` and `usrxmemflt` wrappers (the latter carries trapped-page faults), reflection fast path in assembly.

### 13.2 Modules

| Module | Type | Depends | Contents |
|---|---|---|---|
| `guestcore` | `MOD_HOOK_WRAPPER` | — | proc/container, dispositions, reflection, `guest_priv`, vIPL, vector-mode frames, trapped-page framework and instruction decoder, trap-and-patch and ROM restore, sentinels, NatFeats |
| `guestdev` | `MOD_DRV_WRAPPER` (`/dev/guest`) | `guestcore` | container create/configure (RAM, ROM copy, images, windows, vCPU page, capabilities), `GIOC_ENTER`, raw-hardware grants and device ownership |
| `hrtimer` | `MOD_MISC_WRAPPER` | — | deadline queue on a spare VIA/CIA/MFP timer (platform backend) |
| `guestsvc` | `MOD_MISC_WRAPPER` + `MOD_TY_SYS` | `guestcore`, `hrtimer` | timers, surfaces, screen switching, input routing, block images, metadata, sockets, audio |
| `auxcore`, `auxexec`, `uinter` | as in [aux-kernel-design.md](aux-kernel-design.md) | | Mac |
| `amigaguest` | `MOD_MISC_WRAPPER` | `guestcore`, `guestsvc` | Amiga dispositions, custom/CIA/probe page models, thunk tables, frame-level native display and blitter (optional) |
| `amigaexec` | `MOD_EXEC_WRAPPER` | `amigaguest` | hunk header check, redirect |
| `tosguest` | `MOD_MISC_WRAPPER` | `guestcore`, `guestsvc` | TOS dispositions, MFP/ACIA/video/RTC page models, XBIOS filters, polling detector, optional GEMDOS fast path |
| `tosexec` | `MOD_EXEC_WRAPPER` (0x601A) | `tosguest` | PRG header check, redirect |

Guest-side (not kernel): Amiga machine-layer image (resident modules, `guestfs-handler`, P96 board driver, serial/parallel), TOS cartridge image.

Hunk magic: register 0x0000 with `EXF_FIRST` in `gexec`, check the full long *(verify `gexec` magic handling)*.

### 13.3 Performance targets

Slowest target: 030/25 MHz; reference 040/25 MHz.

| Path | Target |
|---|---|
| native syscall overhead | ≤ 5 instructions |
| A-line reflect (Mac) | ≤ 60 instructions |
| TRAP reflect + guest `rte` | ≤ 2× A-line |
| trapped register access (decode + model) | ≤ 2× A-line; hot sites patched to 0 exceptions |
| SR op in guest supervisor | ≤ 1 null syscall; 0 in paravirtual code |
| NatFeats / thunk call | ≤ 1 null syscall |
| virtual interrupt delivery | ≤ 100 µs on 030/25 |
| timer expiry to guest handler | ≈ 100–130 µs on 040/25 (§10.6) |
| surface | 0 copies when formats match; ≤ 50 Hz on written pages |

## 14. Licensing

| Component | Licence | Distribution |
|---|---|---|
| user ROMs (Mac, Kickstart, TOS), AmigaOS 3.2, TOS installs, A/UX, P96, NVDI | proprietary | user-supplied only; never bundled, never modified on disk |
| in-memory ROM adaptation | patch lists hold offsets, expected bytes and our replacement code, keyed by hash; no ROM content | shipped with the launcher |
| our kernel modules, launchers, headers | ours; headers MIT/BSD | yes |
| our Amiga resident modules, `guestfs-handler`, P96 board driver, TOS cartridge image | ours: permissive (MIT/BSD) proposed so users and other projects can combine them freely; written from public manuals and SDK headers, no EmuTOS/AROS code (keeps them licence-clean) | yes, separate images |
| P96 board-driver interface headers | P96 SDK terms *(check before shipping the driver source)* | — |
| EmuTOS, AROS container builds | GPL-2.0+, APL 1.1; separate images, source provided; never combined with each other | yes |
| AMIX/ASV kernel objects | proprietary, local | unchanged |

## 15. Staged plan

Static pass criteria per milestone (builds, image checks, host harnesses in the style of `kernel/mac/integrate/verify.sh`); runtime in QEMU `q800` (040) unless stated. The user's ROMs and installs are test inputs kept outside the repository.

| # | Milestone | Needs | Static pass | Runtime pass |
|---|---|---|---|---|
| G0 | Interfaces frozen | this document; aux-kernel-design names → `guest_*` | `hooksw` names; disposition, register-page and ROM-descriptor headers compile | — |
| G1 | `guestcore` + test profile `t68` | DLM loader with driver, exec and syscall/hook modules; gates; `guestdev` | `guest_priv` decoder harness; frame builder formats 0/2; **register-page decoder harness** over every store/load form incl. `movep`, RMW | raw 68k binary: `trap #3` → host `write`, `NF_STDERR`, 60 Hz virtual timer, `stop` idles; a trapped page counts accesses; a hot site is patched and restored on reset |
| G2 | A/UX `startmac` on `guestcore` | G1; A/UX M0–M4 ([aux-kernel-design.md](aux-kernel-design.md)) | A/UX static criteria | A/UX M3, M4 |
| G3 | Kickstart 3.2 → AmigaDOS | G1; `amigaguest`, custom/CIA/probe pages, `$F00000` RomTag image (KickTag builder as fallback), `containerinit`, timer, battclock, `scsi.device` over HDF, host-tty serial | `$F00000` image and ExecBase/KickTag builder checked against [kickstart32-analysis.md](kickstart32-analysis.md) §3; RDB parser harness; ROM hash table | user's A1200 3.2 ROM boots the user's 3.2 HDF; a Shell on `AUX:` at the host tty lists and writes files; absorbed-access log shows no unexpected registers |
| G4 | 3.2 Workbench on RTG | G3; surfaces, P96 board driver, keyboard/gameport replacements, VBL, `guestp96` | board-driver function table vs P96 interface; input mapping tables | Workbench on the P96 container screen (converted to the Q800 frame buffer), mouse and keyboard, a Shell window, Workbench icons from the HDF |
| G5 | Amiga host services | G4; `guestfs-handler`, ADF trackdisk, `audio.device`, parallel, `bsdsocket` thunks (`GUEST_SOCK`, shared with A/UX M6) | DOS packet table; thunk generator (`bsdsocket` LVOs) | boot from a host directory; ADF read/write; a sample plays; a TCP client reaches a host |
| G6 | Amiga native display, installer | G4; frame-level native display; synchronous blitter if G4/G5 logs require it; `guestmk hdf` | copper-walk and planar conversion harness; blitter harness vs HRM examples | official 3.2 installer from the user's ADFs installs onto a new HDF, which then boots to Workbench |
| G7 | TOS 2.06 → GEM desktop | G1; `tosguest`, `tosexec`, MFP/ACIA/video/RTC pages, cartridge image, `hdv_*` over AHDI images, GEMDOS host layer, `guestmk ahdi` | PRG header harness; AHDI/ICD parser; IKBD encoder tables; sysvar preset checker | user's TOS 2.06 boots an AHDI image in ST-high on the Q800 (zero-copy): AUTO runs, an ACC loads, desktop opens, a GEM program runs from a host directory |
| G8 | TOS 3.06 and 4.04 | G7; MMU/cache emulation, Videl via `VsetMode`, XBIOS sound filter, RTG VDI (NVDI or fVDI driver on a surface) | per-image patch/filter lists | TT and Falcon desktops in a TT/Falcon mode and on an RTG VDI |
| G9 | Three guests at once | G2, G4, G7; `hrtimer` (VIA1 T2), RT class, screen switching, focus, polling detector, budgets | budget checker; hotkey not routable to guests; deadline-queue harness (ordering, coalescing, chaining, rate cap) | Mac Finder, TOS desktop and Amiga Workbench run concurrently in QEMU `q800`; hotkey switches screens with contents intact; all three idle together use < 10 % CPU; **timer accuracy**: with all three running, guest test programs measure 1 ms MICROHZ requests, TOS Timer C and a 50 Hz VBL against host time for 10 min: period error < 1 %, 99th-percentile lateness < 2 ms *(provisional; QEMU timing noise)*; expiry cost counted in instructions |
| G10 | Amiga passthrough | AMIX on A3000; raw-hardware grants, ownership table, chip RAM reservation | grant exclusivity harness; forward table | shared grade: 3.2 Workbench on the native display with Unix alongside; take-over grade: a chipset demo runs and Unix resumes after |
| G11 | Atari passthrough | ASV on TT | same | TOS 3.06 desktop on the TT's own video; a YM/DMA-sound program plays |
| G12 | Free fallback | G3/G7 infrastructure; EmuTOS and AROS container targets | EmuTOS/AROS target builds | the G4/G7 pass tests without user ROMs |
| G13 | 030 and 060 | Amiga platform kernel; generic 060 trap-and-patch | helper page per profile | exception counts per second on 030 and 060 hosts |

Dependencies:

- DLM driver, exec and syscall/hook module support before G1; without it G1 links statically.
- Every milestone after G1 builds on `guestcore`; A/UX work (G2) uses the `guest_*` shims from the start.
- Mac host prerequisites: user exec, ITT0/DTT0 (in `pstartmac.s`), user-mappable frame buffer, raw ADB event path, SONIC for G5 networking.

## 16. Open questions and risks

**Open questions**

Kickstart 3.2 reset path, RomTag scan ranges, probe values and idle loop: [kickstart32-analysis.md](kickstart32-analysis.md).

1. `CA_INIT` stage bits and cartridge call order in TOS 2.06/3.06/4.04; TOS 4.04's DSP and codec initialisation without the chips.
2. Carrier signal number for vector-mode delivery; launcher link address; HAT acceptance of segments above `UVEND`; `gexec` hunk magic.
3. AMIX/ASV: chip RAM/ST-RAM and vector-base use, page size (for passthrough); AMIX's CIA-B timer use; ASV's MFP timer use.
4. The longest non-preemptible kernel path (RT latency bound); QEMU `q800` VIA T2 one-shot accuracy.
5. Windowed display backend owner (X server helper vs kernel).
6. P96 SDK licence for publishing our board driver's source.

**Risks**

1. **Legal: in-memory ROM adaptation.** Hot-site and load-time patches alter the user's ROM image in RAM. Kept to per-session memory of a lawfully owned copy for interoperability, never saved or shipped; the user's ROM licence terms may still restrict it (e.g. emulator-only ROM files). Default injection changes no ROM bytes.
2. **Kickstart early init.** A wrong probe answer is a reset loop or a silent hang. The 3.2 probes and failure modes are listed in [kickstart32-analysis.md](kickstart32-analysis.md) §2; other ROM versions need the same check.
3. **Exception cost on unmodified ROM code**: SR writes, `rte`, trapped `INTENA` writes on every `Disable`/`Enable`. Hot-site patching is required for usable speed on 030/040; sites shorter than 6 bytes stay trapped.
4. **Timing**: accuracy depends on a spare hardware timer and RT preemption; without them guests fall back to 60 Hz bursts. Beam-counter busy-waits stay approximate. Many short guest timers across containers cost ≈ 100 µs each on an 040/25.
5. **3.2's own requirements**: SetPatch and MMU libraries on 040/060 expect a real MMU; recorded-only MMU state may not satisfy them.
6. **Native display dependence**: early boot, requesters and the installer are invisible until G6; a requester there blocks boot silently (the absorbed log shows it).
7. **Single process, many Exec tasks**: blocking host calls must be asynchronous in every replacement module.
8. **Instruction decoder** for trapped pages must cover compiler and ROM forms; 040/060 fault frames differ and the 060 lacks store data.
9. **030 logical caches** with shared pages: cache-inhibit on 030, pushes on 040/060 copyback.
10. **Passthrough stalls**: take-over grade freezes Unix while the guest masks interrupts; shared grade is slow for chip-banging code.
11. **Scope creep toward emulation**: every register beyond §6.4/§7.4 is a design change; the frame-level display and synchronous blitter are the ceiling.
12. **Licence mixing** if EmuTOS/AROS code enters our permissive machine-layer images or kernel modules.
