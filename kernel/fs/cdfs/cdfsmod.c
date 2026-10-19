/*
 * cdfsmod.c -- the cdfs filesystem as a loadable module, plus the
 * 64-bit divide and shift helpers it calls, which the kernel does not
 * export.  The helpers use 32-bit operations only (no 68060 traps).
 *
 * K&R C.
 */

#include "sys/types.h"
#include "sys/moddefs.h"

extern int cdfsinit();

struct mod_fs_data cdfs_fsdata = { "cdfs", cdfsinit, 0 };

static int
cdfs_load()
{
	return 0;
}

static int
cdfs_unload()
{
	return 0;
}

MOD_FS_WRAPPER(cdfs, cdfs_load, cdfs_unload, "cdfs read-only CD filesystem");

typedef union {
	unsigned long long ll;
	struct { unsigned long hi, lo; } w;
} dw;

/* q = a / b, r = a % b: one quotient bit per step */
static void
udivmod(a, b, q, r)
	dw *a, *b, *q, *r;
{
	int i;

	q->w.hi = q->w.lo = r->w.hi = r->w.lo = 0;
	for (i = 63; i >= 0; i--) {
		r->w.hi = r->w.hi << 1 | r->w.lo >> 31;
		r->w.lo = r->w.lo << 1 |
		    ((i >= 32 ? a->w.hi >> (i - 32) : a->w.lo >> i) & 1);
		if (r->w.hi > b->w.hi || (r->w.hi == b->w.hi && r->w.lo >= b->w.lo)) {
			r->w.hi -= b->w.hi + (r->w.lo < b->w.lo);
			r->w.lo -= b->w.lo;
			if (i >= 32)
				q->w.hi |= 1L << (i - 32);
			else
				q->w.lo |= 1L << i;
		}
	}
}

unsigned long long
__udivdi3(a, b)
	unsigned long long a, b;
{
	dw x, y, q, r;

	x.ll = a;
	y.ll = b;
	udivmod(&x, &y, &q, &r);
	return q.ll;
}

unsigned long long
__umoddi3(a, b)
	unsigned long long a, b;
{
	dw x, y, q, r;

	x.ll = a;
	y.ll = b;
	udivmod(&x, &y, &q, &r);
	return r.ll;
}

unsigned long long
__lshrdi3(a, n)
	unsigned long long a;
	int n;
{
	dw x;

	x.ll = a;
	if (n >= 32) {
		x.w.lo = n >= 64 ? 0 : x.w.hi >> (n - 32);
		x.w.hi = 0;
	} else if (n > 0) {
		x.w.lo = x.w.lo >> n | x.w.hi << (32 - n);
		x.w.hi >>= n;
	}
	return x.ll;
}
