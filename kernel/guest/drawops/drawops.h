/*
 * drawops.h -- drawing into packed 1- and 8-bit bitmaps (drawops.s).
 */
#ifndef _DRAWOPS_H
#define _DRAWOPS_H

enum { DO_FILL = 1, DO_EXPAND, DO_BLIT, DO_LINE };
#define	DOF_INVERT	1	/* expand: the source inverted */
#define	DOF_TEXT	2	/* expand: a text run, offered under do_limit[0] */

/*
 * Pixel values, not colours.  Modes 1 replace, 2 transparent, 3 xor,
 * 4 reverse transparent; blits take logic operations 0-15.  A line
 * runs from x,y to w,h.  Pattern rows are picked by y & 15, their bits
 * by x & 15; pat 0 is solid.
 */
struct do_op {
	long kind, depth;
	void *dst;
	long dstride, x, y, w, h;
	long mode, fg, bg, pmask;
	void const *src;
	long sstride, sx, sy;
	unsigned short const *pat;
	long style, flags;
};

void do_init(long cpu);			/* 30, 40, 60: MOVE16 from 40 */
long do_draw(struct do_op *op);
extern long do_m16;
/* an executor for large operations: nonzero when it drew */
extern long (*do_hook)(struct do_op *op);
extern long do_limit[5];		/* by kind; [0] text */

#endif
