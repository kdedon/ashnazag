# Mac environment fixes, round 4 (partial, not run)

Against 1d1f86c; `macfix4.diff` includes macfix3. `macfix4-tests.diff` is macfix3's unchanged. The budget ran out before a build or QEMU run, so none of this is verified in the environment.

## Done

**Log Out in the Apple menu (`images/macenv/logout/`).** `app.s` is an application and `da.s` a desk accessory (DRVR 30 "\0Log Out", dNeedLock). Both call `_AUXDispatch(12, nil)`, the call behind Shut Down's Logout button: `cLogout` runs the shutdown procedures, greys the screen, sends `UI_KILLMYLAYER` and exits. It never halts. `build.sh` assembles both with the m68k-linux toolchain. `mklogout.py` writes the application as an AppleDouble pair and adds the DA to a System file. Installed by `mksys76.sh` and `mksys81.sh` in `Apple Menu Items`, and by `mksys6.sh` in the System 6 System file. This covers the image templates and the test roots. Checked offline: the DRVR adds to 2.0.1's AppleSingle System (507 resources, data fork intact), and the application's CODE 1 is in place.

**`rsrcedit.add`** now accepts an existing type and a name. It also accepts a gap before the map and an AppleSingle data fork after the resource fork.

**Root's Shut Down (`auxguard.py`).** 7.6.1's `lpch` 63 patches `_ShutDown` in two groups: condition `$1F` (all ROMs, with one code module) and `$10` (the `$067C` family, ours). These replace `Patch.067C`'s `aShutDown`, so `cShutDwnPower` → `_PowerOff` → `cPowerOff` → `UI_SHUTDOWN` never runs. Instead, the System's code goes on into ROM power-off code and faults at PC $17 (the last A-line is in the System heap, and the stack holds ROM addresses). auxguard now gives notAUX to groups that patch only `_ShutDown`, so A/UX's hook wins (`-n` on the S761 System lists both groups). Expected: root alone → no dialog → `cPowerOff` → `uiadmin` halt; users → A/UX's dialog → Logout.

## Not done

- **Tests.** The test root's s5 file system has 14-character names, so `Apple Menu Items` can't exist there. The application could instead be linked into `/Desktop Folder` and opened like SimpleText. Still to do:
  - `t_mac76`: a root session ended with Log Out (`startmac` exits 0, `uinter_adcall` 0); `shutdown76` already checks the halt.
  - t_mac6: end the first session with Special > Logout, mouse at (184,9) then (189,155). End the second with Apple > Log Out at (60,218), alphabetically after Key Caps. Wait on `macpid` as `shutdown76` does.
  - A non-root 7.6.1 session needs the users' `fmnu` Finder in the test root.
- **Recovery of a killed System 6 session** (stale Desktop DB/DF): not started.
- **Unverified risk:** the `$1F` group's code module goes with it. If other 7.6.1 or 8.1 code links to that module, the guard may need to cover only the `$10` group.
- No full suite run.
