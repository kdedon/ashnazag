# Mac environment fixes, round 6

Against d9e5a30. `macfix6.diff` includes macfix5, and `macfix6-tests.diff` replaces macfix5's tests.

## The LAP Manager call

`Patch.067C` has two AppleTalk transition queue walkers: `ateventr` ($381E8) and `CALLATQ` ($3823E). Each one runs `moveq #25,d0; jsr ([$0B18],2)` to get the queue header (selector 25, LGetATQ, returned in a1), then calls each element's handler at +6. `CALLCLTASK` (close event 0) calls `CALLATQ`. `cLogout`, `cShutDwnStart` and `cShutDwnPower` call `CALLCLTASK` unless bit 0 of `startInfo`+66 is set.

`loadlmgr` clears `$0B18` and then sets it from the System's `DRVR` 9 (`.MPP`). System 7.0.1 has `DRVR` 9 (.MPP, 14808 bytes). 7.6.1 has only `drvr` 9 and `lmgr` 1, so `loadlmgr` leaves `$0B18` unset, and the environment reads `$FFFFFFFF`. A/UX's own dispatcher at $3645A checks first (`tst.l $0B18; ble` → `moveq #-1,d0; rts`), but the two walkers do not.

**Why the stub INIT broke every Log Out:** the user's Log Out also reaches `CALLATQ`. With `$0B18 = -1`, the jump to $1 ended the user's session with exit 194, which MACFIX5 counted as a clean exit. With any LAP Manager present, both sessions run past the call and reach the crash below. The stub's return convention was not the cause.

## Change

`uinter` redirects both walkers' calls in the loaded copy of `Patch.067C`:

    lea $36430(pc),a1; moveq #25,d0; jsr $3645A(pc)

These 10 bytes replace `moveq #25,d0; jsr ([$0B18],2)` at $381EE and $38244. With a LAP Manager, A/UX's dispatcher tail-jumps to it, so the call behaves as before. Without one, the dispatcher returns at once, and a1 points at the `.MPP` header, whose long at +2 is 0. That is an empty queue, so no handler runs. The redirect is applied once per session, at the first `UI_TIMER` (which `Patch.067C` issues), and only if all 50 bytes match exactly: both sites, the dispatcher and the header. Afterwards the caches are pushed. The files on disk are not changed. Off switch: `uinter_lapchk=0`.

I preferred the redirect to a stub. The environment had a LAP Manager only through the System's `DRVR` 9, and 7.6.1 has none. A stub would have to emulate AppleTalk 58's LAP Manager for 7.6.1's own callers. The redirect only adds the check A/UX's own dispatcher already makes.

Test: `t_mac76` checks `user_lap_redirect` and `root_lap_redirect`, the 10 bytes at $38244 in the live session.

## Results

QEMU runs: 2 Mac-only and 1 full suite (plus 2 that stopped at the test build, before QEMU).

- **PC $17 is gone.** Under 7.6.1, `$0B18` still reads `$FFFFFFFF`, and both sessions get past the LAP call.
- **New blocker, the same for the user and root:** about 0.5 s after Log Out (or Special > Shut Down), `vector 4 at $ADD4C6` (word `$00E0`) ends startmac with SIGILL. `user_clean`, `root_clean` and `shutdown_halts` fail. Shut Down exits with status 0 before `uadmin`.
  - $ADD4C0 holds 8-byte segment-loader jump-table entries (`.. 4EAD 05BA`, which is `jsr $5BA(a5)`). The return address $ADD4C6 is on the stack: an entry's `jsr $5BA(a5)` came back to the entry, so it ran without loading its segment. a5 = $AEC860, the Finder's partition. The caller's frame returns to about $1D03F8 (A5-relative code).
  - A/UX's shutdown queue at this point holds procedures $6FF2 (flags 4), $207270 (1), $281D92 (4) and $28B614 (8).
  - Next step: find whose jump table is at $ADD4C0, and which `cLogout` step (`d372` shutdown procedures, `_HideCursor`/`a8d6`, `doLogout`) runs it under the wrong A5 or after its segments are unloaded.
- System 7.0.1 (`t_mac`, 203/0) and System 6 twice in a boot (`t_mac6`, 54/0) pass with the redirect.
- **Full default suite (run 3):** 1342 pass, 3 fail, 6 skip. The 3 failures are the $ADD4C6 ones above. The 6 skips are the usual network, swap and bash ones. There was no panic, and `t_mac`, both `t_mac6` runs and `t_amiga` pass. MACFIX5 had 2 failures; the third, `user_clean`, comes from the user's exit through $1 being gone.

## Open

- The $ADD4C6 crash above, which blocks both Log Out and root's Shut Down on 7.6.1.
- The `ufs_bmap` zero divide after unmount is being fixed elsewhere. Volume handling is unchanged.
