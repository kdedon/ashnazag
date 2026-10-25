/*
 * strings.h - the BSD/POSIX string header Amix does not have.
 *
 * Copyright (c) 2026 Jussi Alanärä.  BSD-2-Clause; see LICENSE.
 *
 * Amix has <string.h> but no <strings.h>, so `#include <strings.h>` -- which
 * autoconf emits behind HAVE_STRINGS_H, and which any program calling
 * strcasecmp is supposed to use -- fails outright.
 *
 * WHAT IS DELIBERATELY NOT HERE.  The other traditional <strings.h> residents
 * (bcopy, bzero, bcmp, index, rindex, ffs) are NOT declared, because libacompat
 * does not implement them and they are not in Amix's libc.so.1 either -- they
 * live in the BSD compatibility archive under /usr/ucblib.  Declaring a
 * function this library does not provide would turn a clean "undefined symbol"
 * at link time into a puzzle, and the whole point of this shim is to make the
 * libc surface honest.  If you need them, link -lucb and declare them yourself,
 * or use the ANSI equivalents (memmove, memset, memcmp, strchr, strrchr) which
 * Amix does have.
 */

#ifndef _AMIX_STRINGS_H
#define _AMIX_STRINGS_H

#include <sys/types.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * ASCII-only case-insensitive comparison; see amixcompat.h for why these are a
 * trap rather than a plain absence (present in libresolv.a, declared nowhere).
 */
extern int strcasecmp(const char *, const char *);
extern int strncasecmp(const char *, const char *, size_t);

#if defined(__cplusplus)
}
#endif

#endif /* _AMIX_STRINGS_H */
