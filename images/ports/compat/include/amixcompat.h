/*
 * amixcompat.h - the declarations for libacompat, the Amix (Amiga Unix,
 * SVR4.0 / m68k, 1992) missing-libc shim.
 *
 * Copyright (c) 2026 Jussi Alanärä.  BSD-2-Clause; see LICENSE.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS FOR
 *
 * Amix's libc predates almost everything a modern C program assumes.  Each
 * function here was chosen because it is MEASURABLY absent -- not absent by
 * reputation.  The measurement, reproducible with the cross toolchain's nm
 * against $SYSROOT/usr/lib/libc.so.1:
 *
 *     snprintf     absent      setenv     absent      mkstemp   absent
 *     vsnprintf    absent      unsetenv   absent      usleep    absent
 *     strcasecmp   absent      strncasecmp absent     gettimeofday absent
 *
 * and the headers stdint.h and strings.h do not exist at all, while ssize_t
 * appears nowhere in <sys/types.h>.
 *
 * ---------------------------------------------------------------------------
 * TWO THINGS A CONSUMER MUST KNOW
 *
 * 1. DO NOT COMPILE WITH -ansi OR -D_POSIX_SOURCE.
 *
 *    131 headers in the Amix sysroot gate their contents on __STDC__, and on
 *    this vintage the ANSI view is the SMALLER one -- <sys/signal.h> hides
 *    `struct sigaction` itself behind `(__STDC__ - 0 == 0) || _POSIX_SOURCE`.
 *    <sys/time.h> hides `struct timeval` behind `!defined(_POSIX_SOURCE)`,
 *    which is exactly the type gettimeofday() below needs.  If a configure
 *    script reports a facility "missing", try -U__STDC__ before believing it.
 *
 *    libacompat itself does NOT require -U__STDC__: it was built and measured
 *    both ways and the object code is identical.  The rule is about what you
 *    must not ADD, not about what you must add.
 *
 * 2. strcasecmp IS A TRAP, NOT MERELY ABSENT.
 *
 *    It is absent from libc but PRESENT in libresolv.a (and the other
 *    networking archives), and declared in NO system header.  So a program
 *    that links -lresolv finds it at link time having compiled every call
 *    against gcc 2.7's implicit `int strcasecmp()` declaration.  That is why
 *    this header exists at all for those two functions: the declaration is the
 *    deliverable, as much as the code is.
 *
 *    If you link both -lresolv and -lacompat, whichever archive the linker
 *    reaches first wins.  Both implementations agree on ASCII; neither is
 *    locale-aware.
 *
 * ---------------------------------------------------------------------------
 * USAGE
 *
 *     cc -I/usr/local/include ... -L/usr/local/lib -lacompat
 *
 * The archive is libacompat.a and NOT libamixcompat.a on purpose: the Amix
 * root filesystem is s5, whose directory entries truncate at 14 bytes, and
 * "libamixcompat.a" is 15.  It would install under the name "libamixcompat."
 * and no -l flag would ever find it.
 */

#ifndef _AMIXCOMPAT_H
#define _AMIXCOMPAT_H

#include <stdarg.h>
#include <sys/types.h>

/*
 * ssize_t - a signed size, and SVR4.0's <sys/types.h> has no such typedef
 * (grep it: the name does not occur).  On this ABI (m68k, ILP32) plain long is
 * the right width and signedness.
 *
 * It is a real typedef rather than a #define, because a macro named ssize_t
 * rewrites any LATER declaration of the same name into something unreadable.
 * A consumer that already has its own ssize_t compiles this away with
 * -D_AMIX_HAVE_SSIZE_T; C89 has no "identical typedef is fine" rule to fall
 * back on.
 */
