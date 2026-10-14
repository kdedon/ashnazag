# NeXT environment

Design for `startnext`: the user's NeXT ROM and an installed NeXTSTEP run natively on a
68040/060 host, unmodified. Status: design only.

## Shape

```
 startnext (native launcher, user's rights): ROM file, RAM size, env dir, SCSI map
   |
 guest process: NeXT ROM, Mach kernel, apps, all native 68k in user mode
   |  privileged insns, page faults           |  accesses to 0x02000000 I/O pages
   v                                          v
 guestcore: virtual S mode, vIPL,          nextguest DLM: the board's register model
 shadow 040 MMU (S and U spaces)             KMS        -> display service kbd/mouse, sndout
                                             VRAM pages -> converted display surface
                                             ESP + DMA  -> host files (CD, disks)
                                             enet + DMA -> station -> nextnet (user daemon):
                                                           bootp, tftp, NFS2, mount,
                                                           NetInfo, NAT to host sockets
                                             RTC/NVRAM  -> wall clock, env's nvram file
```

Same split as Voyage 200 (`tiplatform`) and the Amiga (`amigaguest`): guestcore runs the
CPU side, a platform DLM answers the hardware through trapped register pages
(guest-container-design §5), host services do display, input, sound, files.

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
| MMU off | TC.E=0: identity map of the board (ROM, RAM, VRAM; I/O trapped). The ROM runs here |
| `movec` | add URP, SRP, TC, ITT0/1, DTT0/1, MMUSR as live state; `ptest` fills MMUSR from a table walk |
| Interrupts | vector mode: levels 1-7 from the interrupt status/mask registers, autovectored through vVBR |
| FPU | 040 host: unimplemented-FP traps go to the guest's own handlers. 060 host: host FPSP/ISP for 060-only gaps, the rest reflected |

What traps, and cost (a gate round trip is a few µs on a 25 MHz 040; measure):

- every privileged instruction; `spl` (moves to and from SR) runs thousands of times a second;
- `moves` in copyin/copyout, per element;
- shadow fills, first writes (M bit), every I/O register access;
- each guest syscall or trap: host gate, reflect, Gs switch, `rte` back, Gu switch.

Mitigations, all generic (no hooks keyed to an OS build): hot-site trap-and-patch from
guest-container §5 for `move SR` sites; a `moves` run-ahead that emulates the copy loop
in one trap; a shadow cache keyed by URP so task switches keep their mappings.

## 2. Hardware and host services

Target model: NeXTstation Turbo. Guest-physical map (all quadrant 0 except slots):

| Range | Part | Host side |
|---|---|---|
| 0x00000000, 0x01000000 | ROM 128 KB (alias) | user's file, read-only pages |
| 0x02000000-0x0201FFFF, 0x02100000 alias | I/O | trapped pages, model in `nextguest` |
| 0x02200000 | TMC memory controller | model (sizes RAM) |
| 0x04000000 | RAM, up to 128 MB | guest RAM object |
| 0x0C000000 | VRAM | surface pages |
| 0xF0000000- | NextBus slots | absent |

| Device | Model | Host service |
|---|---|---|
| Video | mono 1120x832, 2 bpp, 0 = white, 280 bytes/row; colour 16 bpp RGBA4444 | converted surface (write-protect tracking, ≤50 Hz): 2 bpp to 8-bit CLUT, 4444 to host 16/32 |
| Keyboard, mouse | KMS at 0x0200E000, level 3; turbo's ADB controller reports no devices | display service `/dev/kbd`, `/dev/mouse` codes to NeXT codes |
| Sound | sound-out DMA, 44.1 kHz 16-bit stereo, via KMS | sndout; sound-in silent |
| SCSI | NCR53C90A + SCSI DMA channel | targets are host files: CD images read-only, disk images rw |
| Ethernet | AT&T 7213 (turbo) + rx/tx DMA | station raw-frame stream (as `otbridge`) to `nextnet` |
| Clock, NVRAM | MCCS1850 behind SCR2 bit-bang | wall clock; NVRAM in `~/NeXT/<env>/nvram` |
| Timers | hardclock, event counter | high-resolution timer service |
| System regs | SCR1 (machine id), SCR2, interrupt status/mask | model |
| DMA | channel CSRs, next/limit | DMA done by the module into guest RAM |
| DSP 56001, printer, floppy, SCC | | absent first; floppy and serial later |

