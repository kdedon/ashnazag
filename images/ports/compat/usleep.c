/*
 * usleep.c - usleep(3) for Amix (SVR4.0, m68k).
 *
 * Copyright (c) 2026 Jussi Alanärä.  BSD-2-Clause; see LICENSE.
 *
 * Amix's libc has sleep(3) and nothing finer.  It does have poll(2), and
 * poll(NULL, 0, ms) is SVR4's canonical sub-second sleep -- no descriptors, no
 * signal handling, no alarm() to collide with a caller's own timers (which is
 * exactly why the classic setitimer/pause implementation of usleep is a bad
 * neighbour and is not used here).
 *
 * TWO HONEST LIMITS, both worth reading before building anything on this:
 *
 *   1. poll's timeout is in MILLISECONDS.  Any non-zero request under 1000 us
 *      rounds UP to 1 ms.  Rounding down would turn a polling loop that says
 *      "wait 100 us" into a spin, and a spin on a 25 MHz 68030 is not a
 *      nuisance, it is the whole machine.
 *   2. the kernel tick is HZ = 60, so the real granularity is ~16.7 ms
 *      regardless of what is asked for.  usleep(1) sleeps for a tick.  Do not
 *      build timing on this; use it for backing off, not for measuring.
 *
 * A request of 0 returns immediately without entering the kernel, matching
 * every other usleep.
 */

#include <errno.h>
#include <poll.h>
#include <sys/types.h>

#include "amixcompat.h"

int
usleep(unsigned int usec)
{
	int ms;

	if (usec == 0)
		return (0);

	/*
	 * Round up, and clamp: poll takes an int timeout, and a caller asking
	 * for more than ~24 days of microseconds has overflowed something
	 * upstream.  Clamping to INT_MAX milliseconds is a longer sleep than
	 * any caller means, but it is never a SHORTER one, which is the
	 * direction that produces a busy loop.
	 */
	if (usec > (unsigned int)2147483647U - 999U)
		ms = 2147483647;
	else
		ms = (int)((usec + 999U) / 1000U);

	while (poll((struct pollfd *)0, (unsigned long)0, ms) < 0) {
		/*
		 * EINTR is the only failure worth retrying, and retrying with
		 * the ORIGINAL timeout would extend the sleep every time a
		 * signal arrived.  BSD usleep returns -1/EINTR instead; so does
		 * this, because a caller that cares can loop with its own
		 * remaining-time arithmetic and one that does not would rather
		 * be woken.
		 */
		return (-1);
	}
	return (0);
}
