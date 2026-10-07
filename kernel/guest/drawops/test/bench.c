/*
 * bench.c -- one operation for counting instructions: run with and
 * without it (the last argument 0) under an instruction trace.
 *
 *   bench cpu case run
 */
#include "../drawops.h"

static unsigned char fb[1344 * 1000 + 64] __attribute__((aligned(16)));
static unsigned char font[320 * 16 / 8] __attribute__((aligned(16)));

static long atol_(char const *s)
{
	long v = 0;

	while (*s)
		v = v * 10 + *s++ - '0';
	return v;
}

int main(int argc, char **argv)
{
	struct do_op o;
	long c, i;

	if (argc < 4)
		return 2;
	do_init(atol_(argv[1]));
	c = atol_(argv[2]);
	o.kind = DO_FILL; o.depth = 8; o.dst = fb; o.dstride = 832;
	o.x = 0; o.y = 0; o.w = 640; o.h = 480; o.mode = 1; o.fg = 5; o.bg = 0;
	o.pmask = -1; o.src = fb; o.sstride = 1344; o.sx = 0; o.sy = 0; o.pat = 0;
	o.style = 0xffff; o.flags = 0;
	switch (c) {
	case 1: break;					/* fill 640x480 */
	case 2: o.w = o.h = 200; o.x = 13; o.y = 7; break;
	case 3: o.w = o.h = 16; o.x = 13; o.y = 7; break;
	case 4: o.w = o.h = 8; o.x = 13; o.y = 7; break;
	case 5: o.kind = DO_BLIT; o.mode = 3; o.sstride = 832; o.sy = 8; o.h = 472; break;	/* scroll */
	case 6: o.kind = DO_BLIT; o.mode = 3; o.dstride = 1344; o.w = o.h = 200; o.x = 400; o.y = 300; o.sx = 20; o.sy = 20; break;
	case 7: o.kind = DO_BLIT; o.mode = 3; o.dstride = 1344; o.src = fb + 1344 * 480; o.sstride = 0; break;	/* 640x480 copy */
	case 14: o.kind = DO_BLIT; o.mode = 3; o.sstride = 832; o.w = o.h = 200; o.x = 200; o.y = 200; o.sx = 0; o.sy = 200; o.src = fb; break;
	case 8: o.kind = DO_LINE; o.x = 0; o.y = 100; o.w = 639; o.h = 100; break;
	case 9: o.kind = DO_LINE; o.x = 100; o.y = 0; o.w = 100; o.h = 479; break;
	case 10: o.kind = DO_LINE; o.x = 0; o.y = 0; o.w = 639; o.h = 479; break;
	case 11: o.kind = DO_EXPAND; o.src = font; o.sstride = 40; o.w = 8; o.h = 16;
		o.mode = 2; o.x = 17; o.y = 30; break;	/* one character; 40 make the text case */
	case 12: o.kind = DO_EXPAND; o.src = font; o.sstride = 40; o.w = 320; o.h = 16;
		o.mode = 2; o.x = 17; o.y = 30; break;	/* the text as one template */
	case 13: o.kind = DO_EXPAND; o.src = font; o.sstride = 40; o.w = 320; o.h = 16;
		o.mode = 1; o.x = 17; o.y = 30; break;
	/* 1-bit, rows of 168 bytes */
	case 21: o.depth = 1; o.dstride = 168; break;
	case 22: o.depth = 1; o.dstride = 168; o.w = o.h = 200; o.x = 13; o.y = 7; break;
	case 23: o.depth = 1; o.dstride = 168; o.w = o.h = 16; o.x = 13; o.y = 7; break;
	case 24: o.depth = 1; o.dstride = 168; o.w = o.h = 8; o.x = 13; o.y = 7; break;
	case 25: o.depth = 1; o.kind = DO_BLIT; o.mode = 3; o.dstride = o.sstride = 168; o.sy = 8; o.h = 472; break;
	case 26: o.depth = 1; o.kind = DO_BLIT; o.mode = 3; o.dstride = o.sstride = 168; o.w = o.h = 200; o.x = 400; o.y = 300; o.sx = 20; o.sy = 20; break;
	case 27: o.depth = 1; o.kind = DO_EXPAND; o.src = font; o.sstride = 40; o.w = 8; o.h = 16;
		o.mode = 2; o.x = 17; o.y = 30; o.dstride = 168; break;
	case 29: o.depth = 1; o.kind = DO_EXPAND; o.src = font; o.sstride = 40; o.w = 320; o.h = 16;
		o.mode = 2; o.x = 17; o.y = 30; o.dstride = 168; break;	/* the text as one template */
	case 28: o.depth = 1; o.kind = DO_BLIT; o.mode = 3; o.dstride = o.sstride = 168; o.src = fb + 168 * 480; break;
	}
	if (c == 7)
		o.sstride = 1344;
	if (atol_(argv[3]))
		for (i = 0; i < (c == 11 || c == 27 ? 40 : 1); i++, o.x += 8)
			do_draw(&o);
	return 0;
}