#ifndef _AMIX_HAVE_SSIZE_T
#define _AMIX_HAVE_SSIZE_T
typedef long ssize_t;
#endif

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * snprintf / vsnprintf - C99 semantics, including the return value.
 *
 * Both return the number of characters that WOULD have been written had the
 * buffer been large enough, excluding the terminating NUL.  That is the whole
 * reason modern code calls them, and the SVR4-era "return -1 on truncation"
 * behaviour some 1990s shims shipped is a silent source of infinite loops.
 *
 * A size of 0 is legal and str may then be NULL; nothing is written and the
 * would-be length is still returned.
 *
 * Supported: the flags - + space # 0, a field width (or *), a precision (or
 * .*), the length modifiers hh h l ll L z t j, and the conversions
 * d i o u x X c s p % and e E f F g G.
 *
 * %n is DELIBERATELY NOT SUPPORTED - it is a format-string-attack primitive
 * with no legitimate use in this codebase's consumers, and it is treated as an
 * unknown conversion (emitted literally).
 *
 * LONG LONG IS THE POINT.  SVR4.0's printf family has no ll modifier at all,
 * and %llu / %Lu is systemic in the code this repo wants to port (bzip2recover,
 * coreutils, dropbear's libtom*).  One long-long-capable snprintf closes that
 * whole class, which is why this function is written here rather than wrapped
 * around libc's _doprnt.
 *
 * FLOATING POINT IS DELEGATED, NOT REIMPLEMENTED.  e E f F g G are handed to
 * libc's own sprintf with a reconstructed one-conversion format string, so the
 * digits are exactly what every other program on the box produces.  Two
 * consequences worth stating out loud:
 *   - On a part with no 68881/68882 those conversions die with SIGSYS inside
 *     libc's _doprnt, exactly as they would without this shim.  The shim does
 *     not add an FPU dependency; it does not remove one either.
 *   - libc's own float PARSING (atof/strtod) drifts above 1e50.  Printing is
 *     unaffected; this note is here because the two get confused.
 */
extern int amix_snprintf(char * /*str*/, size_t /*size*/,
    const char * /*fmt*/, ...);
extern int amix_vsnprintf(char * /*str*/, size_t /*size*/,
    const char * /*fmt*/, va_list /*ap*/);

#ifndef AMIX_NO_LIBC_NAMES
extern int snprintf(char *, size_t, const char *, ...);
extern int vsnprintf(char *, size_t, const char *, va_list);
#endif

/*
 * setenv / unsetenv - BSD environment editing over SVR4's putenv.
 *
 * putenv(3) exists on Amix but KEEPS THE CALLER'S POINTER, so the classic
 * `putenv("X=1")` with a stack buffer leaves a dangling entry in environ.
 * setenv() here always installs a heap copy and frees the copy it replaces,
 * so repeated setenv() of the same name does not grow without bound.  Entries
 * that were already in environ when the process started are never freed --
 * they are not ours.
 *
 * Returns 0 on success, -1 with errno set (EINVAL for a NULL/empty name or a
 * name containing '=', ENOMEM on allocation failure).
 */
extern int setenv(const char * /*name*/, const char * /*value*/,
    int /*overwrite*/);
extern int unsetenv(const char * /*name*/);

/*
 * environ - and it is spelled _environ on this platform.
 *
 * Measured against the sysroot:
 *
 *     libc.so.1     B _environ                 <- and nothing else
 *     libc.a        D _environ,  W environ     <- both
 *     /usr/include  neither, in any header
 *
 * So the universal `extern char **environ;` compiles, links against the STATIC
 * libc, and fails at link time against the SHARED one -- which is what an Amix
 * program actually links.  The failure arrives as an undefined symbol in the
 * consumer's own object, nowhere near the cause.
 *
 * The macro below fixes both the declaration and every use, because it rewrites
 * the identifier itself: a consumer's own `extern char **environ;` becomes a
 * declaration of _environ, and `environ[i]` indexes the real array rather than
 * a private copy.  Defining a separate `environ` VARIABLE in this library would
 * link just as cleanly and be silently wrong -- putenv updates _environ, so the
 * copy would go stale on the first change.
 *
 * This is not an invention: <rpc/types.h> already does exactly this for
 * gettimeofday.  Compile with -DAMIX_NO_ENVIRON_ALIAS to turn it off if a
 * consumer has a struct member or local variable of that name.
 */
#ifndef AMIX_NO_ENVIRON_ALIAS
extern char **_environ;
#define environ _environ
#endif

/*
 * strcasecmp / strncasecmp - ASCII-only, deliberately.
 *
 * SVR4 tolower() is defined only for characters that ARE upper case, so the
 * obvious tolower()-based loop is undefined for everything else.  These fold
 * by hand over 'A'..'Z' and nothing else, which also means they do not change
 * behaviour with LC_CTYPE.  For the uses that motivated them -- header names,
 * option keywords, hostnames -- that is the correct semantics, not a shortcut.
 */
extern int strcasecmp(const char * /*s1*/, const char * /*s2*/);
extern int strncasecmp(const char * /*s1*/, const char * /*s2*/, size_t /*n*/);

/*
 * mkstemp - create and open a unique file, O_EXCL.
 *
 * Built on SVR4 mktemp(3), which DOES exist, plus the O_EXCL create mktemp
 * itself never performed: mktemp only picks a name, and the gap between
 * picking and opening is the race mkstemp exists to close.
 *
 * The template's trailing XXXXXX is overwritten in place, as usual.  On EEXIST
 * the pristine template is restored and another name is tried, a few times.
 *
 * s5 NOTE, and it is not academic: on the Amix root filesystem a dirent
 * truncates at 14 bytes, so if the template's BASENAME is 14 characters or
 * longer every name mktemp can generate truncates back onto the same dirent
 * and every O_EXCL create returns EEXIST forever.  Keep template basenames to
 * 8 characters plus XXXXXX.  This is not a hypothetical -- it is the defect
 * that made pkgadd unable to overwrite a long-named file on this exact
 * filesystem.
 */
extern int mkstemp(char * /*template*/);

/*
 * usleep - sleep for microseconds, built on poll(2).
 *
 * poll() with zero descriptors is SVR4's portable sub-second sleep, and it is
 * present in Amix's libc.  Its timeout is in MILLISECONDS, so the resolution
 * here is 1 ms and any non-zero request below that rounds UP to 1 ms rather
 * than down to a busy return -- a usleep that returns immediately turns a
 * polling loop into a spin.
 *
 * The kernel tick is HZ=60 (~16.7 ms), so the true granularity is coarser than
 * the interface suggests.  Do not build timing on it.
 */
extern int usleep(unsigned int /*usec*/);

/*
 * gettimeofday - BSD name, SVR4.0 ABI underneath.
 *
 * Amix's libc exports NO `gettimeofday`.  What it exports is
 * `_abi_gettimeofday`, and <rpc/types.h> line 82 quietly does
 *
 *     #define gettimeofday _abi_gettimeofday
 *
 * so code that happens to include that header links and code that does not,
 * does not.  That asymmetry is worse than a plain absence, because a build
 * probing the SHARED library concludes the function is missing while a build
 * that pulls in the static libc.a finds it.
 *
 * The shipped _abi_gettimeofday in libc.so.1 has this contract,
 * which is NOT the BSD one:
 *
 *   - it takes ONE argument.  It reads %fp@(8) and never touches
 *     %fp@(12); there is no timezone parameter to fill in.
 *   - a NULL argument is not an error: it returns 0 having done nothing.
 *   - it reads the clock via _abi_hrtcntl(1, 1, 0, &buf) and stores buf[0] to
 *     tv_sec and buf[1] to tv_usec, returning -1 if that call fails.
 *
 * So this wrapper accepts the two-argument BSD signature every caller writes,
 * IGNORES the timezone pointer (there is nothing underneath to fill it from),
 * and forwards the timeval.  A caller that wants a timezone must not use this.
 *
 * Callers must not include <rpc/types.h> and expect this symbol: there the
 * macro rewrites the call to _abi_gettimeofday before it is ever seen, which
 * works, but by a different route.
 */
struct timeval;
extern int gettimeofday(struct timeval * /*tp*/, void * /*tzp*/);

/*
 * amix_compat_version - so a smoke test can prove which build it is talking
 * to, rather than assuming the archive on the box is the one just pushed.
 */
extern const char *amix_compat_version(void);

#if defined(__cplusplus)
}
#endif

#endif /* _AMIXCOMPAT_H */
