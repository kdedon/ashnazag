/*
 * tosdisp.c -- the container's display session: the TOS screen
 * (shifter/TT video state from /dev/tos, pixels from the shared ST-RAM)
 * converted to an 8-bit frame buffer, and /dev/kbd and /dev/mouse
 * turned into IKBD packets.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#include "dsio.h"
#include "tosio.h"

#define	REFRESH	40		/* ms between screen updates */

/* ADB key code -> IKBD scan code (US layout), 0 none */
static unsigned char adb2ikbd[128] = {
	0x1e, 0x1f, 0x20, 0x21, 0x23, 0x22, 0x2c, 0x2d,	/* a s d f h g z x */
	0x2e, 0x2f, 0x60, 0x30, 0x10, 0x11, 0x12, 0x13,	/* c v ISO b q w e r */
	0x15, 0x14, 0x02, 0x03, 0x04, 0x05, 0x07, 0x06,	/* y t 1 2 3 4 6 5 */
	0x0d, 0x0a, 0x08, 0x0c, 0x09, 0x0b, 0x1b, 0x18,	/* = 9 7 - 8 0 ] o */
	0x16, 0x1a, 0x17, 0x19, 0x1c, 0x26, 0x24, 0x28,	/* u [ i p ret l j ' */
	0x25, 0x27, 0x2b, 0x33, 0x35, 0x31, 0x32, 0x34,	/* k ; \ , / n m . */
	0x0f, 0x39, 0x29, 0x0e, 0x72, 0x01, 0x1d, 0x38,	/* tab sp ` bs enter esc ctrl cmd */
	0x2a, 0x3a, 0x38, 0x4b, 0x4d, 0x50, 0x48, 0x00,	/* shift caps opt left right down up */
	0x00, 0x71, 0x00, 0x66, 0x00, 0x4e, 0x00, 0x47,	/* . * + clear(home) */
	0x00, 0x00, 0x00, 0x65, 0x72, 0x00, 0x4a, 0x00,	/* / enter - */
	0x00, 0x00, 0x70, 0x6d, 0x6e, 0x6f, 0x6a, 0x6b,	/* = 0 1 2 3 4 5 */
	0x6c, 0x67, 0x00, 0x68, 0x69, 0x00, 0x00, 0x00,	/* 6 7 8 9 */
	0x3f, 0x40, 0x41, 0x3d, 0x42, 0x43, 0x00, 0x61,	/* F5 F6 F7 F3 F8 F9 F11=undo */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x00, 0x52,	/* F10 F12=insert */
	0x00, 0x00, 0x62, 0x47, 0x00, 0x53, 0x3e, 0x00,	/* help home pgup del F4 end */
	0x3c, 0x00, 0x3b, 0x36, 0x38, 0x1d, 0x00, 0x00	/* F2 pgdn F1 rshift ropt rctrl */
};

static int tfd, fbfd;
static unsigned long ramsize;
static struct fbinfo fi;
static unsigned char *fb;
static unsigned char *shadow;
static unsigned long exl[256][2];	/* a byte of one plane as 8 pixels of 0/1 */
static unsigned char line[1280];

struct geom {
	int	w, h, planes, rowb, sx, sy, step;
};

static struct tosinput in;
static int mbtn, mdx, mdy;

/* keys, motion and buttons to the kernel, which makes the mouse packets */
static void
flush()
{
	if (in.ti_n == 0 && mdx == 0 && mdy == 0 && in.ti_btn == mbtn)
		return;
	in.ti_dx = mdx > 32767 ? 32767 : mdx < -32767 ? -32767 : mdx;
	in.ti_dy = mdy > 32767 ? 32767 : mdy < -32767 ? -32767 : mdy;
	in.ti_btn = mbtn;
	ioctl(tfd, TOSIOC_INPUT, &in);
	in.ti_n = 0;
	mdx -= in.ti_dx;
	mdy -= in.ti_dy;
}

static void
put(b)
	int b;
{
	if (in.ti_n >= sizeof in.ti_b)
		flush();
	in.ti_b[in.ti_n++] = b;
}

