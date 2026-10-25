/*
 * snprintf.c - C99 snprintf/vsnprintf for Amix (SVR4.0, m68k).
 *
 * Copyright (c) 2026 Jussi Alanärä.  BSD-2-Clause; see LICENSE.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS WRITTEN OUT RATHER THAN WRAPPED AROUND libc
 *
 * The tempting three-line shim is
 *
 *     int snprintf(char *b, size_t n, const char *f, ...) { ... vsprintf ... }
 *
 * and it is wrong twice over.  It cannot bound the write at all -- vsprintf has
 * no size argument, which is the entire point of the function being added --
 * and, decisively, SVR4.0's printf family HAS NO `ll` LENGTH MODIFIER.  %llu
 * against Amix's _doprnt does not print a 64-bit number; it consumes the wrong
 * number of argument bytes and desynchronises everything after it in the same
 * format string.
 *
 * %llu / %Lu is systemic in the software this repo exists to port -- bzip2's
 * bzip2recover, coreutils, dropbear's libtom* -- so one long-long-capable
 * snprintf written once closes that whole class of blocker.  That is why the
 * integer path here is home-made and the code is longer than a wrapper.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS DELEGATED, AND WHY THAT IS THE RIGHT CALL
 *
 * The floating-point conversions (e E f F g G) are handed to libc's own
 * sprintf with a reconstructed single-conversion format string.  Reimplementing
 * correctly-rounded binary-to-decimal conversion is a large, subtle job whose
 * only reward here would be output that DISAGREES with every other program on
 * the box.  Delegating means %f prints what printf prints.
 *
 * Two properties of the box that this inherits and does not change:
 *
 *   - On a part with no 68881/68882, any float conversion dies with SIGSYS
 *     inside libc's _doprnt.  Stock Amix requires an FPU; the FPE kernels
 *     emulate one.  The shim neither adds nor removes that dependency.
 *   - libc's float PARSING (atof/strtod) drifts above 1e50.  That is a
 *     different function; printing is unaffected.
 *
 * Width and precision are passed through to libc as part of the reconstructed
 * spec, so libc also does the padding for float conversions.  Getting
 * zero-padding of a negative number right is exactly the kind of detail that a
 * hand-rolled pad-then-copy gets wrong ("-001.5" vs "00-1.5"), so it is not
 * hand-rolled.
 *
 * ---------------------------------------------------------------------------
 * DIALECT
 *
 * C89 as gcc 2.7.2.3 accepts it: no declarations after statements, no
 * loop-scoped indices, no // comments.  `long long` IS used -- it is a
 * documented, measured gcc 2.7 extension on this target, and it is the feature
 * being delivered.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>

#include "amixcompat.h"

/* --- flags -------------------------------------------------------------- */

#define F_LEFT		0x01	/* '-' */
#define F_PLUS		0x02	/* '+' */
#define F_SPACE		0x04	/* ' ' */
#define F_ALT		0x08	/* '#' */
#define F_ZERO		0x10	/* '0' */

/* --- length modifiers ---------------------------------------------------- */

#define L_INT		0
#define L_CHAR		1	/* hh */
#define L_SHORT		2	/* h  */
#define L_LONG		3	/* l  */
#define L_LLONG		4	/* ll, j */
#define L_SIZE		5	/* z, t */
#define L_LDOUBLE	6	/* L  */

/*
 * The bounded output sink.
 *
 * `len` counts every character the format WOULD have produced, whether or not
 * it fit; that count is snprintf's return value and the reason callers can size
 * a buffer with one dry run.  A shim that returns the truncated length instead
 * turns the standard "call once to measure, malloc, call again" idiom into an
 * infinite loop, so this is not a detail.
 */
struct sink {
	char	*buf;
	size_t	cap;
	size_t	len;
};

static void
sput(struct sink *s, int c)
{
	if (s->buf != NULL && s->len + 1 < s->cap)
		s->buf[s->len] = (char)c;
	s->len++;
}

