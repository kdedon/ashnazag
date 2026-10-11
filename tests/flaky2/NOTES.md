# aux.sleep_1 and otb.udp.options

`flaky2.diff` applies with `patch -p1`, before or after `../flaky/flaky.diff`.

## aux.sleep_1 (834 ms): kernel bug

Cause: A/UX `alarm` (call 27) went straight to AMIX's alarm, which counts whole seconds on the clock's second boundary, so `alarm(n)` fires after n−1 to n s. `/aux/bin/sleep 1` is `alarm(1)` and `pause()`. The test accepted anything under 4 s.

Fix (auxcore): A/UX `alarm(n)` arms its own callout of `n * HZ + 1` ticks, so it is never early. It returns the seconds left, rounded up, and reads and clears any stock alarm left from before an exec. It counts against the same quarter of the callout table as ITIMER_REAL. The kit sets NCALL to 60, so these timers share 15 callouts and can run out. When none is free, or `n` is too large for ticks, the stock alarm gets `n + 1` s, which is late by under 1 s but never early. A forked child starts with no alarm, and exit stops it. On exec of a native image, the stock alarm gets the seconds left plus 1. guestcore sets the new `GPF_EXEC` before calling `gpf_exit` so the profile can tell that exec apart from an exit. The AMIX alarm itself is unchanged, so `sig.alarm_pause` and `time.*` still describe it.

`sleep_1` requires `t >= 1000`; the upper bound stays at 4 s.

`abi` checks: `alarm_1` (1000 to 1500 ms) and `alarm_left` (`alarm(5)` then `alarm(0)` returns 5). Then children set ITIMER_REAL until one gets EAGAIN (`alarm_slots_full`), and `alarm_1_full` checks the fallback (1000 to 2500 ms).

## otb.udp.options: older build, plus a test bug

- The failing run used an older build with the DLM `_cerror` errno bug (`INFO otb.unload_busy_errno: -1056833824`); the otbridge review fixed it, the `kmem_zalloc(KM_SLEEP)` in the write service procedure and the shared `x_bufcall`.
- The failure message could not tell us: `t_check`'s message arguments are evaluated in unspecified order (here before `optreq`), so `status ffffffff ffffffff` was the state before the request, not its result. The check now runs `optreq` and `optst` first, and reports both statuses, the last primitive `optreq` read, and errno.
