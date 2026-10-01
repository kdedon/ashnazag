# Flaky Mac-environment tests

`flaky.diff` applies to live with `patch -p1` (after s1 step1). Both flakes were in the test, not in the kernel's open/UI_SET order or input path.

## mac.system_opened (also mac76)

Cause: `relay()` turned off `aux_trace` and `uinter_trace` as soon as the Mac's session came to front, because later opens would wrap the 8 KB ring over the UI_SET line. The Finder opens `//mac/sys/Sys7/System` at about the same moment, so the check depended on when the 100 ms poll saw the front change. Failing logs show the trace ending at the zoneinfo opens, before `Q42` and the System open. When I only moved the cut-off, it still failed: the Finder's directory walk right after the System open writes more than 8 KB between two polls, so the ring wrapped past `Q01`. step1 had hidden this for 7.6.1 by accepting the open made before UI_SET (`q1 = klog`).

Fix: `aux_trace` bit 16 (auxsys.c) keeps the first `AUX_TBUF` bytes. Lines that do not fit are dropped whole. `t_mac` zeroes `aux_tpos`, traces with `4|16` from the start of startmac, and no longer stops the trace at front. The first 8 KB always holds UI_SET (about 160 bytes in) and the System open after it (about 900 bytes in for Sys7, about 980 for 7.6.1). I removed the `SYS76` workaround, so mac76 now checks the real open after UI_SET (`%System`).

## mac.clicks_fast (16/20)

Cause: 20 clicks are 40 button events, and the kernel queue (`NEV` 32) drops its oldest event when full, as the Mac's own queue does. If the Finder takes no events for the whole 2.6 s burst, 8 events are lost, which is exactly 16 mouseDowns. The 16/20 seen with `rom-precopy.diff` matches this: it makes the Finder slower. Reproduced on purpose by SIGSTOPping startmac during the old burst: `16 mouseDowns, 8 lost`. The ADB path was clean: 40 of 40 events were posted in every run.

Fix: `clicks()` sends 10 clicks at a time (20 events, which fit the queue). It waits until that chunk's mouseDowns are taken (up to 20 s), or, after 1 s, until every posted event has been taken (this is for `clicks_short`, where QEMU drops 8 ms clicks). The check is still exactly 20. A new counter, `uin_nlost` (uievent.c), counts queue drops and goes into the failure message. With the Mac stopped for the first 3 s, the chunked version still got 20.

## Verification (QEMU q800, MEM=128, t_mac + t_mac76 per boot)

Each boot runs `runall t_mac t_mac76`. FPU on: 11 of 11 boots passed (169 of 169 checks each). `CMDLINE=nofpu`: 1 of 1 passed. With `rom-precopy.diff` (trace fix only, old clicks), 3 of 3 t_mac runs passed; corediag had 3 of 3 failing.

## Other intermittent failures in tests/results (101 runs)

- `otb.udp.options` failed once (20260930-010654, "status ffffffff ffffffff": no reply to the option request) and passed in the 22 runs after it. It comes right after the 5 s wait for a loopback datagram that never comes on a non-net root. Not investigated.
- `aux.sleep_1` failed once (20260929-223851, 834 ms). The current check accepts that, so it no longer fails.
- All other failures cluster in single development runs, or in runs that hung with "config: unknown boot method" (02:33 to 02:54, debug kernels). None of them are flakes.