static void
spad(struct sink *s, int c, int n)
{
	while (n-- > 0)
		sput(s, c);
}

static void
sputn(struct sink *s, const char *p, int n)
{
	while (n-- > 0)
		sput(s, *p++);
}

/*
 * Unsigned to ASCII, digits produced least-significant first.
 *
 * Base 10 on a 64-bit value becomes a libgcc __udivdi3/__umoddi3 call pair.
 * That matters for one reason beyond speed: ROADMAP D4 requires that nothing
 * shipped carries a mulu.l/muls.l with a Dh:Dl destination, because the
 * 68LC060's vector-61 handler emulates only the immediate-source form and the
 * others take a SIGKILL.  A scan of this toolchain's libgcc.a finds no such
 * instruction anywhere in it, so routing 64-bit division through libgcc is the
 * 060-safe choice as well as the simple one.  The build script re-runs that
 * gate on the archive rather than trusting this comment.
 */
static int
utoa(unsigned long long v, unsigned int base, int upper, char *out)
{
	static const char lower_digits[] = "0123456789abcdef";
	static const char upper_digits[] = "0123456789ABCDEF";
	const char *dig;
	int n;

	dig = upper ? upper_digits : lower_digits;
	n = 0;
	if (v == 0) {
		out[n++] = '0';
		return (n);
	}
	while (v != 0) {
		out[n++] = dig[(unsigned int)(v % (unsigned long long)base)];
		v /= (unsigned long long)base;
	}
	return (n);
}

/*
 * Format one floating-point conversion through libc sprintf.
 *
 * Returns 0 on success, -1 if a buffer could not be allocated.  The size
 * calculation has to cover the worst case honestly: a %f of DBL_MAX is 309
 * integer digits before the precision even starts, and the caller's own field
 * width can be larger than either.  So: digits + precision + width + slack, on
 * the stack when it fits and on the heap when it does not.  Silently clamping
 * the precision instead would produce confidently wrong output, which is worse
 * than an honest allocation failure.
 */
/* A double's bits, copied with integer moves so no FPU is touched here. */
struct dbits { unsigned long hi, lo; };
struct xbits { unsigned long se, hi, lo; };

/* Narrow a 96-bit extended to double by truncation, in integer code. */
static struct dbits
xtod(struct xbits x)
{
	struct dbits d;
	long e;

	e = (long)((x.se >> 16) & 0x7fff);
	d.hi = x.se & 0x80000000UL;
	d.lo = 0;
	if (e == 0x7fff) {
		d.hi |= 0x7ff00000UL;
		if ((x.hi & 0x7fffffffUL) != 0 || x.lo != 0)
			d.hi |= 0x00080000UL;	/* NaN */
		return (d);
	}
	if (x.hi == 0 || (e -= 16383 - 1023) <= 0)
		return (d);
	if (e >= 0x7ff) {
		d.hi |= 0x7ff00000UL;
		return (d);
	}
	d.hi |= (unsigned long)e << 20 | (x.hi & 0x7fffffffUL) >> 11;
	d.lo = x.hi << 21 | x.lo >> 11;
	return (d);
}