NeXTdimension (i860) and the non-turbo MO drive are out of scope.

## 3. Files

Like the Mac environment, the guest works on host files directly: the root is a host
folder, `~/NeXT/<env>`, served over NFS. The ROM netboots (`bootp`, `tftp` of the guest's
own boot files), Mach mounts `/` from `nextnet`, a per-session user daemon with the
user's rights. NeXT owners, modes, device nodes and links live in the sidecars of
guest-container §4.4. `~` and `/` of the host appear under `/Net`.

The ROM, Mach and NetInfo do this unmodified: diskless NeXT clients are standard.
`nextnet` also NATs other traffic to host sockets. Any NFS or RPC code taken from
elsewhere stays in this separate program.

Install: `makenext` boots the user's CD image with a temporary SCSI disk file, runs
the stock installer, copies the result into the folder with a host-side NeXT UFS
reader and deletes the image. A user's existing NeXT disk dump copies the same way.
Disk images as SCSI targets stay an option, never the default.

## 4. First model and ROM

NeXTstation Turbo, ROM Rev 3.3 v74, mono first, colour next:

- turbo boards have no memory-write-function aliases (0x10000000 RAM, 0x0C-0x0F VRAM on
  non-turbo mono), which would trap on every window-server pixel write;
- no MO controller, no NBIC, one VRAM layout;
- 040 as on the host.

Rev 2.5 v66 (non-turbo) follows with MWF pages trapped. Rev 1.2 (68030) needs a 030
table walker; later. NeXTdimension and drive firmware ROMs are unused.

## 5. Media

| Item | Source | Format |
|---|---|---|
| ROM | user (`NeXTROMS.zip`: 68040 turbo Rev 3.3 v74) | 128 KB raw |
| NeXTSTEP 3.3 user CD (68k or multi-architecture) | user | raw 2048-byte-sector image: `.iso`, `.cdr`, `.toast`, `.img`; any copy |
| OPENSTEP 4.2 user CD | user, optional | same |
| Developer CD | user, optional | same |
| Installed NeXT disk dump | user, optional | raw image |

No hashes are checked; the launcher only checks sizes and formats.

## 6. Risks

1. Trap rate in the Mach kernel (`spl`, `moves`, syscalls) decides usability. Measure
   first, then patch hot sites generically.
2. Shadow MMU and the Gs/Gu switch touch the host HAT; a static hook may be needed.
3. Screen size: 1120x832 is fixed. Hosts at 1152x870 fit; smaller ones pan. The
   environment never sets host modes.
4. Netboot root over NFS: speed and NetInfo setup; the DSP absent may upset sound apps.
5. 060 hosts: FP and integer gaps under a guest that expects an 040.
6. Licence: register facts come from public sources. Any code taken from a GPL
   implementation stays in a separately loaded DLM or the `nextnet` program, listed in
   THIRD_PARTY.md, never the static kernel.

## 7. Milestones

1. ROM monitor: Rev 3.3 boots with MMU off to its `NeXT>` prompt on the display
   service; typing works. Needs I/O model (SCR1/2, TMC, interrupts, timers, RTC/NVRAM,
   KMS, idle DMA/ESP/enet) and the 2 bpp surface. No shadow MMU.
2. Mach from CD: ESP + DMA with the CD image as a target; ROM loads the boot program
   and Mach; shadow MMU; installer screen.
3. Installed system in `~/NeXT`: `makenext`, `nextnet` netboot root, login window,
   Workspace.
4. Sound, ethernet NAT, colour turbo, trap-cost work.
5. Non-turbo (Rev 2.5), OPENSTEP 4.2.
