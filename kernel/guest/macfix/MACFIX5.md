# Mac environment fixes, round 5

Against 0546a7d. `macfix5.diff` includes macfix4, and `macfix5-tests.diff` replaces macfix4's tests. There were 6 QEMU runs: 5 with only the Mac tests and 1 full suite.

## Changes

**Test volume for long names.** `tests/aux/build.sh` builds the 7.6.1 System Folder (`S761`), a user's copy of it (`S761u`) and the 8.1 System Folder (`S81`) from the CDs. It puts them on a ufs volume, `tests/build/aux/macsys.img`, which is made from a cpio archive with `mkufs.py`. `run-qemu.sh` attaches the volume as SCSI disk 1 with `snapshot=on`. `mktestroot.sh` adds `/dev/dsk/c1d0s0` and `/macsys`, plus ufs mount when either volume exists. `t_mac76` and `t_mac81` mount the volume once per boot and add its line to `/etc/mtab` for fidd. Without the CDs there is no volume, and the `startmac` part skips. The s5 test root no longer holds trimmed copies of S761 and S81.

**`images/macenv/userfinder.py`** is macfix4's inline `fmnu` 1255 edit (Log Out in place of Restart and Shut Down). `mkmacimage.sh` and the test's `S761u` both use it.

**`auxguard.py`, the risk MACFIX4 named.** In `lpch` 63, the `$1F` group holds module 31 (`_ShutDown`) and module 37 (trap 0). The loader marks a trap-0 module for loading without patching a trap. Module 37 shares only the group's condition and is not shown to be ShutDown code. auxguard now gives notAUX only to leading `_ShutDown` entries. If the group continues with other modules, it inserts a new group header (`fe` plus the old condition) after those entries, so module 37 still loads. The loader at `lodr` -16385 accepts a header between entries. The table is the last thing in the resource, so nothing after it moves. 7.6.1 result: `lpch` 63 grows from 2746 to 2750 bytes, with groups `$11F: 31:a895`, `$1F: 37:0000` and `$110: 42:a895`.

**Tests.**
- `t_mac76` runs a user session (uid 101, `S761u`): it shows the desktop, holds the Special menu open, then chooses Apple > Log Out. Next comes a root session that ends with Apple > Log Out. Last comes the existing root session, which ends with Special > Shut Down under `uinter_adtest`.
- Each Log Out checks that startmac exits by itself, without a signal (`*_clean`), with `uinter_adcall` 0, the console in front, and the Mac's RAM freed.
- `t_mac6` ends its session from the Finder: the first run in a boot uses Special > Logout, the next uses Apple > Log Out (the DA). The `runall` limit for t_mac76 is now 600 s.

## Results

- **Second System 6 session: fixed.** Logout runs the shutdown procedures, so the Desktop DB/DF close. `t_mac6` twice in one boot: 0 fail in all 6 runs. Both Logout paths exit with status 0 and no halt.
- **Users' Special menu:** Clean Up Desktop, Empty Trash, Eject Disk, Erase Disk, then Log Out. Restart and Shut Down are gone (screenshot `mac76_user_special`).
- **User's Apple > Log Out (7.6.1):** startmac exits with status `0xc200` (exit 194), with no halt and no fault.
- **Root's Log Out and Shut Down on 7.6.1 still fault at PC $17.** `root_clean` and `shutdown_halts` fail. See the finding below.
- **Full suite (run 6):** 1342 pass, 2 fail, 6 skip. The two failures are `mac76.root_clean` (status `0xc`) and `mac76.shutdown_halts` (exit 250, no uadmin); both come from the finding below. The 6 skips are the usual network and swap ones. Both `t_mac6` runs pass (54/0), and so do `t_mac` (406/0) and `t_amiga` (45/0).

## Finding: root's Log Out calls the LAP Manager unchecked

The PC $17 fault is not in 7.6.1's `_ShutDown` patches. Even with them disabled, `_ShutDown` points at A/UX's dispatcher, and root's Log Out (`_AUXDispatch(12)`, which bypasses `_ShutDown`) faults the same way. The stack in the fault dump leads to `Patch.067C` file offset `$34322`:

    link a6,#-20; move.l a2,-(sp); moveq #25,d0; jsr ([$0B18],2)

`$0B18` is AtalkHk2 (LAP Manager); `Patch.067C`'s own symbol table names it. A/UX sets it from DRVR 9 at startup. Under 7.6.1 it reads `$FFFFFFFF` (no LAP Manager), and the jump lands at $17. A/UX's other call site checks first (`tst.l $0B18; ble` → `moveq #-1,d0`), but the selector 25 sites at `$342C8` and `$3431E` do not. These sites read and edit the AppleTalk transition queue at `2(a1)`. The user's path never reaches them, so it exits cleanly.

**Tried, then left out:** an INIT in `Extensions` (system heap, locked, detached) that points AtalkHk2 at a stub. For selector 25 the stub returns an empty queue in a1; for other selectors it returns -1. The $17 fault went away, but both the user's and root's Log Out then hit an illegal instruction at `$ADD4C6`. A valid AtalkHk2 apparently starts other AppleTalk work. The INIT was left out because it broke the user's clean exit. Possible next steps:
- find what reaches `$342C8` or `$3431E` only for root, and why A/UX skips it when the LAP Manager is missing;
- or have the stub tell the checked path (`$3253A`, a tail jump) apart from the unchecked one.

## Open

- **Kernel panic after unmounting the ufs volume (run 3).** After `t_mac76` unmounted `/macsys`, the next `t_mac6` startmac hit `KERNEL FAULT ... Zero Divide` in `ufs_bmap` ← `ufs_getapage` ← `ufs_getpage` ← `segmap_fault`. Before the unmount, the user session had changed the owner of every file in `S761u` and written to the volume. The unmount itself succeeded. The volume now stays mounted for the rest of the boot. The kernel bug still needs a root fix: a segmap or page reference to a ufs vnode outlives its file system.
- Root Log Out and Shut Down on 7.6.1 (above). 8.1 (`t_mac81`, not in the default suite) was not run.
- `restart()` in `t_mac` calls `/usr/bin/chown`, which the test root lacks, so its chown fails without a message. `t_mac76` changes owners with `chown(2)` instead.
