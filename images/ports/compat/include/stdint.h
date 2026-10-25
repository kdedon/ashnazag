/*
 * stdint.h - C99 exact-width integer types for Amix (SVR4.0, m68k).
 *
 * Copyright (c) 2026 Jussi Alanärä.  BSD-2-Clause; see LICENSE.
 *
 * Amix has no <stdint.h> -- the file simply does not exist in /usr/include --
 * and a great deal of otherwise portable C89 code includes it anyway, because
 * "C89 code" in 2026 usually means "C99-typed code with C89 syntax".  libtom*
 * (dropbear's crypto) is the concrete consumer this was written for.
 *
 * THE WIDTHS ARE THIS TARGET'S, NOT A GUESS.  m68k SVR4 is big-endian ILP32:
 *
 *     char 8   short 16   int 32   long 32   long long 64   pointer 32
 *
 * long long is a gcc 2.7.2.3 extension here, not C89, and it is available:
 * libgcc supplies __udivdi3/__umoddi3/__lshrdi3, and a scan of that libgcc.a
 * shows it contains no mulu.l/muls.l at all -- so 64-bit arithmetic through it
 * is also safe on the 68LC060, whose vector-61 handler emulates only the
 * immediate-source form of those instructions.
 *
 * This header is installed into /usr/local/include, NEVER over /usr/include.
 * It shadows nothing: there is nothing there to shadow.
 */

#ifndef _AMIX_STDINT_H
#define _AMIX_STDINT_H

/* --- exact-width types -------------------------------------------------- */

typedef signed char		int8_t;
typedef unsigned char		uint8_t;
typedef short			int16_t;
typedef unsigned short		uint16_t;
typedef int			int32_t;
typedef unsigned int		uint32_t;
typedef long long		int64_t;
typedef unsigned long long	uint64_t;

/*
 * The least/fast families are the same types here.  On a 32-bit m68k there is
 * nothing to gain by widening a "fast" 8-bit type to a register: the 68020's
 * byte and word operations are single instructions.
 */
typedef int8_t			int_least8_t;
typedef uint8_t			uint_least8_t;
typedef int16_t			int_least16_t;
typedef uint16_t		uint_least16_t;
typedef int32_t			int_least32_t;
typedef uint32_t		uint_least32_t;
typedef int64_t			int_least64_t;
typedef uint64_t		uint_least64_t;

typedef int32_t			int_fast8_t;
typedef uint32_t		uint_fast8_t;
typedef int32_t			int_fast16_t;
typedef uint32_t		uint_fast16_t;
typedef int32_t			int_fast32_t;
typedef uint32_t		uint_fast32_t;
typedef int64_t			int_fast64_t;
typedef uint64_t		uint_fast64_t;

/* Pointers are 32 bits; long is the type that round-trips one. */
typedef long			intptr_t;
typedef unsigned long		uintptr_t;

typedef int64_t			intmax_t;
typedef uint64_t		uintmax_t;

/* --- limits -------------------------------------------------------------- */

#define INT8_MIN		(-128)
#define INT8_MAX		127
#define UINT8_MAX		255U
#define INT16_MIN		(-32768)
#define INT16_MAX		32767
#define UINT16_MAX		65535U
#define INT32_MIN		(-2147483647 - 1)
#define INT32_MAX		2147483647
#define UINT32_MAX		4294967295U
#define INT64_MIN		(-9223372036854775807LL - 1)
#define INT64_MAX		9223372036854775807LL
#define UINT64_MAX		18446744073709551615ULL

#define INT_LEAST8_MIN		INT8_MIN
#define INT_LEAST8_MAX		INT8_MAX
#define UINT_LEAST8_MAX		UINT8_MAX
#define INT_LEAST16_MIN		INT16_MIN
#define INT_LEAST16_MAX		INT16_MAX
#define UINT_LEAST16_MAX	UINT16_MAX
#define INT_LEAST32_MIN		INT32_MIN
#define INT_LEAST32_MAX		INT32_MAX
#define UINT_LEAST32_MAX	UINT32_MAX
#define INT_LEAST64_MIN		INT64_MIN
#define INT_LEAST64_MAX		INT64_MAX
#define UINT_LEAST64_MAX	UINT64_MAX

#define INT_FAST8_MIN		INT32_MIN
#define INT_FAST8_MAX		INT32_MAX
#define UINT_FAST8_MAX		UINT32_MAX
#define INT_FAST16_MIN		INT32_MIN
#define INT_FAST16_MAX		INT32_MAX
#define UINT_FAST16_MAX		UINT32_MAX
#define INT_FAST32_MIN		INT32_MIN
#define INT_FAST32_MAX		INT32_MAX
#define UINT_FAST32_MAX		UINT32_MAX
#define INT_FAST64_MIN		INT64_MIN
#define INT_FAST64_MAX		INT64_MAX
#define UINT_FAST64_MAX		UINT64_MAX

#define INTPTR_MIN		INT32_MIN
#define INTPTR_MAX		INT32_MAX
#define UINTPTR_MAX		UINT32_MAX
#define INTMAX_MIN		INT64_MIN
#define INTMAX_MAX		INT64_MAX
#define UINTMAX_MAX		UINT64_MAX

#define PTRDIFF_MIN		INT32_MIN
#define PTRDIFF_MAX		INT32_MAX
#define SIZE_MAX		UINT32_MAX
#define SIG_ATOMIC_MIN		INT32_MIN
#define SIG_ATOMIC_MAX		INT32_MAX

/*
 * WCHAR_MIN/MAX: Amix's wchar_t is a signed 32-bit type (<stdlib.h>'s
 * typedef).  Guarded because <limits.h> or a vendored wchar shim may have
 * defined them already.
 */
#ifndef WCHAR_MIN
#define WCHAR_MIN		INT32_MIN
#endif
#ifndef WCHAR_MAX
#define WCHAR_MAX		INT32_MAX
#endif
#define WINT_MIN		INT32_MIN
#define WINT_MAX		INT32_MAX

/* --- constant macros ----------------------------------------------------- */

#define INT8_C(c)		c
#define UINT8_C(c)		c ## U
#define INT16_C(c)		c
#define UINT16_C(c)		c ## U
#define INT32_C(c)		c
#define UINT32_C(c)		c ## U
#define INT64_C(c)		c ## LL
#define UINT64_C(c)		c ## ULL
#define INTMAX_C(c)		c ## LL
#define UINTMAX_C(c)		c ## ULL

#endif /* _AMIX_STDINT_H */
