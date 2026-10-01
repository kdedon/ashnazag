# macenv polish: notices, desktop, full install, model, memory

Diffs against live (after `tests/flaky*`, `kernel/guest-tos/{mouse,hostfs}`):

- `kernel.diff`: `kernel/guest/{guest_shim.c,chkguest.py,include/guest.h}`, `kernel/mac/relink-mac.sh`, `kernel/guest/mod/{guestcore/gcpu.c,guestcore/guestcore.c,auxcore/auxsys.c,uinter/uinter.c,uinter/uinter.h,uinter/uirom.c}`.
- `macenv.diff`: `images/macenv/{startmac,S05aux,macdiag.c,mkmacimage.sh,mac-diskroot.diff}`, new `images/macenv/mkdesktop.py`, `images/README.md`.
- `docs.diff`: new `kernel/guest-m4/MODEL.md` (model table, untested hosts, SE/30), `docs/aux-startmac-boot.md` (memory map).
- `tests.diff`: `tests/aux/macabi.c` (`prodinfo` accepts a Quadra 800 on the Quadra 700 record).

Build: `kernel/build.sh` (relink: `unt_latch` override), then `images/macenv/mkmacimage.sh`.

## 1. Probe notices

The NOTICE comes from `u_trap`'s `cmn_err`, which the port routes through `unt_latch`.  The Mac kernel now overrides `unt_latch` (`guest_shim.c`, aliased `__amix_unt_latch`) and asks the `guest_unote` hook; `guestcore` says quiet when `guest_fault` will reflect the fault to the guest's vector 2 (same checks, vector nonzero).  Counted in `guest_nunote`, shown by `macdiag` as `quiet N` on the `vec2` line.  If the reflection then fails (bad USP), `guest_fsig` prints the notice itself, so faults that kill still report.

## 2. "Rebuilding the desktop"

Not a loop in our code.  The Finder rebuilds the desktop database of `/` (the whole Unix root) when `TBSYSTEM` has no `Desktop DB`/`DF`; A/UX keeps them in the System Folder and ships them prebuilt.  The Q13/Q0c pattern is the Finder's idle loop: `WaitNextEvent` with a sleep, ended by every tick signal (`EINTR` / null event), about 130 syscalls a second; unchanged.

- A/UX's own `Desktop DB` from the user's disk names its files and window places: with it, opening `/` left no visible window.  Dropped.
- `mkdesktop.py` (last step of `mkmacimage.sh`, `DESKTOP=0` skips): boots the image once in QEMU (Quadra 800 ROM, 128 MB, not `-snapshot`), runs `startmac`, waits until both files are unchanged for a minute, kills it, `sync`, `uadmin 2 0`.  A killed Finder leaves a usable database (checked: the next start does not rebuild).

## 3. Full Mac install

`mkmacimage.sh` packs A/UX's Mac files into `mac.cpio` (`a` line in the manifest): `/mac` (System Folder, SystemFiles, CommandShell, TeachText, MacX, Commando, …), `/ Applications`, `/ Documentation`, `/ Shared Data`, `/ System Folder alias`, root's `/.mac/localhost`, `/usr/bin/systemfolder`, `/usr/lib/updtsysfldr`; less `.fs_*` caches; root, group sys.  root runs `/mac/sys/System Folder` (as A/UX); other users get `$HOME/System Folder` from A/UX's `systemfolder` (ksh).  Root 160 MB, swap 96 MB.

Found with the apps present: quitting any second application (a desk accessory, TeachText) killed `startmac` (SIGSEGV, SP `0x3FFFFF00`).  The System's Process Manager (`scod` −16468) moves to a stack at `0x3FFFFF00` (HeapEnd `0x3FFF0000`) while it disposes of a process.  `uinter` maps 64 KB zero-fill there with the ROM (Q5); `aux_vur` lets a quadrant-0/1 range end at the quadrant's last byte (the stock rule took the end as exclusive and refused it).

