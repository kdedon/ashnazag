/*
 * fastmem.c -- memcpy, memmove, memset, bcopy, bzero and sqrt for our
 * programs, linked ahead of libc and libm.  libc's copies move a byte
 * at a time, and on a frame buffer across Zorro III or PCI every store
 * costs a bus cycle whatever its width.  libm's sqrt is a software
 * routine; the FPU has fsqrt.
 *
 * Copies align the destination, then move four longs a pass.  Card
 * memory is never read unless it is the source.
 */
#include <sys/types.h>

typedef unsigned char uchar;

#define	ALIGN(p)	((unsigned long)(p) & 3)

static void
fwd(d, s, n)
	register uchar *d, *s;
	register size_t n;
{
	register ulong *dl, *sl;

	if (n >= 8) {
		while (ALIGN(d))
			*d++ = *s++, n--;
		dl = (ulong *)d;
		sl = (ulong *)s;
		for (; n >= 16; n -= 16) {
			*dl++ = *sl++;
			*dl++ = *sl++;
			*dl++ = *sl++;
			*dl++ = *sl++;
		}
		for (; n >= 4; n -= 4)
			*dl++ = *sl++;
		d = (uchar *)dl;
		s = (uchar *)sl;
	}
	while (n-- > 0)
		*d++ = *s++;
}

static void
bwd(d, s, n)
	register uchar *d, *s;
	register size_t n;
{
	register ulong *dl, *sl;

	d += n;
	s += n;
	if (n >= 8) {
		while (ALIGN(d))
			*--d = *--s, n--;
		dl = (ulong *)d;
		sl = (ulong *)s;
		for (; n >= 16; n -= 16) {
			*--dl = *--sl;
			*--dl = *--sl;
			*--dl = *--sl;
			*--dl = *--sl;
		}
		for (; n >= 4; n -= 4)
			*--dl = *--sl;
		d = (uchar *)dl;
		s = (uchar *)sl;
	}
	while (n-- > 0)
		*--d = *--s;
}

void *
memcpy(d, s, n)
	void *d;
	const void *s;
	size_t n;
{
	fwd((uchar *)d, (uchar *)s, n);
	return d;
}

void *
memmove(d, s, n)
	void *d;
	const void *s;
	size_t n;
{
	if ((uchar *)d <= (uchar *)s || (uchar *)d >= (uchar *)s + n)
		fwd((uchar *)d, (uchar *)s, n);
	else
		bwd((uchar *)d, (uchar *)s, n);
	return d;
}

void
bcopy(s, d, n)
	const void *s;
	void *d;
	size_t n;
{
	memmove(d, s, n);
}

void *
memset(d, c, n)
	void *d;
	int c;
	size_t n;
{
	register uchar *p = d;
	register ulong v, *pl;

	if (n >= 8) {
		v = (uchar)c;
		v |= v << 8;
		v |= v << 16;
		while (ALIGN(p))
			*p++ = c, n--;
		pl = (ulong *)p;
		for (; n >= 16; n -= 16) {
			*pl++ = v;
			*pl++ = v;
			*pl++ = v;
			*pl++ = v;
		}
		for (; n >= 4; n -= 4)
			*pl++ = v;
		p = (uchar *)pl;
	}
	while (n-- > 0)
		*p++ = c;
	return d;
}

void
bzero(d, n)
	void *d;
	size_t n;
{
	memset(d, 0, n);
}

double
sqrt(x)
	double x;
{
	double r;

	__asm__("fsqrt%.x %1,%0" : "=f" (r) : "f" (x));
	return r;
}
