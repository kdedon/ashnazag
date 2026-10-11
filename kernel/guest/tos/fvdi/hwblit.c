/*
 * hwblit.c -- large 1-bit fills and copies on the machine's blitter,
 * one host call each, where the host lends it (-P on a Falcon).
 */
#include "../../drawops/drawops.h"

#define	TOSIOC_BLIT	(('T' << 8) | 15)
#define	P_TFD		0xfa0078	/* /dev/tos */
#define	P_HOSTCALL	0xfa007c	/* trap #0; rts */
#define	FXSR		0x80
#define	NFSR		0x40

/* $FF8A00-$FF8A3D */
struct bregs {
	unsigned short	ht[16];
	short		sxi, syi;
	unsigned long	sa;
	unsigned short	em[3];
	short		dxi, dyi;
	unsigned long	da;
	unsigned short	xn, yn;
	unsigned char	hop, op, ctl, skew;
};

#ifdef HOSTTEST
long bcall(struct bregs *r);
#else
/* ioctl(p_tfd, TOSIOC_BLIT, r), again after EINTR: 0, or -1 */
static long bcall(struct bregs *r)
{
	long e;

	__asm__ volatile("move.l %1,-(%%sp)\n\tmove.l %2,-(%%sp)\n\tmove.l %3,-(%%sp)\n"
	    "1:\tmoveq #54,%%d0\n\tmove.l %4,%%a0\n\tjsr (%%a0)\n\tbcc.s 2f\n"
	    "\tcmp.l #4,%%d0\n\tbeq.s 1b\n\tmoveq #-1,%%d0\n2:\tlea 12(%%sp),%%sp\n\tmove.l %%d0,%0"
	    : "=r"(e) : "r"(r), "r"((long)TOSIOC_BLIT), "r"(*(long *)P_TFD), "a"(*(long *)P_HOSTCALL)
	    : "d0", "d1", "a0", "a1", "memory", "cc");
	return e;
}
#endif

/* fill modes by mode, fg, bg: blitter logic operations on halftone S */
static unsigned char const fillop[4][4] = {
	{ 0, 12, 3, 15 }, { 4, 4, 7, 7 }, { 6, 6, 6, 6 }, { 1, 1, 13, 13 }
};

long hw_hook(struct do_op *o)
{
	struct bregs r;
	long lx, rx, sl, sr, dsp, ssp, sk, i, n, y0;
	unsigned long d, s = 0;
	int up = 1;

	if (o->kind == DO_EXPAND || o->depth != 1 || !(o->pmask & 1) ||
	    (o->dstride & 1) || ((long)o->dst & 1))
		return 0;
	lx = o->x;
	rx = o->x + o->w - 1;
	d = (unsigned long)o->dst + o->y * o->dstride + (lx >> 4) * 2;
	r.em[1] = 0xffff;
	r.yn = o->h;
	r.ctl = 0;
	if (o->kind == DO_FILL) {
		if (o->mode < 1 || o->mode > 4)
			return 0;
		r.op = fillop[o->mode - 1][(o->fg & 1) << 1 | (o->bg & 1)];
		r.hop = o->pat ? 1 : 0;
		for (i = 0; o->pat && i < 16; i++)
			r.ht[i] = o->pat[i];
		r.ctl = o->y & 15;
		r.xn = (rx >> 4) - (lx >> 4) + 1;
		r.da = d;
		r.dxi = 2;
		r.dyi = o->dstride - (r.xn - 1) * 2;
		r.em[0] = 0xffff >> (lx & 15);
		r.em[2] = 0xffff << (15 - (rx & 15));
		r.skew = 0;
		r.sxi = r.syi = 0;
		r.sa = 0;
	} else {
		if ((o->sstride & 1) || ((long)o->src & 1))
			return 0;
		r.op = o->mode & 15;
		r.hop = 2;
		sl = o->sx;
		sr = o->sx + o->w - 1;
		s = (unsigned long)o->src + o->sy * o->sstride + (sl >> 4) * 2;
		ssp = (sr >> 4) - (sl >> 4);
		dsp = (rx >> 4) - (lx >> 4);
		r.xn = dsp + 1;
		sk = (lx & 15) - (sl & 15);
		n = (ssp == dsp ? 2 : 0) | (sk < 0);
		/* the source first in memory: right to left, bottom up */
		if (s < d || (s == d && sk >= 0)) {
			up = 0;
			r.sa = s + (o->h - 1) * o->sstride + 2 * ssp;
			r.da = d + (o->h - 1) * o->dstride + 2 * dsp;
			r.sxi = r.dxi = -2;
			r.syi = -(o->sstride - 2 * ssp);
			r.dyi = -(o->dstride - 2 * dsp);
			r.em[0] = 0xffff << (15 - (rx & 15));
			r.em[2] = 0xffff >> (lx & 15);
		} else {
			n |= 4;
			r.sa = s;
			r.da = d;
			r.sxi = r.dxi = 2;
			r.syi = o->sstride - 2 * ssp;
			r.dyi = o->dstride - 2 * dsp;
			r.em[0] = 0xffff >> (lx & 15);
			r.em[2] = 0xffff << (15 - (rx & 15));
		}
		/* FXSR and NFSR by direction, equal spans, alignment */
		r.skew = (sk & 15) | "\100\200\300\000\100\200\000\300"[n];
		if (!ssp && !dsp) {
			r.sxi = sk;
			r.skew = sk ? sk & 15 : FXSR;
		}
	}
	if (r.xn == 1)
		r.em[0] &= r.em[2];
	y0 = r.yn;
	if (bcall(&r) == 0 && r.yn == 0)
		return 1;
	/* what is left (the session hidden, a signal) goes to the CPU */
	if (up) {
		o->y += y0 - r.yn;
		o->sy += y0 - r.yn;
	}
	o->h = r.yn;
	return 0;
}

/* the blitter for operations of at least lim[kind] pixels, if the host lends it */
void hw_init(long const *lim)
{
	struct bregs r = { { 0 } };
	int i;

	if (bcall(&r))
		return;
	for (i = 0; i < 5; i++)
		do_limit[i] = lim[i];
	do_hook = hw_hook;
}
