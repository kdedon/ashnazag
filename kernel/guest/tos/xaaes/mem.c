/*
 * mem.c -- one memory pool for the kernel and every process: blocks in
 * address order, first fit, each owned by a process (or the kernel) so
 * a process's memory goes when it exits.  The header's MEMREGION is
 * what addr2mem hands out.
 */
#include "xamint.h"

#define	MAGIC	0x584d454dL
#define	HDR	((sizeof(struct blk) + 15) & ~15L)
#define	MINSPLIT 64

struct blk {
	MEMREGION	m;
	struct blk	*next, *prev;
	struct kp	*owner;
	long		magic;
	short		used;
};

static struct blk *first;

void
mem_init(long lo, long hi)
{
	lo = (lo + 15) & ~15L;
	hi &= ~15L;
	first = (struct blk *)lo;
	bzero(first, HDR);
	first->magic = MAGIC;
	first->m.loc = lo + HDR;
	first->m.len = hi - lo - HDR;
	first->m.links = 1;
	xlog("pool %lx-%lx (%ld KB)", lo, hi, (hi - lo) >> 10);
}

static struct blk *
hdr(void *p)
{
	struct blk *b = (struct blk *)((char *)p - HDR);

	if (!p || b->magic != MAGIC || !b->used)
		return 0;
	return b;
}

/* split b to n bytes when the rest is worth a block */
static void
split(struct blk *b, unsigned long n)
{
	struct blk *r;

	if (b->m.len < n + HDR + MINSPLIT)
		return;
	r = (struct blk *)(b->m.loc + n);
	bzero(r, HDR);
	r->magic = MAGIC;
	r->m.loc = (long)r + HDR;
	r->m.len = b->m.len - n - HDR;
	r->m.links = 1;
	r->next = b->next;
	r->prev = b;
	if (b->next)
		b->next->prev = r;
	b->next = r;
	b->m.len = n;
}

void *
kmalloc_o(unsigned long n, struct kp *owner)
{
	struct blk *b;
	short s;

	n = (n + 15) & ~15L;
	if (n == 0)
		n = 16;
	s = xa_spl7();
	for (b = first; b; b = b->next)
		if (!b->used && b->m.len >= n)
			break;
	if (!b) {
		xa_splx(s);
		xlog("out of memory: %ld bytes", n);
		return 0;
	}
	split(b, n);
	b->used = 1;
	b->owner = owner;
	xa_splx(s);
	return (void *)b->m.loc;
}

static void
merge(struct blk *b)
{
	struct blk *n = b->next;

	if (n && !n->used && (long)n == b->m.loc + b->m.len) {
		b->m.len += HDR + n->m.len;
		b->next = n->next;
		if (n->next)
			n->next->prev = b;
		n->magic = 0;
	}
}

void
kfree_any(void *p)
{
	struct blk *b = hdr(p);
	short s;

	if (!b) {
		if (p)
			xlog("free of a bad block %lx", (long)p);
		return;
	}
	s = xa_spl7();
	b->used = 0;
	b->owner = 0;
	merge(b);
	if (b->prev && !b->prev->used)
		merge(b->prev);
	xa_splx(s);
}

long
mem_largest(void)
{
	struct blk *b;
	long m = 0;

	for (b = first; b; b = b->next)
		if (!b->used && (long)b->m.len > m)
			m = b->m.len;
	return m;
}

/* Mshrink: to n bytes, 0 frees */
long
mem_shrink(void *p, long n)
{
	struct blk *b = hdr(p);
	short s;

	if (!b)
		return EIMBA;
	if (n == 0) {
		kfree_any(p);
		return 0;
	}
	n = (n + 15) & ~15L;
	if ((unsigned long)n > b->m.len)
		return EGSBF;
	s = xa_spl7();
	split(b, n);
	if (b->next && !b->next->used)
		merge(b->next);
	xa_splx(s);
	return 0;
}

void
mem_freeproc(struct kp *k)
{
	struct blk *b;

again:
	for (b = first; b; b = b->next)
		if (b->used && b->owner == k) {
			kfree_any((void *)b->m.loc);
			goto again;
		}
}

/* the block holding addr */
MEMREGION *
mem_region(struct kp *k, long addr)
{
	struct blk *b;

	(void)k;
	for (b = first; b; b = b->next)
		if (b->used && addr >= (long)b->m.loc && addr < (long)(b->m.loc + b->m.len))
			return &b->m;
	return 0;
}

/* a block's owner changes, e.g. a program image given to its process */
void
mem_give(void *p, struct kp *k)
{
	struct blk *b = hdr(p);

	if (b)
		b->owner = k;
}

void *_cdecl
_kmalloc(unsigned long size, const char *func)
{
	(void)func;
	return kmalloc_o(size, 0);
}

void _cdecl
_kfree(void *place, const char *func)
{
	(void)func;
	kfree_any(place);
}

void *_cdecl
_umalloc(unsigned long size, const char *func)
{
	(void)func;
	return kmalloc_o(size, KP(curproc));
}

void _cdecl
_ufree(void *place, const char *func)
{
	(void)func;
	kfree_any(place);
}

MEMREGION *_cdecl
addr2mem(struct proc *p, long addr)
{
	return mem_region(KP(p), addr);
}

long _cdecl
attach_region(struct proc *p, MEMREGION *m)
{
	(void)p;
	return m ? (long)m->loc : 0;
}

void _cdecl
detach_region(struct proc *p, MEMREGION *m)
{
	(void)p;
	(void)m;
}

/* the first block of k's above after, 0 if none; its length in *len */
long
mem_next(struct kp *k, long after, long *len)
{
	struct blk *b;

	for (b = first; b; b = b->next)
		if (b->used && b->owner == k && (long)b->m.loc > after) {
			*len = b->m.len;
			return b->m.loc;
		}
	return 0;
}
