/*
 * fbflick.c -- the pointer on the display service's frame buffer, over
 * a frame buffer in memory: no row written over the pointer leaves it
 * out, a move writes only the old and new places, and interleaved
 * planes end up showing the shadow with the pointer on it.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "win.h"
#include "dsio.h"

struct x86 *cpu;
u32 pmvec[256];

static struct fbinfo tfi;
static u8 tfb[640 * 480];
static long written;
static int fails;

static int
topen(char *path, int flags, ...)
{
	return strcmp(path, "/dev/fb0") == 0 ? 9 : -1;
}

static int
tioctl(int fd, unsigned long req, ...)
{
	va_list ap;

	va_start(ap, req);
	if (req == FBIOGINFO)
		*va_arg(ap, struct fbinfo *) = tfi;
	va_end(ap);
	return 0;
}

static void *
tmmap(void *a, size_t n, int prot, int fl, int fd, off_t off)
{
	return tfb;
}

static void *tmemcpy();

#define open topen
#define ioctl tioctl
#define mmap tmmap
#define memcpy tmemcpy
#include "scr_fb.c"
#undef memcpy

/* the pointer's pixel at (x, y), -1 where it shows the screen */
static int
curpix(x, y)
	int x, y;
{
	struct rect pr;
	int k;

	cursrect(&pr);
	if (x < pr.l || x >= pr.r || y < pr.t || y >= pr.b)
		return -1;
	k = (y - pr.t) * cur->w + x - pr.l;
	return cur->and[k] ? -1 : cur->xor[k] ? 255 : 0;
}

/* a row of the 8-bit device: where it crosses the pointer, the pointer */
static void *
tmemcpy(d, s, n)
	void *d, *s;
	size_t n;
{
	u8 *dp = d, *sp = s;
	int y, x, i, v;

	if (dp >= tfb && dp < tfb + sizeof tfb) {
		y = (dp - tfb) / tfi.fi_rowbytes;
		x = (dp - tfb) % tfi.fi_rowbytes;
		for (i = 0; i < (int)n; i++)
			if ((v = curpix(x + i, y)) >= 0 && sp[i] != v) {
				printf("[FAIL] fbflick: row %d at %d written without the pointer\n", y, x + i);
				fails++;
				break;
			}
		written += n;
	}
	return memmove(d, s, n);
}

static void
fill(v)
	int v;
{
	memset(screen.pix, v, screen.w * screen.h);
}

int
main()
{
	static u8 and[16 * 16], xor[16 * 16];
	static struct cursor c = { 16, 16, 0, 0, and, xor };
	struct rect r;
	int x, y, p, v, want, bad;

	/* a 10x12 white block, a 6-pixel black edge, the rest transparent */
	memset(and, 1, sizeof and);
	for (y = 0; y < 12; y++)
		for (x = 0; x < 10; x++) {
			and[y * 16 + x] = 0;
			xor[y * 16 + x] = x >= 6 ? 0 : 1;
		}

	tfi.fi_layout = FBL_PACKED;
	tfi.fi_depth = 8;
	tfi.fi_width = 640;
	tfi.fi_height = 480;
	tfi.fi_rowbytes = 640;
	tfi.fi_visual = FBV_PSEUDO;
	tfi.fi_cmapsize = 256;
	tfi.fi_size = sizeof tfb;
	if (scr_open(640, 480, 0) < 0)
		return 1;
	fill(7);
	scr_setcursor(&c);
	scr_warp(100, 100);
	scr_flush();
	/* redrawn under the pointer */
	r_set(&r, 50, 90, 300, 130);
	scr_dirty(&r);
	scr_flush();
	/* a move by (3, 2): the 19x18 union and nothing else */
	written = 0;
	scr_warp(103, 102);
	scr_flush();
	if (written > 19 * 18) {
		printf("[FAIL] fbflick: a move wrote %ld bytes\n", written);
		fails++;
	}
	bad = 0;
	for (y = 90; y < 130; y++)
		for (x = 90; x < 130; x++)
			if (tfb[y * 640 + x] != ((v = curpix(x, y)) >= 0 ? v : 7))
				bad++;
	if (bad) {
		printf("[FAIL] fbflick: %d pixels wrong after a move\n", bad);
		fails++;
	}
	/* no change: nothing written */
	written = 0;
	scr_flush();
	if (written) {
		printf("[FAIL] fbflick: an idle flush wrote %ld bytes\n", written);
		fails++;
	}

	/* 4 interleaved planes */
	tfi.fi_layout = FBL_IPLAN2;
	tfi.fi_depth = 4;
	tfi.fi_rowbytes = 320;
	tfi.fi_cmapsize = 16;
	memset(tfb, 0, sizeof tfb);
	if (scr_open(640, 480, 0) < 0)
		return 1;
	fill(5);
	scr_warp(37, 41);
	scr_flush();
	scr_warp(45, 44);
	scr_flush();
	bad = 0;
	for (y = 30; y < 70; y++)
		for (x = 20; x < 80; x++) {
			for (v = 0, p = 0; p < 4; p++)
				v |= (tfb[y * 320 + (x >> 4) * 8 + 2 * p + (x >> 3 & 1)] >> (7 - (x & 7)) & 1) << p;
			want = curpix(x, y);
			want = map16[want >= 0 ? want : 5];
			if (v != want)
				bad++;
		}
	if (bad) {
		printf("[FAIL] fbflick: %d pixels wrong on planes\n", bad);
		fails++;
	}
	if (!fails)
		printf("[ok] fbflick\n");
	return fails != 0;
}
