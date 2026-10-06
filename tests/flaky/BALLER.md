# tos.baller_st_sky

`baller.diff` (tests/src/t_tos.c) applies with `patch -p1` at HEAD.

## Cause

`ballerburg()` took "running at both dumps and the dumps match" as drawn. Between Pexec and Ballerburg's own clear, the AES fills the screen with the desktop pattern. Under load that fill can last across two dumps 1.2 s apart, so `drawn` passes on the fill and `sky` measures the pattern: white 500, fail.

Dumps every 0.6 s under load (run 2) show the sequence:

| dump | running | tos stats | sky |
|---|---|---|---|
| boot screen | 0 | box 140x12 | 1000 |
| AES launch fill | 1 | box 640x480, menu 467, desk 1000 | **500** |
| game | 1 | box 640x396, menu 995 | 1000 |

Under fVDI the boot screen also holds still once Ballerburg runs (dump at 4150 ms). The old check took it as drawn too, but `sky` there is info only.

## Fix

The loop also requires the still dump to be a program's own screen (`fullmenu`): the guest area is filled (w >= 600, h >= 380) and the top 16 rows are white (menu >= 650). Game: 995 (ST), 814 (fVDI). Launch fill: 467. Boot screen: 140 wide. The `sky` and `switch_keeps_screen` checks are unchanged.

## Runs (GROUP=tos, 10 nice-19 compile loops beside QEMU)

- Offline: `fullmenu` applied to all 47 run-2 dumps rejects every boot and launch-fill dump and accepts every game dump.
- Run 3 and run 4, fix plus an extra frozen-guest case: every baller check passed both times.
- A SIGSTOP held on the launch fill was attempted to force the old failure. It missed the fill and froze on the game screen, so that test proves nothing.

## Other flakes seen under the same load (not fixed)

- `tos.tos306_fvdi_drive_c_window: diff 1295 ... second press at 217 ms` (run 3)
- `tos.tos306_fvdi_window_close: diff 63271` (run 4)
