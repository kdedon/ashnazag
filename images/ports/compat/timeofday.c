/*
 * timeofday.c - gettimeofday(2) for Amix (SVR4.0, m68k).
 *
 * Copyright (c) 2026 Jussi Alanärä.  BSD-2-Clause; see LICENSE.
 *
 * ---------------------------------------------------------------------------
 * THE ASYMMETRY THIS EXISTS TO FIX
 *
 * Amix's shared libc exports NO symbol called `gettimeofday`.  What it exports
 * is `_abi_gettimeofday`, and <rpc/types.h> line 82 quietly says
 *
 *     #define gettimeofday	_abi_gettimeofday
 *
 * so a translation unit that happens to include that header links, and one that
 * does not, does not.  Worse: `gettimeofday` IS present in the static libc.a,
 * so a configure probe that link-tests against the static archive concludes the
 * function exists while the shared link fails.  A probe of the SHARED library
 * concludes it is missing.  Both answers are true of the thing each measured,
 * and neither is a usable answer for a recipe -- which is exactly why this
 * wrapper is here rather than left for each port to rediscover.
 *
 * ---------------------------------------------------------------------------
 * THE REAL CONTRACT
 *
 * _abi_gettimeofday's signature is NOT the BSD one.  Its code in
 * libc.so.1 shows:
 *
 *     _abi_gettimeofday:
 *         moveal %fp@(8),%a2      <- argument 1, the only one touched
 *         tstl   %a2
 *         beqw   <return 0>       <- NULL is not an error; it does nothing
 *         ... bsrl _abi_hrtcntl   <- (1, 1, 0, &buf): the high-res timer
 *         movel  %fp@(-16),%a2@   <- buf[0] -> tv_sec
 *         movel  %fp@(-12),%a2@(4)<- buf[1] -> tv_usec
 *         clrl   %d0              <- 0 on success, -1 if hrtcntl failed
 *
 * %fp@(12) -- where a second argument would live -- is never read.  There is no
 * timezone parameter underneath, so there is nothing this wrapper could fill
 * one from.
 *
 * The wrapper therefore accepts the two-argument BSD signature every caller
 * writes and IGNORES tzp.  That is a deliberate, documented lie of omission and
 * the only honest option: a caller wanting a timezone gets a NULL-safe no-op
 * rather than fabricated data.  In 2026 no correct program uses the timezone
 * argument anyway; every one passes NULL.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS FILE MUST NOT INCLUDE
 *
 * <rpc/types.h>.  Its macro would rewrite the definition below into a
 * definition of _abi_gettimeofday itself -- a duplicate symbol against libc at
 * best, and infinite recursion if it resolved the other way.  The include list
 * here is deliberately short for that reason.
 */

#include <sys/types.h>
#include <sys/time.h>

#include "amixcompat.h"

/*
 * Declared here rather than taken from a header, because no header declares it.
 * One argument, returns int -- see the listing above.
 */
extern int _abi_gettimeofday(struct timeval *);

int
gettimeofday(struct timeval *tp, void *tzp)
{
	/*
	 * tzp is accepted and discarded; see the header comment.  The cast to
	 * void is what stops -Wall from reporting it and what tells a reader
	 * the omission is intentional rather than forgotten.
	 */
	(void)tzp;
	return (_abi_gettimeofday(tp));
}
