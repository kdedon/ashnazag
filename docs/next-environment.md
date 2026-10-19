# NeXT environment

Design for `startnext`: the user's installed NeXTSTEP / OPENSTEP runs natively on a
68040/060 host, unmodified. No SCSI, ethernet, DMA or desktop-bus chip is modelled:
those devices reach Mach through our own drivers that call host services. Status:
reader (`tools/nextcd.py`), kernel load and entry (`native -k`), root over host folders, login window,
Workspace and sound done; no board register is modelled (§2).

## Shape

```
 startnext (native launcher, user's rights): env dir, RAM size, optional ROM
   |  acts as the boot program: loads mach_kernel and our drivers, as A/UX's launch does
 guest process: Mach kernel, our drivers, apps, all native 68k in user mode
   |  privileged insns, page faults        |  host call: store to the doorbell
   v                                       v
 guestcore: virtual S mode, vIPL,        nextguest DLM: host services for the kernel's
 shadow 040 MMU (S and U spaces)           patched timer, clock, clock-chip and soft-interrupt
                                           routines; interrupt requests; VRAM -> display surface
                                         startnext (host side): the host-call broker
                                           files, input, sound; network later
```

Same split as the Amiga environment: guestcore runs the CPU side; devices are guest-side
drivers over a mailbox page plus a doorbell write (as `hostfswire.h`), served by a
broker with the user's rights.

## 1. Mach under guestcore

Today guestcore gives Kickstart, the Mac ROM and AMS a virtual SR (S, M, IPL in
`gp_vsr`), emulates `ori/andi/eori SR`, `move SR`, `rte` (formats 0/2/3/9/A/B), `movec`
(SFC, DFC, CACR, USP, VBR; MMU registers read as 0), `moves`, `cinv/cpush` (push all),
`fsave/frestore`, `stop`, and 030 `pmove` as no-ops. Guest MMU state is recorded, never
applied: guests must be identity-mapped. Access faults reflect to vVBR+8 as format 7.

Mach needs real translation: separate supervisor (SRP) and user (URP) spaces, 8 KB
pages, TT registers, per-task URP switches. Kernel and user VAs overlap (user shared
libraries sit where kernel RAM is), so one host space cannot hold both. Missing:

| Piece | Design |
|---|---|
| Shadow MMU | two host address spaces per guest: Gs (SRP + ITT/DTT) and Gu (current URP, small cache of recent roots). Filled lazily at fault by walking the guest's 040 tables in guest RAM; an 8 KB guest page is two host pages |
| Invalidation | `pflush`/`pflusha`, URP/SRP/TC writes drop shadow entries, as an ATC would |
| U and M bits | set in the guest descriptor at fill; pages map read-only until the first write, which sets M. Mach's pmap reads M |
| Space switch | on vS change (exception into guest, `rte` to user) swap Gs/Gu: root reload + ATC flush. May need a small static hook to swap `p_as`; prefer guestcore |
| MMU off | TC.E=0: identity map of the board. The kernel entry runs here |
| `movec` | add URP, SRP, TC, ITT0/1, DTT0/1, MMUSR as live state; `ptest` fills MMUSR from a table walk |
| Interrupts | vector mode: levels 1-7 from the interrupt status/mask registers, autovectored through vVBR |
| FPU | 040 host: unimplemented-FP traps go to the guest's own handlers. 060 host: host FPSP/ISP for 060-only gaps, the rest reflected |

What traps, and cost (a gate round trip is a few µs on a 25 MHz 040; measure):

- every privileged instruction; `spl` (moves to and from SR) runs thousands of times a second;
- `moves` in copyin/copyout, per element;
- shadow fills, first writes (M bit), every remaining I/O register access;
- each guest syscall or trap: host gate, reflect, Gs switch, `rte` back, Gu switch.