static void
events(fd)
	int fd;
{
	struct inev ev[32];
	int n, i, k, press = 0;

	if ((n = read(fd, (char *)ev, sizeof ev)) <= 0)
		return;
	for (i = 0; i < n / (int)sizeof ev[0]; i++)
		switch (ev[i].ie_type) {
		case IE_KEY:
			if (ev[i].ie_code >= 128 || (k = adb2ikbd[ev[i].ie_code]) == 0)
				break;
			if (ev[i].ie_code == 0x39) {	/* caps lock latches: each change toggles */
				put(k);
				put(k | 0x80);
			} else
				put(ev[i].ie_value ? k : k | 0x80);
			break;
		case IE_REL:
			if (ev[i].ie_code == IE_RELX)
				mdx += ev[i].ie_value;
			else
				mdy += ev[i].ie_value;
			break;
		case IE_BTN:
			k = ev[i].ie_code == 1 ? 2 : 1;
			mbtn = ev[i].ie_value ? mbtn | k : mbtn & ~k;
			press = 1;
			break;
		case IE_SYN:		/* each button change is a packet of its own */
			if (press)
				flush();
			press = 0;
			break;
		}
	flush();
}

/* the displayed mode, 0 if not one we show */
static int
geom(tv, g)
	struct tosvideo *tv;
	struct geom *g;
{
	g->sx = g->sy = g->step = 1;
	switch ((tv->tv_ttmode >> 8) & 7) {
	case 0: g->w = 320; g->h = 200; g->planes = 4; g->sx = g->sy = 2; break;
	case 1: g->w = 640; g->h = 200; g->planes = 2; g->sy = 2; break;
	case 2: g->w = 640; g->h = 400; g->planes = 1; break;
	case 4: g->w = 640; g->h = 480; g->planes = 4; break;
	case 6: g->w = 1280; g->h = 960; g->planes = 1; g->step = 2; break;
	case 7: g->w = 320; g->h = 480; g->planes = 8; g->sx = 2; break;
	default: return 0;
	}
	g->rowb = g->w * g->planes / 8;
	return 1;
}

static int
comp(v)
	int v;
{
	return (v & 15) * 0x1111;
}

/* the guest palette into the session's CLUT */
static void
palette(tv, g)
	struct tosvideo *tv;
	struct geom *g;
{
	static unsigned short r[256], gr[256], b[256];
	struct fbcmap cm;
	int n = 1 << g->planes, i, v, mode = (tv->tv_ttmode >> 8) & 7;
	int bank = mode == 7 ? 0 : (tv->tv_ttmode & 15) * 16;

	for (i = 0; i < n; i++) {
		if (g->planes == 1)
			v = (tv->tv_stpal[0] & 1) ^ i ? 0xfff : 0;
		else if (mode >= 4)
			v = tv->tv_ttpal[(bank + i) & 255];
		else {		/* STE bits: the top one of each nibble is the lowest */
			v = tv->tv_stpal[i];
			v = ((v & 0x777) << 1) | ((v & 0x888) >> 3);
		}
		r[i] = comp(v >> 8);
		gr[i] = comp(v >> 4);
		b[i] = comp(v);
	}
	cm.cm_start = 0;
	cm.cm_count = n;
	cm.cm_red = r;
	cm.cm_green = gr;
	cm.cm_blue = b;
	ioctl(fbfd, FBIOPUTCMAP, &cm);
}

/* one row of interleaved planes into line[] as colour indices */
static void
chunky(src, g)
	unsigned char *src;
	struct geom *g;
{
	unsigned char *d = line;
	unsigned long a, b;
	int x, k, p = g->planes, byt;

	for (x = 0; x < g->w; x += 16, src += 2 * p)
		for (byt = 0; byt < 2; byt++) {
			a = b = 0;
			for (k = 0; k < p; k++) {
				a |= exl[src[2 * k + byt]][0] << k;
				b |= exl[src[2 * k + byt]][1] << k;
			}
			d[0] = a >> 24; d[1] = a >> 16; d[2] = a >> 8; d[3] = a;
			d[4] = b >> 24; d[5] = b >> 16; d[6] = b >> 8; d[7] = b;
			d += 8;
		}
}

/* 1 if n bytes (a multiple of 16) match, compared four longs at a time */
static int
same(a, b, n)
	unsigned long *a, *b;
	int n;
{
	for (n >>= 4; n > 0; n--)
		if (*a++ != *b++ || *a++ != *b++ || *a++ != *b++ || *a++ != *b++)
			return 0;
	return 1;
}

