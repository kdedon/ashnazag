/*
 * strcase.c - strcasecmp / strncasecmp for Amix (SVR4.0, m68k).
 *
 * Copyright (c) 2026 Jussi Alanärä.  BSD-2-Clause; see LICENSE.
 *
 * These are the shim's subtlest entries, because they are not simply absent.
 *
 *   - libc.so.1 does not export them.
 *   - libresolv.a DOES export strcasecmp (measured: `T strcasecmp`), and so do
 *     the other networking archives.
 *   - NO header on the system declares either one.
 *
 * The failure that produces is quiet.  A program that links -lresolv compiles
 * every call against gcc 2.7's implicit `int strcasecmp()` declaration, the
 * link succeeds, and nothing warns unless -Wall is on and someone reads it.
 * That is why this file's companion header exists at all: on this target the
 * DECLARATION is as much of the deliverable as the code.
 *
 * Linking both -lresolv and -lacompat is not an error -- the linker takes
 * whichever archive it reaches first, and both implementations agree on ASCII.
 *
 * WHY THE FOLD IS HAND-WRITTEN.  SVR4's tolower() is specified only for
 * characters that actually ARE upper case; handing it anything else is
 * undefined, and on some SVR4 libcs it indexes a table out of range.  The
 * classic `tolower(*s1) - tolower(*s2)` loop is therefore wrong here even
 * though it is right on a modern system.  Folding 'A'..'Z' by hand is also
 * locale-independent, which is the semantics the callers that motivated these
 * (header names, option keywords, hostnames) actually want.
 */

#include <sys/types.h>

#include "amixcompat.h"

static int
fold(int c)
{
	if (c >= 'A' && c <= 'Z')
		return (c - 'A' + 'a');
	return (c);
}

int
strcasecmp(const char *s1, const char *s2)
{
	int c1;
	int c2;

	for (;;) {
		c1 = fold((int)(unsigned char)*s1++);
		c2 = fold((int)(unsigned char)*s2++);
		if (c1 != c2)
			return (c1 - c2);
		if (c1 == 0)
			return (0);
	}
}

int
strncasecmp(const char *s1, const char *s2, size_t n)
{
	int c1;
	int c2;

	while (n-- != 0) {
		c1 = fold((int)(unsigned char)*s1++);
		c2 = fold((int)(unsigned char)*s2++);
		if (c1 != c2)
			return (c1 - c2);
		if (c1 == 0)
			return (0);
	}
	return (0);
}
