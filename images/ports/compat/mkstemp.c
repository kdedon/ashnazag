/*
 * mkstemp.c - mkstemp(3) for Amix (SVR4.0, m68k).
 *
 * Copyright (c) 2026 Jussi Alanärä.  BSD-2-Clause; see LICENSE.
 *
 * SVR4.0 has mktemp(3) but not mkstemp(3), and the difference is the whole
 * point of the function: mktemp only CHOOSES a name, and everything between
 * choosing and opening is a race another process can win.  mkstemp closes it by
 * doing the open itself with O_CREAT|O_EXCL, which the kernel makes atomic.
 *
 * ---------------------------------------------------------------------------
 * THE s5 TRAP, WHICH IS NOT HYPOTHETICAL
 *
 * The Amix root filesystem is s5, whose directory entries truncate at 14 bytes
 * ON CREATE AND ON LOOKUP.  If the template's BASENAME is 14 characters or
 * longer, every candidate name mktemp can generate truncates back onto the same
 * dirent -- so every O_EXCL create returns EEXIST, forever, and mktemp
 * eventually gives up and blanks the template.
 *
 * That is not a theory.  It is the exact defect that made SVR4 pkgadd unable to
 * overwrite any file whose basename was 14 characters or longer on this
 * filesystem: the fix there was to cap the template's basename at 8 characters
 * plus XXXXXX, so the X's survive truncation instead of being the part eaten.
 *
 * This implementation cannot rename the caller's template, so instead it
 * DETECTS the shape and fails fast with EEXIST rather than spinning: a caller
 * that hands us a 14-byte basename has a bug we cannot fix from here, and the
 * kind thing is to stop immediately instead of after ten identical failures.
 * The condition is checked once, before the loop, so the common case pays
 * nothing.
 */

#include <stdio.h>
/*
 * <stdlib.h> is what declares mktemp -- `extern char *mktemp(char *)` at line
 * 105 of the Amix header.  Leaving it out does not fail to compile: gcc 2.7
 * invents `int mktemp()` and the returned POINTER is then truncated through an
 * int.  That is PORTING.md sec 3.1's implicit-declaration trap in its exact
 * canonical form, and the build script's -Wall gate is what caught it here.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "amixcompat.h"

#define TEMPLATE_MAX	1024
#define TRIES		16

/*
 * The s5 dirent limit.  Named rather than spelled 14 inline, because the same
 * number appears in the packaging gate and in the prototype scan, and a reader
 * who greps for it should find a name and a reason.
 */
#define S5_NAMELEN	14

int
mkstemp(char *tmpl)
{
	char saved[TEMPLATE_MAX];
	const char *base;
	const char *slash;
	size_t len;
	int fd;
	int tries;

	if (tmpl == NULL) {
		errno = EINVAL;
		return (-1);
	}
	len = strlen(tmpl);
	if (len >= sizeof(saved)) {
		errno = EINVAL;
		return (-1);
	}
	if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) {
		errno = EINVAL;
		return (-1);
	}

	slash = strrchr(tmpl, '/');
	base = (slash != NULL) ? slash + 1 : tmpl;
	if (strlen(base) >= (size_t)S5_NAMELEN) {
		/*
		 * See the header comment: on s5 every generated name would
		 * truncate onto the same dirent, so no O_EXCL create can ever
		 * succeed.  Fail now, with the errno the caller would have got
		 * eventually anyway.
		 */
		errno = EEXIST;
		return (-1);
	}

	(void)strcpy(saved, tmpl);

	for (tries = 0; tries < TRIES; tries++) {
		if (mktemp(tmpl) == NULL || tmpl[0] == '\0') {
			errno = EEXIST;
			return (-1);
		}
		fd = open(tmpl, O_CREAT | O_EXCL | O_RDWR, 0600);
		if (fd >= 0)
			return (fd);
		if (errno != EEXIST)
			return (-1);
		/* Lost the race; mktemp consumed the template, so restore it. */
		(void)strcpy(tmpl, saved);
	}
	errno = EEXIST;
	return (-1);
}