## 4. Model

Rule in `uirom.c` (`pickbox`), override `/etc/aux/model` (Gestalt ID) written by `startmac` from `MACMODEL`; table and untested cases in `kernel/guest-m4/MODEL.md`.  QEMU q800 with the Quadra 700 ROM: box 29, About This Macintosh "Macintosh Quadra 800".  CPUFlag/MMUType now follow `cputype` (3 on a 68030).

## 5. Memory

A/UX's rule: physical memory, at most 16 MB (`__tb_SetDefaults`); `TBMEMORY` overrides.  The Mac got 11 MB for any larger request: Mac RAM is one shm segment at 0 that `startmac` retries 1 MB smaller on failure, and `shmat` failed (ENOMEM) past 15 MB on the 16 MB `RLIMIT_VMEM`.  `uinter` raises SHMMAX to 36 MB; the `startmac` wrapper raises the soft `RLIMIT_VMEM` (see Review fixes).  `startmac` default: a quarter of physical memory (`/etc/aux/memsize`, from `macdiag -m` at boot), 8–32 MB, less if free swap is short.  `macdiag` raises its own limit to attach 32 MB.

## Tested (QEMU q800, 128 MB, FPU)

Fresh image (database made by `mkdesktop.py`), first `startmac`: Finder desktop in 6 s, no rebuild (12k syscalls a minute in, against 190k with a rebuild); About This Macintosh: Macintosh Quadra 800, Built-in 131,072K, Total 32,768K; Apple menu (Alarm Clock … Scrapbook, CommandShell), Control Panels folder (18), `/` and Applications windows with their icons; Key Caps, Calculator and TeachText open and quit back to the Finder; console: no NOTICE (`quiet 3`).  `MACMODEL=22`: box 16.  `t_mac` 156/156, `t_mac76` 13/13 (`run-qemu.sh`, these two only), no NOTICE.  Not run: a non-root user's `systemfolder` path, hosts other than the Quadra 800.

## Live issue seen

Live `kernel/guest-tos/hostfs.diff` makes `tosml.s` call `gemdos` (in `hostfs.c`), but `images/tosenv/mktos.sh` does not build `hostfs.c`: `mkmacimage.sh` fails at "TOS container files".  Built here with hostfs reverted.

## Review fixes

- `mkdesktop.py` holds `images/work/.qemu.lock` (`fcntl.flock`, the file `run-qemu.sh` and `run-mac.sh` lock) from before QEMU starts until exit.
- `uinter` no longer touches `RLIMIT_VMEM`.  The kernel's default `rlimits` already give VMEM soft 16 MB, hard unlimited; an admin caps it with the hard limit.  `startmac` raises the soft limit to TBMEMORY + 16 MB; past the hard limit it says so and shrinks TBMEMORY (`startmac: TBMEMORY=32M needs a memory limit of 49152K, the hard limit is 40000K: 23M`; below 3 MB it exits).
- Built with live's `mktos.sh` (hostfs present).  QEMU: root 32 MB as before; under `ulimit -H -v 40000` 23 MB with the message; non-root `display` user (`su - macu`, console owned by the user as after a console login): `systemfolder` made `$HOME/System Folder` (shared files linked, Desktop DB/DF copied), Finder up with 32 MB.
- A killed `startmac` left its `tLOW` segment, and the next user's `startmac` said "not enough memory".  Now `uidetach` (the layer's task exits or kills its layer) marks the `tLOW` segment that task created for removal (`shmctl IPC_RMID` as that task, found in `shmem[]` by key and creator pid); the kernel frees it at the last detach, so a live session's other attachers keep it.  `t_mac`: `mac_ram_freed` after SIGKILL, then `restart` runs as uid 101, group display (System Folder and console handed over): `restart_front`, `restart_no_alert` pass.  `t_mac` 157/157, `t_mac76` 14/14, no NOTICE.
