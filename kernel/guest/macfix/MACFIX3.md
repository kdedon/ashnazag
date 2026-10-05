# Mac environment fixes, round 3 (partial)

Against 49f05f0. `macfix3.diff` includes macfix2's uinter change; `macfix3-tests.diff` is macfix2's tests unchanged. 2 QEMU runs, then the budget ran out. The full suite was not run.

## Done

**Users' Special menu in 7.6.1 (`mkmacimage.sh`).** After root's copy is made, the template's Finder gets a new `fmnu` 1255: Restart is removed, and the `shut` item is renamed "Log Out" (flags `81 00`, literal name, like Restart's). Verified in QEMU: the Special menu shows Clean Up Desktop … Erase Disk, a separator, then Log Out. Choosing it as a non-root user brings up A/UX's dialog, whose Logout button ends the session. This was not run as non-root.

**`rsrcedit.py -r TYPE ID HEX`** replaces a resource's data at any size.

**DITL 128** (root's dialog when other users are logged in) also gets Cancel → Logout.

## Findings

**Root never sees a dialog when alone.** In `Patch.067C`, `mainDialog` skips the dialog when `getuid() == 0`, no other users are logged in (utmp `USER_PROCESS` entries not on `console` or `ttyC*`), and no NFS clients are listed. `cShutDwnPower` then shuts down directly. Any 0 return (Cancel) calls `cLogout`. A root Log Out option can't come from a resource edit, so a decision is needed. One option is a small Log Out item of our own that calls `_AUXDispatch(12)`. Open issue: `makemac -f` run by root copies the users' Finder over root's.

**macfix2's root halt is not reached.** `shutdown_halts` fails: `uinter_adcall` stays 0, and the PC $17 F-line fault still happens. 7.6.1's Shut Down never reaches `UI_SHUTDOWN`. The likely cause is 7.6.1's own ShutDown patch replacing A/UX's `aShutDown`. Next step: find it in the `lpch` and re-guard it with `auxguard.py`. `uinter_adtest` is a plain module global, written only through `/dev/kmem` (root). `uiadmin` checks `suser` before using it.

**System 6 menu.** Finder 6.1.7 (CODE 3) uses MENU 25 (Restart, Shut Down, Logout) when `HWCfgFlags` bit 9 (`hwCbAUX`, `btst #1,$B22`) is set, and A/UX 2's `Patch.067C` sets it. Logout calls `_AUXDispatch(12)`. Root already gets the right menu. Users would need their own Finder copy with MENU 25 trimmed, but our System 6 staging has no per-user System Folder.

**Second System 6 session: cause found.** Run 2 moved the first session's `Desktop DB`/`DF` (in the System Folder) away, and the second session passed. The third session, which found the second session's files, failed (Finder hung: no menus, `finder_alive`). The damage comes from how the session ends: t_mac6 ends it with SIGHUP/SIGKILL, so `_DTInit`'s Desktop Manager never closes its B-tree files. Root fix: end the session from the Finder (Special > Logout, which runs the shutdown procedures) instead of killing it. Not done.

## Not verified

Non-root Log Out ending a session, root halt via the hook, t_mac6 twice, and the full suite.
