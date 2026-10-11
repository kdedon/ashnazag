# Flaky Mac-environment tests

`flaky.diff` applies with `patch -p1`. Both flakes were in the test, not in the kernel's open/UI_SET order or input path.

## mac.system_opened (also mac76)

Cause: `relay()` turned off `aux_trace` and `uinter_trace` as soon as the Mac's session came to front, because later opens would wrap the 8 KB ring over the UI_SET line. The Finder opens `//mac/sys/Sys7/System` at about the same moment, so the check depended on when the 100 ms poll saw the front change. Moving the cut-off was not enough: the Finder's directory walk right after the System open writes more than 8 KB between two polls, so the ring wrapped past `Q01`.

Fix: `aux_trace` bit 16 (auxsys.c) keeps the first `AUX_TBUF` bytes. Lines that do not fit are dropped whole. `t_mac` zeroes `aux_tpos`, traces with `4|16` from the start of startmac, and no longer stops the trace at front. The first 8 KB always holds UI_SET (about 160 bytes in) and the System open after it (about 900 bytes in for Sys7, about 980 for 7.6.1). The `SYS76` workaround is gone; mac76 checks the real open after UI_SET (`%System`).

## mac.clicks_fast (16/20)

Cause: 20 clicks are 40 button events, and the kernel queue (`NEV` 32) drops its oldest event when full, as the Mac's own queue does. If the Finder takes no events for the whole 2.6 s burst, 8 events are lost, which is exactly 16 mouseDowns. SIGSTOPping startmac during the old burst reproduces it: `16 mouseDowns, 8 lost`. The ADB path was clean: 40 of 40 events were posted in every run.

Fix: `clicks()` sends 10 clicks at a time (20 events, which fit the queue). It waits until that chunk's mouseDowns are taken (up to 20 s), or, after 1 s, until every posted event has been taken (this is for `clicks_short`, where QEMU drops 8 ms clicks). The check is still exactly 20. A new counter, `uin_nlost` (uievent.c), counts queue drops and goes into the failure message.