static int
fmt_double(struct sink *s, const char *spec, struct dbits d)
{
	char stackbuf[512];
	char *buf;
	int n;

	buf = stackbuf;
	/*
	 * Ask libc how long it will be.  There is no snprintf to ask with --
	 * that is what we are implementing -- so bound it by construction
	 * instead: worst-case %f magnitude (309 digits) + the spec's own width
	 * and precision, both already baked into `spec` as decimal text.
	 */
	{
		const char *p;
		long want;
		long acc;
		int seen_dot;

		want = 340L;		/* DBL_MAX integer digits + sign + slack */
		acc = 0;
		seen_dot = 0;
		for (p = spec; *p != '\0'; p++) {
			if (*p == '.') {
				want += acc;	/* the width parsed so far */
				acc = 0;
				seen_dot = 1;
			} else if (*p >= '0' && *p <= '9') {
				if (acc < 100000L)
					acc = acc * 10 + (*p - '0');
			}
		}
		want += acc;
		if (!seen_dot)
			want += 6;	/* default precision */
		want += 32;
		if (want > (long)sizeof(stackbuf)) {
			buf = (char *)malloc((size_t)want);
			if (buf == NULL) {
				errno = ENOMEM;
				return (-1);
			}
		}
	}

	/*
	 * The length is taken with strlen rather than from sprintf's return
	 * value.  SVR4 sprintf does return an int count -- Amix's <stdio.h>
	 * declares it that way -- but the pre-SVR4 spelling returned char*,
	 * and this file is compiled against 1992 headers whose visible
	 * declaration depends on the __STDC__ compilation mode.  strlen is
	 * correct under every one of those views.
	 */
	sprintf(buf, spec, d);
	n = (int)strlen(buf);
	if (n > 0)
		sputn(s, buf, n);
	if (buf != stackbuf)
		free(buf);
	return (0);
}