Mitigations, all generic (no hooks keyed to an OS build): hot-site trap-and-patch from
guest-container §5 for `move SR` sites; a `moves` run-ahead that emulates the copy loop
in one trap; a shadow cache keyed by URP so task switches keep their mappings.

## 2. Board and devices

Target model: NeXTstation Turbo. Guest-physical map (all quadrant 0):

| Range | Part | Host side |
|---|---|---|
| 0x00000000, 0x01000000 | ROM 128 KB (alias), if used | user's file, read-only pages |
| 0x02000000-0x0201FFFF, 0x02200000, 0x03E00000 | system registers | the pages the kernel uses are memory holding the ids; device pages absent (bus error) |
| 0x04000000 | RAM, up to 128 MB | guest RAM object |
| 0x0C000000 | VRAM | surface pages |
| host-call page | mailbox + doorbell | broker |

No register is modelled and no guest instruction is decoded. The I/O pages the kernel
itself uses (interrupts, SCR1/2, keyboard unit, brightness, timer, event counter, the
turbo's memory controller and cache) are memory: they hold the constants it reads (SCR1
and the turbo's id, SCR2 at power-on, the screen's size: the host screen's) and take its writes with no
effect. Device pages are absent. The kernel's routines that
drive the board are patched in the loaded copy, found by symbol, to make host calls
(`HF_HCALL`: `illegal` at a fixed address in the host-call region, service in d0, C
arguments on the stack):

| Kernel routine | Host service |
|---|---|
| event counter read in `clock_value`, `kern_timestamp` | host µs (20 bits; the kernel extends it) |
| `delay` | host wait |
| `_set_timer` | next timer interrupt, one-shot: a host timeout at the requested delay |
| timer handler (installed by `clock_timer_init`) | acknowledge, host µs |
| status read in the level 6 dispatcher (`ipl6`) | pending interrupt status |
| `callout_dispatch` setting SCR2 soft bits | software interrupt 1 or 2 request |
| `rtc_get`, `rtc_set`, `startrtclock` | host time; setting and start ignored |
| `nvram_check`, `nvram_set` | `~/NeXT/<env>.nvram` |
| `rtc_power_down`, monitor trap | the machine ends |
| `km_send`, `adb_initialize`, `mon_reset` | absent units: time out, return |
| drivers `fc`, `sc`, `oc`, `np`, `sound`, `dsp`, `en` (probes, controller setup); `zsintsetup`, `enintsetup` | absent: the probes find nothing |

Interrupt requests are module state (timer, soft 1 and 2), delivered through vIPL; a
taken timer interrupt stays in the dispatcher's status until the handler acknowledges it.
An access to an address nothing backs is the guest's bus error, counted; `t_next` asserts
none in steady state. VRAM: write-protect tracking, 2 bpp to 8-bit CLUT or 4444 to host 16/32, ≤50 Hz.

Devices as guest drivers (host call = request in the mailbox, doorbell store, reply):

| Device | Driver | Host service |
|---|---|---|
| Storage | `HostDisk` block driver | host file (disk file option) |
| Root and other filesystems | host filesystem (§3), vnode operations | the environment folder `~/NeXT/<env>` |
| Network (later) | `en0` driver, one host call per frame | host sockets; never used for files |
| Display | `HostFB` frame buffer driver over VRAM pages | display service surface |
| Keyboard, mouse | `HostEvent` event source | display service `/dev/kbd`, `/dev/mouse` |
| Sound | `HostSound` | sndout |
| Console | `HostCon` kernel console | log file or display |

NeXTdimension, DSP, printer, floppy and the non-turbo MO drive are out of scope.

## 3. Drivers in Mach

How the drivers get in. NeXTSTEP boots as: boot program loads `mach_kernel` and the
boot-time driver bundles (`/private/Drivers/<arch>/*.config`) into memory, then starts
the kernel, which initialises them before the root mounts. This is how a driver
reaches the kernel before any disk exists. So:

- `startnext` is the boot program. It loads the user's `mach_kernel` from
  `~/NeXT/<env>/`, the boot-time drivers, and the boot arguments the kernel expects
  (memory map, root device, console). The user's kernel is not modified.
- Drivers are DriverKit drivers (the OPENSTEP 4.2 and NeXTSTEP 3.3 driver model) in
  `.config` bundles, kept in the environment folder with the rest. `HostCon`
  (and `HostDisk`) load at boot, ahead of the root. Later drivers load
  through the normal kernel loader.
- Host call: a store to the doorbell page after filling the mailbox. No trap vector or
  instruction is used, so nothing depends on the kernel's own trap use.
- Fallbacks if the boot program cannot be replaced: `startnext` loads the ROM's own
  boot path, or links the driver set into a copy of the kernel with the user's tools
  (cctools `ld` on the Developer CD, run in the guest).

Root. The root is the user's folder `~/NeXT/<env>`, served by our own host filesystem
inside the kernel; there is no NFS, bootp, ethernet or daemon, since a file server over
packets is emulation one layer up. `startnext` loads the kernel's Mach-O, finds the
filesystem switch by symbol name, installs the host filesystem's vfs and vnode operations
as the operations of a free switch entry in the loaded copy (the kernel file is never
modified), and replaces the kernel's root-device setup with a store of that entry's name
as the root type, so the kernel mounts it as `/` with no device probe. Each operation (lookup, getattr, setattr, read, write, readdir, create, remove,
rename, mkdir, rmdir, symlink, readlink, link, fsync, inactive, statfs) is one host call
on the folder, as the Amiga handler's one-request-per-operation design
(`hostfswire.h`) at the vnode level. Files stay host files; NeXT owners, modes and device
nodes live in the `~/NeXT/<env>.meta` sidecar that `makenext` writes, and the host
appends what the guest changes. Symlinks are the guest's: the host follows none, and a
path through a host link (a directory the guest removed and made a link) fails. `HostDisk` over a host disk file holding a NeXT UFS stays
an option, not the default; it also gives local `/private` and swap.

The kernel stays as the user installed it. Throughput is bounded by the mailbox
(8 KB per read or write), not by an emulated chip. `~` and `/` of the host appear under
`/Net` only when the network feature exists.

## 4. Install

`makenext` copies the user's OPENSTEP 4.2 User CD into `~/NeXT/<env>` without
running the installer. The CD (`Openstep-4.2-Intel-User.iso`, 2048-byte sectors) is
multi-architecture. It carries a NeXT disk label (`dlV3`, at byte 8192; it names
`mach_kernel` as the boot file) and a 4.3BSD-style UFS (big-endian, 8 KB blocks, 2 KB
fragments, magic 0x011954, first filesystem at byte 163840). Its root lists `mach_kernel`,
`NextCD`, `NextLibrary`, `NextApps`, `NextAdmin`, `NextDeveloper`, `private` and `usr`.

Steps, all on the host:

1. Read the label and UFS (a small reader in the user-space tools; no mount).
2. Copy the tree, keeping the m68k slice of fat binaries only if the user asks to save
   space; map owners, modes and device nodes to sidecars.
3. Unpack the CD's package archives the installer would, and apply what its
   post-install scripts do to files (NetInfo data, `hostconfig`, device nodes). Scripts
   that need the target's own tools wait for first boot, run by the guest.
4. Add our driver bundles and the boot arguments file.

Open: whether the CD's tree is already the installed layout or needs package
unpacking is checked first by the reader. A NeXTSTEP 3.3 CD or an existing disk dump
copies the same way (disk dump: its UFS is read directly).

## 5. Toolchain

Building NeXT drivers needs a NeXT compiler, but NeXTSTEP cannot run before the first
driver. No ready cross compiler exists:

- FSF GCC 2.x had an `m68k-next-nextstep3` target; it emits assembler for the NeXT
  `as` and needs Mach-O tools (cctools: `as`, `ld`, `libtool`), which FSF binutils
  lack. Apple's cctools sources have Linux ports but the m68k backends left in
  later releases.
- The Developer CD (`Openstep-4.2-Intel-Developer.iso`, in `OPENSTEP 4.2
  Enterprise.7z`, read with the same reader) supplies the kernel and DriverKit
  headers and the m68k slices of `cc`, `as`, `ld`; these run only on NeXTSTEP.

Path:

1. Bootstrap driver, host-built: write `HostCon` (+ `HostDisk`) freestanding
   (K&R C, no libc), compile with our m68k gcc against the headers from the Developer
   CD, and convert the ELF object to a Mach-O relocatable with a small `elf2macho`
   (in the style of `coff2elf`). The driver calls a handful of kernel entry points
   (`printf`, `kalloc`, device registration, interrupt hookup), resolved against the
   user's `mach_kernel` symbols by `startnext` at load.
2. With the root up, NeXTSTEP runs the Developer tools natively; later drivers
   (`HostFB`, `HostEvent`, `HostSound`) build in the guest, and the bootstrap
   driver is rebuilt there. Self-hosting afterwards.

The ABI points to confirm against the headers: structure return, argument and
return-register conventions, alignment, and the DriverKit class layout. DriverKit
classes (Objective-C) need either the NeXT Objective-C runtime and compiler in the
guest, or a C-level driver with the kernel's `devsw` entries; the bootstrap driver
uses the C level.

## 6. Media

| Item | Source | Format |
|---|---|---|
| Boot ROM | user, optional | 128 KB raw; not needed on the driver path |
| NeXTSTEP 3.3 user CD (68k or multi-architecture) | user | raw 2048-byte-sector image: `.iso`, `.cdr`, `.toast`, `.img` |
| OPENSTEP 4.2 User CD | user | same; the first install source |
| OPENSTEP 4.2 Developer CD | user | same; headers and tools for drivers |
| Installed NeXT disk dump | user, optional | raw image |

No hashes are checked; the launcher only checks sizes and formats.

## 7. Risks

1. Trap rate in the Mach kernel (`spl`, `moves`, syscalls) decides usability.
2. Shadow MMU and the Gs/Gu switch touch the host HAT; a static hook may be needed.
3. Boot arguments: the kernel's expectations from the boot program (memory map, root
   from the network, console) are not documented; they are checked against the kernel
   in the first milestone. If the kernel needs the ROM's globals, the ROM runs as a
   library with absent-device answers.
