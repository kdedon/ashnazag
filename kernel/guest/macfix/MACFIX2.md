# Mac environment fixes, round 2 (partial)

Against 520685c. Budget spent; not verified in QEMU (the one run went to single-user mode: init doesn't take `VAR=x cmd` in an inittab command, so runall never started).

## Done, unverified

**Root's Shut Down and Restart (`macfix2.diff`).** uinter's `UI_SHUTDOWN` (45) and `UI_REBOOT` (44) call `uiadmin`: for root, `sync()` then `uadmin(A_SHUTDOWN, AD_HALT or AD_BOOT)`, which ends in `mdboot` (halt or ROM restart). Others still get EPERM. Test hook: with `uinter_adtest` set, the call is recorded in `uinter_adcall` (`cmd << 8 | fcn`) and the caller is killed with SIGKILL, so the suite isn't halted. This should also fix the PC $17 fault (item 3): the ROM's fallthrough only ran because the ioctl returned.

**Tests (`macfix2-tests.diff`).** `t_mac76`'s `shutdown76` sets the hook and checks `mac76.shutdown_halts`: `uinter_adcall == 0x200` and SIGKILL status (a PC $17 fault ends with SIGILL first). runall: a second `t_mac6` after the `t_mac` rerun.

## Findings, not done

**Special menu (with the refinement: root gets Log Out, Restart, Shut Down; others Log Out only).**
- System 6: Finder 6.1.7 already has MENU 25: Special with Restart, Shut Down, `-`, `Logout` (MENU 5 and 15 are the plain variants). Root's menu exists; find which flag makes the Finder pick 25. Non-root: edit MENU 25 in the user's copy to drop Restart and Shut Down.
- 7.6.1: the Finder's menus are `fmnu`, not MENU. Special is `fmnu` 1255, items keyed by command (`rest`, `shut`), so an item can be removed without renumbering. The `shut` entry's name is the 1-byte string `S` (`81 04 00 00 01 'S'`), so its title comes from elsewhere at run time. A Log Out item needs a command the Finder handles; none exists in 1255.
- makemac runs on the guest, which has no Python: the edited non-root resource file has to be made at image build and copied by makemac when the user isn't root (root's copy also goes through `HOME=/mac/sys makemac -f`).
- Log Out's action: Patch.067C's `doLogout` (`UI_KILLMYLAYER`, `_AUXDispatch` 12 `cLogout`).

**Second System 6 session.** The sizes aren't a sign of corruption: A/UX 2 keeps files as AppleSingle with a 282-byte header. The CD's `Desktop DB` is 6426 = 282 + 6144 (12 nodes of 512); `Desktop DF` 14106 = 282 + 13568. A first session's 2330 = 282 + 2048 and 1562 = 282 + 1280 are well formed by size. Planned check: before the second session, move `Desktop DB`/`DF` away; if it passes, the database contents are at fault, otherwise other state is (run it with a `t_mac6`-only inittab, no environment variables on the command).

## Next

1. Run `t_mac76 t_mac6 t_mac6` (inittab `tt::sysinit:/tests/runall t_mac76:330 t_mac6:330 t_mac6:330 ...`) to verify `shutdown_halts`.
2. The Desktop DB experiment above.
3. Menus as above, then the full suite.
