# Mac environment robustness (macfix), partial

Against 914cc67. `macfix-tests.diff` only; `macfix.diff` is empty (no environment or kernel change yet). Checkpoint after 2 of 6 QEMU runs, budget spent.

## Done

**t_mac rerun `fidop` EINTR (item 2): fixed.** Cause: the first run's fidd. fidd forks into its own process group, so `kill(-fiddpg, SIGTERM)` missed it. It stayed blocked in `fidop` serve, and the second run's `macabi` messages went to it: `fidop_serve` waited until EINTR, then `take_reply` and `cancel` failed. The kernel queues are fine.
- t_mac: `fiddkill()` finds every live fidd through `/proc` (PIOCPSINFO) and kills it with SIGKILL, before starting fidd and at the end. `mac.fidd_stopped` fails if one survives; `mac.fidd_killed` reports the count.
- runall: a second `t_mac` after `t_mac6` in the default suite (rerun check).

**t_mac6 ends the session with a hangup** first (as a closed login line does), then SIGKILL. A/UX 2's startmac exits 0 on SIGHUP within 20 s.

## Findings, not fixed

**Second System 6 session (item 1).** A clean SIGHUP exit doesn't help: the second session still fails (`apple_menu`, `special_menu`, `disk_window`). So the cause isn't only the kill. The first session's Desktop Manager made `Desktop DB` (2330 bytes) and `Desktop DF` (1562 bytes); neither is a multiple of 512, as a B-tree file would be. The second Finder opens them and loops. Next: compare them with the `Desktop DB`/`DF` that ship next to `_DTInit` on the A/UX 2.0.1 CD (mksys6.sh doesn't copy them), and check whether the Mac side flushes on SIGHUP (trace the File Manager writes at exit with `TBFMDEBUG=1`).

**7.6.1 PC $17 (item 3).** It happens at Shut Down (`mac76.shutdown_exits`), not at startup: `vector 11 at 17 (F000)`, last A-line at `$1D2BAE`. Root's Shut Down reaches `UI_SHUTDOWN`, which returns EPERM for everyone; the ROM's power-off path then falls through to a bad address, and the SIGILL ends the session. So item 3 is fixed by item 4's root Shut Down path, not separately.

**Shut Down (item 4).** Not started beyond design notes:
- Kernel: `UI_SHUTDOWN`/`UI_REBOOT` (uinter ioctls 45/44) are where root's Shut Down/Restart arrive; A/UX calls 64 (reboot) and 65 (powerdown) are still TODO. A clean halt needs `init 0`/`init 6`, which the kernel can't start; suggested: for root, end the session with a distinct exit status and let the startmac script run `init 0` or `init 6`.
- Non-root: the A/UX shutdown dialog (DLOG/DITL 129/130 in `%AUX Resources`, Cancel already reads Logout) is shown without a kernel call. Log Out in the Finder's Special menu (MENU edit in the user's `%Finder`) still reaches that dialog unless the dialog is skipped.

## Verification

Run 1 (`t_mac t_mac t_mac6 t_mac6 t_mac76`): mac 406/0 (both t_mac runs 203/0, `fidd_killed: 1` each), mac76 30/0, mac6 43 pass / 3 fail (second session only).

Run 2 (full default suite, with the t_mac rerun): 1295 pass, 0 fail, 6 skip (mac 406, mac6 23, mac76 30).