int
amix_vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
	struct sink s;
	const char *f;
	char numbuf[32];		/* 64-bit octal is 22 digits + slack */
	char spec[40];
	int flags;
	int width;
	int prec;
	int lmod;
	int conv;
	int i;
	int ndig;
	int nlen;
	int sign;
	int fail;
	unsigned long long uval;
	long long sval;
	const char *sarg;
	char prefix[4];
	int plen;
	int zeros;
	int bodylen;

	s.buf = str;
	s.cap = size;
	s.len = 0;
	fail = 0;

	for (f = fmt; *f != '\0'; f++) {
		if (*f != '%') {
			sput(&s, (unsigned char)*f);
			continue;
		}
		f++;
		if (*f == '%') {
			sput(&s, '%');
			continue;
		}

		flags = 0;
		for (;;) {
			if (*f == '-')		flags |= F_LEFT;
			else if (*f == '+')	flags |= F_PLUS;
			else if (*f == ' ')	flags |= F_SPACE;
			else if (*f == '#')	flags |= F_ALT;
			else if (*f == '0')	flags |= F_ZERO;
			else			break;
			f++;
		}

		width = 0;
		if (*f == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				flags |= F_LEFT;
				width = -width;
			}
			f++;
		} else {
			while (*f >= '0' && *f <= '9') {
				width = width * 10 + (*f - '0');
				f++;
			}
		}

		prec = -1;
		if (*f == '.') {
			f++;
			if (*f == '*') {
				prec = va_arg(ap, int);
				f++;
				if (prec < 0)
					prec = -1;
			} else {
				prec = 0;
				while (*f >= '0' && *f <= '9') {
					prec = prec * 10 + (*f - '0');
					f++;
				}
			}
		}

		lmod = L_INT;
		if (*f == 'h') {
			f++;
			if (*f == 'h') { lmod = L_CHAR; f++; }
			else		 lmod = L_SHORT;
		} else if (*f == 'l') {
			f++;
			if (*f == 'l') { lmod = L_LLONG; f++; }
			else		 lmod = L_LONG;
		} else if (*f == 'L') {
			lmod = L_LDOUBLE;
			f++;
		} else if (*f == 'j') {
			lmod = L_LLONG;
			f++;
		} else if (*f == 'z' || *f == 't') {
			lmod = L_SIZE;
			f++;
		} else if (*f == 'q') {
			/* BSD spelling of ll; era code uses it. */
			lmod = L_LLONG;
			f++;
		}

		conv = (unsigned char)*f;
		if (conv == '\0') {
			/* Trailing '%' with no conversion: emit it literally. */
			sput(&s, '%');
			f--;
			continue;
		}

		switch (conv) {
		case 'd':
		case 'i':
			if (lmod == L_LLONG)		sval = va_arg(ap, long long);
			else if (lmod == L_LONG)	sval = (long long)va_arg(ap, long);
			else if (lmod == L_SIZE)	sval = (long long)va_arg(ap, long);
			else if (lmod == L_SHORT)	sval = (long long)(short)va_arg(ap, int);
			else if (lmod == L_CHAR)	sval = (long long)(signed char)va_arg(ap, int);
			else				sval = (long long)va_arg(ap, int);
			sign = 0;
			if (sval < 0) {
				sign = 1;
				/*
				 * Negate in the UNSIGNED domain.  -LLONG_MIN is
				 * not representable, and doing it as a signed
				 * negate is undefined behaviour that gcc 2.7 is
				 * entirely willing to fold into something wrong.
				 */
				uval = (unsigned long long)(-(sval + 1)) + 1ULL;
			} else {
				uval = (unsigned long long)sval;
			}
			ndig = utoa(uval, 10, 0, numbuf);
			plen = 0;
			if (sign)			prefix[plen++] = '-';
			else if (flags & F_PLUS)	prefix[plen++] = '+';
			else if (flags & F_SPACE)	prefix[plen++] = ' ';
			goto emit_integer;

		case 'u':
		case 'o':
		case 'x':
		case 'X':
			if (lmod == L_LLONG)		uval = va_arg(ap, unsigned long long);
			else if (lmod == L_LONG)	uval = (unsigned long long)va_arg(ap, unsigned long);
			else if (lmod == L_SIZE)	uval = (unsigned long long)va_arg(ap, unsigned long);
			else if (lmod == L_SHORT)	uval = (unsigned long long)(unsigned short)va_arg(ap, unsigned int);
			else if (lmod == L_CHAR)	uval = (unsigned long long)(unsigned char)va_arg(ap, unsigned int);
			else				uval = (unsigned long long)va_arg(ap, unsigned int);
			plen = 0;
			if (conv == 'u') {
				ndig = utoa(uval, 10, 0, numbuf);
			} else if (conv == 'o') {
				ndig = utoa(uval, 8, 0, numbuf);
				if ((flags & F_ALT) != 0 && numbuf[ndig - 1] != '0')
					numbuf[ndig++] = '0';
			} else {
				ndig = utoa(uval, 16, conv == 'X', numbuf);
				if ((flags & F_ALT) != 0 && uval != 0) {
					prefix[plen++] = '0';
					prefix[plen++] = (char)conv;
				}
			}
			goto emit_integer;

		emit_integer:
			/*
			 * C99: an explicit precision on an integer conversion
			 * turns OFF the '0' flag, and a precision of 0 with a
			 * value of 0 produces NO characters at all.
			 */
			if (prec >= 0)
				flags &= ~F_ZERO;
			if (prec == 0 && uval == 0)
				ndig = 0;
			zeros = 0;
			if (prec > ndig)
				zeros = prec - ndig;
			bodylen = plen + zeros + ndig;
			if ((flags & F_ZERO) != 0 && (flags & F_LEFT) == 0 &&
			    width > bodylen) {
				zeros += width - bodylen;
				bodylen = width;
			}
			if ((flags & F_LEFT) == 0)
				spad(&s, ' ', width - bodylen);
			for (i = 0; i < plen; i++)
				sput(&s, (unsigned char)prefix[i]);
			spad(&s, '0', zeros);
			for (i = ndig - 1; i >= 0; i--)
				sput(&s, (unsigned char)numbuf[i]);
			if ((flags & F_LEFT) != 0)
				spad(&s, ' ', width - bodylen);
			break;

		case 'c':
			numbuf[0] = (char)va_arg(ap, int);
			if ((flags & F_LEFT) == 0)
				spad(&s, ' ', width - 1);
			sput(&s, (unsigned char)numbuf[0]);
			if ((flags & F_LEFT) != 0)
				spad(&s, ' ', width - 1);
			break;

		case 's':
			sarg = va_arg(ap, const char *);
			/*
			 * A NULL %s is undefined behaviour, and the honest
			 * choices are "crash" or "say so".  Saying so has saved
			 * more debugging time than it has hidden bugs, and it
			 * matches what the platform's own printf does.
			 */
			if (sarg == NULL)
				sarg = "(null)";
			if (prec >= 0) {
				nlen = 0;
				while (nlen < prec && sarg[nlen] != '\0')
					nlen++;
			} else {
				nlen = (int)strlen(sarg);
			}
			if ((flags & F_LEFT) == 0)
				spad(&s, ' ', width - nlen);
			sputn(&s, sarg, nlen);
			if ((flags & F_LEFT) != 0)
				spad(&s, ' ', width - nlen);
			break;

		case 'p':
			/*
			 * Pointers are 32 bits on this ABI.  Printed as
			 * 0x<hex>, NULL included ("0x0") rather than glibc's
			 * "(nil)" -- the SVR4/BSD spelling, and the one a
			 * reader on this box will recognise.
			 */
			uval = (unsigned long long)(unsigned long)va_arg(ap, void *);
			ndig = utoa(uval, 16, 0, numbuf);
			plen = 0;
			prefix[plen++] = '0';
			prefix[plen++] = 'x';
			flags &= ~F_ZERO;
			bodylen = plen + ndig;
			if ((flags & F_LEFT) == 0)
				spad(&s, ' ', width - bodylen);
			for (i = 0; i < plen; i++)
				sput(&s, (unsigned char)prefix[i]);
			for (i = ndig - 1; i >= 0; i--)
				sput(&s, (unsigned char)numbuf[i]);
			if ((flags & F_LEFT) != 0)
				spad(&s, ' ', width - bodylen);
			break;

		case 'e':
		case 'E':
		case 'f':
		case 'F':
		case 'g':
		case 'G': {
			struct dbits dval;
			int si;

			if (lmod == L_LDOUBLE) {
				/*
				 * gcc 2.7 on m68k has a 96-bit long double, but
				 * Amix's libc printf has no %Lf to hand it to.
				 * Narrowing to double is the only thing that can
				 * be done here, and it is done VISIBLY rather
				 * than by pretending the modifier was absent.
				 */
				dval = xtod(va_arg(ap, struct xbits));
			} else {
				dval = va_arg(ap, struct dbits);
			}
			si = 0;
			spec[si++] = '%';
			if ((flags & F_LEFT) != 0)	spec[si++] = '-';
			if ((flags & F_PLUS) != 0)	spec[si++] = '+';
			if ((flags & F_SPACE) != 0)	spec[si++] = ' ';
			if ((flags & F_ALT) != 0)	spec[si++] = '#';
			if ((flags & F_ZERO) != 0)	spec[si++] = '0';
			if (width > 0) {
				sprintf(spec + si, "%d", width);
				si += (int)strlen(spec + si);
			}
			if (prec >= 0) {
				sprintf(spec + si, ".%d", prec);
				si += (int)strlen(spec + si);
			}
			spec[si++] = (char)conv;
			spec[si] = '\0';
			if (fmt_double(&s, spec, dval) != 0)
				fail = 1;
			break;
		}

		default:
			/*
			 * Unknown conversion, %n included.  %n is not supported
			 * ON PURPOSE: it is the format-string-attack primitive,
			 * nothing in this repo's consumers needs it, and a shim
			 * that quietly implements it is a shim that hands every
			 * consumer a write-anywhere gadget.  Unknown specifiers
			 * are echoed literally so the defect is visible in the
			 * output instead of swallowed.
			 */
			sput(&s, '%');
			sput(&s, conv);
			break;
		}
	}

	if (s.buf != NULL && s.cap > 0)
		s.buf[s.len < s.cap ? s.len : s.cap - 1] = '\0';

	if (fail)
		return (-1);
	return ((int)s.len);
}

int
amix_snprintf(char *str, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = amix_vsnprintf(str, size, fmt, ap);
	va_end(ap);
	return (n);
}

#ifndef AMIX_NO_LIBC_NAMES
int
vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
	return (amix_vsnprintf(str, size, fmt, ap));
}

int
snprintf(char *str, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = amix_vsnprintf(str, size, fmt, ap);
	va_end(ap);
	return (n);
}
#endif
