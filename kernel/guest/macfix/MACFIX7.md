# Mac environment fixes, round 7

Against f441c46. `macfix7.diff` is macfix6's environment change unchanged; `macfix7-tests.diff` replaces macfix6's tests. The 7.6.1 Log Out crash is **not fixed**.

## Tests

- `loggedout` (every Log Out: `t_mac76` user and root, `t_mac6`): `_clean` now needs a normal exit with status 0, which is `doLogout`'s `_exit(0)`. A fault, a signal or exit 194 fails.
- `shutdown76` already needed the `uadmin` hook (`0x200`) and SIGKILL. No other Mac test has a "clean" criterion; the other exit statuses are INFO only.
- `t_mac76` lists the shutdown queue before Log Out (`sdqueue_N`, `sdqueue_N_flags`: each procedure's address and first 32 bytes).

## Findings (3 QEMU runs, `t_mac76` only)

- Shutdown queue under 7.6.1: `FileExit` ($6FF2, flag 4) first, then $207270 (1), $281D92 (4), $28B614 (8). $281D92 saves `RECT` -4048 into a resource file and closes it; $28B614 makes Gestalt and file calls. Neither yields.
- **Ordering is not the cause.** A rewrite of `callRoutines` that ran `FileExit` after the other procedures of its phase changed nothing, so it was dropped.
- At the fault: `CurApName` is "Finder", `CurrentA5` $ADCB88, register a5 $AEC860, `ResErr` -192 (resNotFound, user) or 0 (root), and `DSErrCode` 1. `DSErrCode` 1 means A/UX's exception handler had already caught a bus error. In one run the user's session instead died with a bus error reading $FFFFFFFD at $1D47F2. That is the Finder's `CODE` 1 (sysHeap), which reads through a pointer that is -1.
- $ADD4C0 is entry 291 of the Finder's jump table (A5 $ADCB88): `0004 4EAD 05B2 00E0`. The loader behind `jsr $5B2(a5)` (in `CODE` 1, around $1D36F4) returned without loading the segment.
- `UI_KILLMYLAYER` is never reached on Log Out, so `cLogout` does not finish. Root's Special > Shut Down reaches it with `DSErrCode` 0. It exits 0 without `uadmin`, so `cShutDwnPower` does not power off.

## Next

- Most likely cause: something in the Finder's sysHeap code dereferences -1, possibly `AtalkHk2` ($0B18). `loadlmgr` clears it, and something later sets it to $FFFFFFFF. First test: keep $0B18 at 0 under 7.6.1.
- Trace root's Shut Down from `aShutDown`: why `cShutDwnPower` ends at `doLogout`, not `PowerOff`.

## Not run

System 6 twice, `t_mac`/7.0.1 and the full suite were not rerun. The environment change is identical to macfix6, where they passed.