4. The first driver is written without NeXT's compiler: ABI and header mismatches
   cost time until the guest builds its own.
5. Host filesystem: operations the kernel's virtual memory uses for programs (page-in)
   and the vnode layout are the kernel's own; checked against its symbols at load.
6. Screen size: the host's, at least 832x624 (the kernel's console window; a smaller host shows the top left), up to 1 MB of 2-bit VRAM; the window server's own size table
   still names 1120x832.
7. 060 hosts: FP and integer gaps under a guest that expects an 040.
8. Licence: register facts come from public sources; no emulator code is used.

## 8. Milestones

0. Reader: list and copy the CD's UFS tree on the host; check the boot-driver
   directories and `mach_kernel` slices.
1. Kernel and console (reached on the identity map: the kernel prints its version line,
   memory and probe lines, "no suitable root", and returns to the monitor): `startnext` loads `mach_kernel` and `HostCon` (bootstrap, from
   gcc + `elf2macho`); the kernel prints to the host console and stops at its
   missing root. Needs interrupts, the timer and clock as host calls, and the shadow MMU; no device chips.
2. Root over host folders: the host filesystem mounts `/` from `~/NeXT/<env>`;
   single-user shell on the console.
3. Multi-user: `makenext` installs; login window and Workspace (`HostFB`, `HostEvent`).
4. Sound, network (`en0` over host sockets), disk-file option, colour turbo, trap-cost work.
5. Non-turbo, OPENSTEP-only layouts, NeXTSTEP 3.3.