/* redraw the rows that changed (all when full) */
static void
refresh(tv, g, full)
	struct tosvideo *tv;
	struct geom *g;
	int full;
{
	int ow = g->w * g->sx / g->step, oh = g->h * g->sy / g->step;
	int ox = ((int)fi.fi_width - ow) / 2, oy = ((int)fi.fi_height - oh) / 2;
	unsigned char *src = (unsigned char *)tv->tv_base, *sh = shadow, *d;
	int y, x, k;

	if (ox < 0)
		ox = 0;
	if (oy < 0)
		oy = 0;
	if (tv->tv_base >= ramsize || g->rowb * g->h > ramsize - tv->tv_base)
		return;				/* screen outside ST-RAM */
	for (y = 0; y < g->h; y += g->step, src += g->rowb * g->step, sh += g->rowb * g->step) {
		if (!full && same((unsigned long *)src, (unsigned long *)sh, g->rowb))
			continue;
		memcpy(sh, src, g->rowb);
		chunky(src, g);
		for (k = 0; k < g->sy; k++) {
			int row = oy + y / g->step * g->sy + k;

			if (row >= (int)fi.fi_height)
				break;
			d = fb + fi.fi_offset + row * fi.fi_rowbytes + ox;
			if (g->sx == 1 && g->step == 1)
				memcpy(d, line, ow < (int)fi.fi_width ? ow : fi.fi_width);
			else
				for (x = 0; x < ow && ox + x < (int)fi.fi_width; x++)
					d[x] = line[x * g->step / g->sx];
		}
	}
}

/* ms since the first call */
static long
now()
{
	static long t0;
	struct timeval tv;

	gettimeofday(&tv, (void *)0);
	if (t0 == 0)
		t0 = tv.tv_sec;
	return (tv.tv_sec - t0) * 1000L + tv.tv_usec / 1000;
}

void
disp(fd, verbose, ram)
	int fd, verbose;
	unsigned long ram;
{
	struct fbacq acq;
	struct tosvideo tv, last;
	struct geom g;
	struct pollfd p[2];
	int i, k, kfd, mfd, have = 0;
	long t = -REFRESH;

	tfd = fd;
	ramsize = ram;
	for (i = 0; i < 256; i++)
		for (k = 0; k < 8; k++)
			if (i & (0x80 >> k))
				exl[i][k / 4] |= 1L << (8 * (3 - k % 4));
	if ((fbfd = open("/dev/fb0", O_RDWR)) < 0) {
		perror("starttos: /dev/fb0");
		return;
	}
	memset((char *)&acq, 0, sizeof acq);
	acq.fa_kind = FBK_USER;
	acq.fa_flags = FBA_FRONT;
	strcpy(acq.fa_name, "tos");
	if (ioctl(fbfd, FBIOACQUIRE, &acq) < 0 || ioctl(fbfd, FBIOGINFO, &fi) < 0) {
		perror("starttos: display session");
		return;
	}
	if (fi.fi_depth != 8) {
		fprintf(stderr, "starttos: %lu-bit screens are not supported\n", fi.fi_depth);
		return;
	}
	/* cache-inhibited: pixels reach VRAM and the kernel's alias sees them */
	ioctl(fbfd, FBIOCACHE, FBC_CI);
	fb = (unsigned char *)mmap((caddr_t)0, fi.fi_size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fb == (unsigned char *)-1) {
		perror("starttos: mmap /dev/fb0");
		return;
	}
	shadow = (unsigned char *)malloc(160L * 960);
	memset(fb + fi.fi_offset, 0, fi.fi_rowbytes * fi.fi_height);
	kfd = open("/dev/kbd", O_RDONLY);
	mfd = open("/dev/mouse", O_RDONLY);
	if (kfd >= 0)
		ioctl(kfd, EVIOCBIND, fbfd);
	if (mfd >= 0)
		ioctl(mfd, EVIOCBIND, fbfd);
	p[0].fd = kfd;
	p[1].fd = mfd;
	p[0].events = p[1].events = POLLIN;
	for (;;) {
		if (poll(p, 2L, REFRESH / 2) > 0)
			for (i = 0; i < 2; i++)
				if (p[i].revents & POLLIN)
					events(p[i].fd);
		if (now() - t < REFRESH)
			continue;
		t = now();
		if (getppid() == 1 || ioctl(tfd, TOSIOC_VIDEO, &tv) < 0)
			return;
		if (!geom(&tv, &g))
			continue;
		k = !have || tv.tv_base != last.tv_base || tv.tv_ttmode != last.tv_ttmode;
		if (k && verbose)
			fprintf(stderr, "starttos: screen mode %x at %lx\n", tv.tv_ttmode, tv.tv_base);
		if (k)
			memset(fb + fi.fi_offset, 0, fi.fi_rowbytes * fi.fi_height);
		if (k || memcmp((char *)tv.tv_stpal, (char *)last.tv_stpal, sizeof tv.tv_stpal) ||
		    memcmp((char *)tv.tv_ttpal, (char *)last.tv_ttpal, sizeof tv.tv_ttpal))
			palette(&tv, &g);
		refresh(&tv, &g, k);
		last = tv;
		have = 1;
	}
}
