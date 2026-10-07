/*
 * hplat.c -- amilib's platform for the emulator harness (run.py): a
 * first-fit heap, a console port, Zorro boards from a table run.py
 * fills in, identity mappings.
 */

#include "amilib.h"

#define	CONS	(*(volatile unsigned char *)0x01000000)

/* filled in by run.py before the run */
struct amx_zboard h_boards[8];
int h_nboards;
int h_zbsize = sizeof (struct amx_zboard);
int h_attn = AFF_68010 | AFF_68020 | AFF_68030 | AFF_68040 | AFF_FPU40;

/* counters run.py reads afterwards */
long h_delay_us;
int h_attached;			/* interrupt chains routed, as a mask */
int h_lockdepth;

extern char _end[];

/* ------------------------------------------------------------ heap */

struct blk {
	unsigned long	size;
	struct blk	*next;
};

static struct blk *h_free;
static int h_heapup;

#define	HEAP_LO	0x00200000
#define	HEAP_HI	0x00c00000

void *
memset(d, c, n)
	void *d;
	int c;
	unsigned long n;
{
	char *p = d;

	while (n--)
		*p++ = c;
	return d;
}

void *
memcpy(d, s, n)
	void *d;
	const void *s;
	unsigned long n;
{
	char *p = d;
	const char *q = s;

	while (n--)
		*p++ = *q++;
	return d;
}

char *
amx_alloc(size, cansleep)
	unsigned long size;
	int cansleep;
{
	struct blk **pp, *b, *r;

	if (!h_heapup) {
		h_free = (struct blk *)HEAP_LO;
		h_free->size = HEAP_HI - HEAP_LO;
		h_free->next = 0;
		h_heapup = 1;
	}
	size = (size + 15) & ~15;
	for (pp = &h_free; (b = *pp) != 0; pp = &b->next) {
		if (b->size < size)
			continue;
		if (b->size - size >= 32) {
			r = (struct blk *)((char *)b + b->size - size);
			b->size -= size;
		} else {
			r = b;
			*pp = b->next;
		}
		/* fill with garbage: callers that rely on zeroed memory show */
		memset(r, 0xa5, size);
		return (char *)r;
	}
	return 0;
}

void
amx_free(p, size)
	char *p;
	unsigned long size;
{
	struct blk *b = (struct blk *)p, **pp;

	size = (size + 15) & ~15;
	memset(p, 0x5a, size);
	b->size = size;
	for (pp = &h_free; *pp && *pp < b; pp = &(*pp)->next)
		;
	b->next = *pp;
	*pp = b;
	/* merge with the next, then the previous */
	if (b->next && (char *)b + b->size == (char *)b->next) {
		b->size += b->next->size;
		b->next = b->next->next;
	}
	if (pp != &h_free) {
		struct blk *q = (struct blk *)((char *)pp -
		    (unsigned long)&((struct blk *)0)->next);

		if ((char *)q + q->size == (char *)b) {
			q->size += b->size;
			q->next = b->next;
		}
	}
}

/* ------------------------------------------------------- the CPU */

int
amx_spl7()
{
	int s;

	__asm__ __volatile__("movew %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("movew %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s & 0xffff;
}

void
amx_splx(s)
	int s;
{
	__asm__ __volatile__("movew %0,%%sr" : : "d" (s) : "memory");
}

int
amx_ipl()
{
	int s;

	__asm__ __volatile__("movew %%sr,%0" : "=d" (s));
	return (s >> 8) & 7;
}

void
amx_delayus(n)
	long n;
{
	h_delay_us += n;
}

void
amx_time(tv)
	unsigned long *tv;
{
	tv[0] = 1000 + h_delay_us / 1000000;
	tv[1] = h_delay_us % 1000000;
}

void
amx_cacheflush()
{
}

int
amx_attnflags()
{
	return h_attn;
}

/* ------------------------------------------------------- the board */

int
amx_zorro(i, zb)
	int i;
	struct amx_zboard *zb;
{
	if (i >= h_nboards)
		return -1;
	*zb = h_boards[i];
	return 0;
}

char *
amx_iomap(pa, size)
	unsigned long pa, size;
{
	return (char *)pa;
}

void
amx_iounmap(va, size)
	char *va;
	unsigned long size;
{
}

unsigned long
amx_vtop(va)
	char *va;
{
	return (unsigned long)va;
}

int
amx_intattach(n)
	int n;
{
	h_attached |= 1 << n;
	return 0;
}

void
amx_intdetach(n)
	int n;
{
	h_attached &= ~(1 << n);
}

/* ------------------------------------------------------------ lock */

int
amx_lock()
{
	return h_lockdepth++ ? -1 : 0;
}

int
amx_trylock()
{
	return amx_lock();
}

void
amx_unlock()
{
	h_lockdepth--;
}

/* ---------------------------------------------------------- console */

static void
putn(v, base)
	unsigned long v;
	int base;
{
	char b[12];
	int i = 0;

	do {
		b[i++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	while (i)
		CONS = b[--i];
}

void
amx_log(f, a, b, c, d)
	char *f;
	long a, b, c, d;
{
	long arg[4];
	int n = 0;
	char *s;

	arg[0] = a, arg[1] = b, arg[2] = c, arg[3] = d;
	for (; *f; f++) {
		if (*f != '%' || !f[1]) {
			CONS = *f;
			continue;
		}
		switch (*++f) {
		case 's':
			for (s = (char *)arg[n++ & 3]; s && *s; s++)
				CONS = *s;
			break;
		case 'd':
			if (arg[n & 3] < 0) {
				CONS = '-';
				arg[n & 3] = -arg[n & 3];
			}
			putn((unsigned long)arg[n++ & 3], 10);
			break;
		case 'x':
			putn((unsigned long)arg[n++ & 3], 16);
			break;
		default:
			CONS = *f;
		}
	}
}
