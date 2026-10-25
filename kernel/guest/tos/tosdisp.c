/*
 * tosdisp.c -- the container's display session: the TOS screen
 * (the screen TOS set through its XBIOS, pixels from the shared
 * ST-RAM) converted to an 8-bit frame buffer, and /dev/kbd and
 * /dev/mouse posted to the cartridge.
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
#include "tosfb.h"
#include <stropts.h>

#define	REFRESH	40		/* ms between screen updates */
#define	CALM	8		/* unchanged updates before each doubling, up to 8 * REFRESH */

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
static unsigned char line[1280], line2[1280];
static unsigned char even[256];		/* bits 6, 4, 2, 0 of a byte as 3-0 */

struct geom {
	int	w, h, planes, rowb, sx, sy, step;
};

int fbpipe = -1;		/* the launcher gets the session through it */
static int native;		/* key codes are IKBD scancodes */
static int mbtn;
static volatile struct tospv *pv = (struct tospv *)TOSPV;
static unsigned short px, py;		/* motion posted to the cartridge */
static unsigned char kdown[128];	/* scan codes held */
static int hidden;			/* in the background: input is dropped */
static int planar;			/* bitplanes: only fVDI draws */
static unsigned char dark[256];		/* 1-bit host: colour index shown black */
static struct {
	unsigned short	ev;
	unsigned long	xy;
} bl[PV_NEV];				/* events waiting for room in the cartridge's queue */
static int nbl;
static long now();

/* the shifter and TT video state */
struct shifter {
	unsigned long	tv_base;
	unsigned short	tv_stmode;	/* $FF8260 */
	unsigned short	tv_ttmode;	/* $FF8262 */
	unsigned short	tv_stpal[16];	/* $FF8240 */
	unsigned short	tv_ttpal[256];	/* $FF8400 */
};

/* waiting events into the cartridge's queue while it has room, then the motion after them */
static void
drain()
{
	unsigned long h = pv->pv_head;
	int i = 0;

	for (; i < nbl && h - pv->pv_tail < PV_NEV; i++, h++) {
		pv->pv_xy = bl[i].xy;
		pv->pv_ev[h % PV_NEV].e_ev = bl[i].ev;
		pv->pv_ev[h % PV_NEV].e_xy = bl[i].xy;
		pv->pv_head = h + 1;
	}
	nbl -= i;
	memmove((char *)bl, (char *)(bl + i), nbl * sizeof bl[0]);
	if (nbl == 0)
		pv->pv_xy = (unsigned long)px << 16 | py;
}

/* an event for the cartridge, after the motion before it */
static void
post(ev)
	int ev;
{
	if (nbl < PV_NEV) {
		bl[nbl].ev = ev;
		bl[nbl].xy = (unsigned long)px << 16 | py;
		nbl++;
	}
	drain();
}

static void
key(k)
	int k;
{
	kdown[k & 0x7f] = !(k & 0x80);
	post(PE_KEY << 8 | k);
}

/* each button change is a packet of its own */
static void
button()
{
	post(PE_BTN << 8 | mbtn);
}

static void
motion(axis, v)
	int axis;
	long v;
{
	if (axis == IE_RELX)
		px += v;
	else
		py += v;
	if (nbl == 0)
		pv->pv_xy = (unsigned long)px << 16 | py;
}

/* to the background: TOS sees every key and button released, then sleeps */
static void
hide()
{
	int k;

	for (k = 1; k < 128; k++)
		if (kdown[k])
			key(k | 0x80);
	if (mbtn) {
		mbtn = 0;
		button();
	}
	ioctl(tfd, TOSIOC_PAUSE, 1);
}

static void
events(fd)
	int fd;
{
	struct inev ev[32];
	int n, i, k;

	if ((n = read(fd, (char *)ev, sizeof ev)) <= 0 || hidden)
		return;
	for (i = 0; i < n / (int)sizeof ev[0]; i++)
		switch (ev[i].ie_type) {
		case IE_KEY:
			if (ev[i].ie_code >= 128 ||
			    (k = native ? ev[i].ie_code : adb2ikbd[ev[i].ie_code]) == 0)
				break;
			if (k == 0x3a) {	/* caps lock latches: each change toggles */
				key(k);
				key(k | 0x80);
			} else
				key(ev[i].ie_value ? k : k | 0x80);
			break;
		case IE_REL:
			motion(ev[i].ie_code, ev[i].ie_value);
			break;
		case IE_BTN:
			k = ev[i].ie_code == 1 ? 2 : 1;
			mbtn = ev[i].ie_value ? mbtn | k : mbtn & ~k;
			button();
			break;
		}
}

/* the displayed mode, 0 if not one we show */
static int
geom(tv, g)
	struct shifter *tv;
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
	struct shifter *tv;
	struct geom *g;
{
	static unsigned short r[256], gr[256], b[256];
	struct fbcmap cm;
	int n = 1 << g->planes, i, v, mode = (tv->tv_ttmode >> 8) & 7;
	int bank = mode == 7 ? 0 : (tv->tv_ttmode & 15) * 16;

	if (g->step == 2) {	/* TT high, halved: 0-4 black of each 2x2 as greys on white */
		n = 5;
		for (i = 0; i < n; i++)
			r[i] = gr[i] = b[i] = (4 - i) * 0x3fff;
	} else
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
	if (fi.fi_depth == 1) {
		for (i = 0; i < n; i++)
			dark[i] = 2L * r[i] + 5L * gr[i] + b[i] < 4L * 0x10000;
		return;
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

/*
 * Two TT high rows halved onto n bytes of a 1-bit screen: a pixel is
 * dark when 2 of its 4 are set, as the grey palette makes it.
 */
static void
halve(d, a, b, n)
	unsigned char *d, *a, *b;
	int n;
{
	unsigned int x, y, m;

	for (; n > 0; n--, a += 2, b += 2) {
		x = a[0] << 8 | a[1];
		y = b[0] << 8 | b[1];
		m = (x & x >> 1) | (y & y >> 1) | ((x | x >> 1) & (y | y >> 1));
		*d++ = even[m >> 8 & 0xff] << 4 | even[m & 0xff];
	}
}

/* redraw the rows that changed (all when full); 1 if any did */
static int
refresh(tv, g, full)
	struct shifter *tv;
	struct geom *g;
	int full;
{
	int ow = g->w * g->sx / g->step, oh = g->h * g->sy / g->step;
	int ox = ((int)fi.fi_width - ow) / 2, oy = ((int)fi.fi_height - oh) / 2;
	unsigned char *src = (unsigned char *)tv->tv_base, *sh = shadow, *d;
	int y, x, k, v, bits, drawn = 0;
	/* a 1-bit ST high row is copied as it is */
	int direct = fi.fi_depth == 1 && g->planes == 1 && g->sx == 1 && g->step == 1 && dark[1] != dark[0];

	if (ox < 0)
		ox = 0;
	if (fi.fi_depth == 1)
		ox &= ~7;
	if (oy < 0)
		oy = 0;
	if (tv->tv_base >= ramsize || g->rowb * g->h > ramsize - tv->tv_base)
		return 0;			/* screen outside ST-RAM */
	for (y = 0; y < g->h; y += g->step, src += g->rowb * g->step, sh += g->rowb * g->step) {
		if (!full && same((unsigned long *)src, (unsigned long *)sh, g->rowb * g->step))
			continue;
		memcpy(sh, src, g->rowb * g->step);
		drawn = 1;
		if (g->step == 2 && fi.fi_depth == 1) {
			k = oy + y / 2;
			if (k < (int)fi.fi_height)
				halve(fb + fi.fi_offset + k * fi.fi_rowbytes + ox / 8, src,
				    src + g->rowb, ((ow < (int)fi.fi_width - ox ? ow : (int)fi.fi_width - ox)) / 8);
			continue;
		}
		if (g->step == 2) {
			chunky(src + g->rowb, g);
			memcpy(line2, line, g->w);
		}
		if (!direct)
			chunky(src, g);
		for (k = 0; k < g->sy; k++) {
			int row = oy + y / g->step * g->sy + k;

			if (row >= (int)fi.fi_height)
				break;
			d = fb + fi.fi_offset + row * fi.fi_rowbytes + ox;
			if (fi.fi_depth == 1) {
				d = fb + fi.fi_offset + row * fi.fi_rowbytes + ox / 8;
				if (direct) {
					for (x = 0; x < ow / 8 && ox + 8 * x < (int)fi.fi_width; x++)
						d[x] = dark[1] ? src[x] : ~src[x];
					continue;
				}
				for (x = 0, bits = 0; x < ow && ox + x < (int)fi.fi_width; x++) {
					if (g->step == 2)
						v = line[2 * x] + line[2 * x + 1] + line2[2 * x] +
						    line2[2 * x + 1];
					else
						v = line[x * g->step / g->sx];
					bits = bits << 1 | dark[v];
					if ((x & 7) == 7)
						d[x / 8] = bits;
				}
				continue;
			}
			if (g->sx == 1 && g->step == 1)
				memcpy(d, line, ow < (int)fi.fi_width ? ow : fi.fi_width);
			else if (g->step == 2)
				for (x = 0; x < ow && ox + x < (int)fi.fi_width; x++)
					d[x] = line[2 * x] + line[2 * x + 1] + line2[2 * x] +
					    line2[2 * x + 1];
			else
				for (x = 0; x < ow && ox + x < (int)fi.fi_width; x++)
					d[x] = line[x * g->step / g->sx];
		}
	}
	return drawn;
}

/* 1 if the ST screen changed since the shadow copy, which takes it */
static int
stchanged(tv)
	struct shifter *tv;
{
	struct geom g;
	long n;

	if (!shadow || !geom(tv, &g))
		return 0;
	n = (long)g.rowb * g.h;
	if (tv->tv_base >= ramsize || n > ramsize - tv->tv_base ||
	    same((unsigned long *)tv->tv_base, (unsigned long *)shadow, n))
		return 0;
	memcpy(shadow, (char *)tv->tv_base, n);
	return 1;
}

/*
 * Does the fVDI driver own the screen: its cookie is set, the ST
 * screen is still the one it took over, and the ST screen has not
 * changed since the driver last drew (console text, a program's own
 * pixels).  Its palette goes to the CLUT.
 */
static int
fvdi(tv)
	struct shifter *tv;
{
	static unsigned long seq, base, draws;
	static unsigned short stm, ttm;
	static struct tfbshare *last;
	static int stale, st, own;
	static unsigned short r[256], gr[256], b[256];
	struct fbcmap cm;
	struct tfbshare *fs = 0;
	unsigned long jar = *(unsigned long *)0x5a0, *c, v;
	int i;

	if (jar & 1 || jar < 0x600 || jar >= ramsize - 8)
		return 0;
	for (c = (unsigned long *)jar, i = 0; (unsigned long)c < ramsize - 8 && c[0] && i < 256;
	    c += 2, i++)
		if (c[0] == TFB_COOKIE) {
			v = c[1];
			if (!(v & 1) && v >= 0x600 && v < ramsize - sizeof *fs)
				fs = (struct tfbshare *)v;
			break;
		}
	if (fs == 0 || fs->fs_magic != TFB_MAGIC || !fs->fs_on) {
		last = 0;
		return 0;
	}
	if (fs != last) {
		last = fs;
		base = tv->tv_base;
		stm = tv->tv_stmode;
		ttm = tv->tv_ttmode;
		stale = 1;
		draws = fs->fs_draw;
		st = 0;
		stchanged(tv);
	}
	/* a program set its own screen: show that */
	if (tv->tv_base != base || tv->tv_stmode != stm || tv->tv_ttmode != ttm) {
		stale = own = 1;
		return 0;
	}
	if (own || fs->fs_draw != draws) {
		own = 0;
		draws = fs->fs_draw;
		stale |= st;
		st = 0;
		stchanged(tv);
	} else if (!st && stchanged(tv))
		st = 1;
	if (st)
		return 0;
	if (stale || fs->fs_seq != seq) {
		stale = 0;
		seq = fs->fs_seq;
		for (i = 0; i < 256; i++) {
			r[i] = fs->fs_pal[i][0];
			gr[i] = fs->fs_pal[i][1];
			b[i] = fs->fs_pal[i][2];
		}
		cm.cm_start = 0;
		cm.cm_count = fi.fi_cmapsize < 256 ? fi.fi_cmapsize : 256;
		cm.cm_red = r;
		cm.cm_green = gr;
		cm.cm_blue = b;
		ioctl(fbfd, FBIOPUTCMAP, &cm);
	}
	return 1;
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

/* the deepest bitplane mode; 1 if set */
static int
planes()
{
	struct fbmodeinfo mi[16];
	struct fbmodes ms;
	unsigned long i, best = 0;

	if (fi.fi_layout == FBL_PLANES)
		return 1;
	ms.ms_count = 16;
	ms.ms_modes = mi;
	if (ioctl(fbfd, FBIOGMODES, &ms) < 0)
		return 0;
	for (i = 1; i < ms.ms_count && i < 16; i++)
		if (mi[i].mi_depth <= 8 && mi[i].mi_depth > mi[best].mi_depth)
			best = i;
	return best && ioctl(fbfd, FBIOSMODE, mi[best].mi_id) == 0 &&
	    ioctl(fbfd, FBIOGINFO, &fi) == 0 && fi.fi_layout == FBL_PLANES;
}

static unsigned short
tt2st(v)
	int v;
{
	return (v & 0xeee) >> 1 | (v & 0x111) << 3;
}

/* the screen as TOS set it; 0 before it set one */
static int
video(v)
	struct shifter *v;
{
	volatile struct tosvid *vd = (struct tosvid *)TOSVID;
	int i, bank;

	if (vd->vd_seq == 0)
		return 0;
	v->tv_base = vd->vd_base;
	v->tv_ttmode = vd->vd_mode & 0x70f;
	v->tv_stmode = v->tv_ttmode >> 8 & 3;
	bank = (v->tv_ttmode & 15) * 16;
	for (i = 0; i < 256; i++)
		v->tv_ttpal[i] = vd->vd_pal[i];
	for (i = 0; i < 16; i++)
		v->tv_stpal[i] = tt2st(v->tv_ttpal[bank + i]);
	return 1;
}

void
disp(fd, verbose, ram)
	int fd, verbose;
	unsigned long ram;
{
	struct fbacq acq;
	struct tosvideo kv;
	struct shifter tv, last;
	struct geom g;
	struct pollfd p[3];
	struct fbnote fn;
	struct evinfo ei;
	int i, k, kfd, mfd, have = 0, calm = 0;
	long t = -REFRESH, per = REFRESH;

	tfd = fd;
	ramsize = ram;
	for (i = 0; i < 256; i++)
		for (k = 0; k < 8; k++)
			if (i & (0x80 >> k))
				exl[i][k / 4] |= 1L << (8 * (3 - k % 4));
	for (i = 0; i < 256; i++)
		for (k = 0; k < 4; k++)
			if (i & 1 << 2 * k)
				even[i] |= 1 << k;
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
	if (fi.fi_type == FBT_AMIGA && fi.fi_depth != 8)
		planar = planes();
	if (planar) {
		if (fbpipe >= 0) {
			ioctl(fbpipe, I_SENDFD, fbfd);
			close(fbpipe);
		}
		goto input;
	}
	if (fi.fi_depth != 8 && (fi.fi_depth != 1 || fi.fi_layout != FBL_PACKED)) {
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
	if (fbpipe >= 0) {
		ioctl(fbpipe, I_SENDFD, fbfd);
		close(fbpipe);
	}
	shadow = (unsigned char *)malloc(160L * 960);
	memset(fb + fi.fi_offset, 0, fi.fi_rowbytes * fi.fi_height);
input:
	kfd = open("/dev/kbd", O_RDONLY);
	mfd = open("/dev/mouse", O_RDONLY);
	if (kfd >= 0 && ioctl(kfd, EVIOCBIND, fbfd) == 0 &&
	    ioctl(kfd, EVIOCGINFO, &ei) == 0)
		native = ei.ei_kset == EVK_IKBD;
	if (mfd >= 0)
		ioctl(mfd, EVIOCBIND, fbfd);
	p[0].fd = kfd;
	p[1].fd = mfd;
	p[2].fd = fbfd;
	p[0].events = p[1].events = p[2].events = POLLIN;
	for (;;) {
		i = t + per - now();
		if (poll(p, 3L, hidden ? 1000 : i > 0 ? i : 0) > 0) {
			for (i = 0; i < 2; i++)
				if (p[i].revents & POLLIN) {
					events(p[i].fd);
					calm = 0;
				}
			if ((p[2].revents & POLLIN) && read(fbfd, (char *)&fn, sizeof fn) == sizeof fn) {
				if (fn.fn_type == FBN_HIDDEN && !hidden)
					hide();
				else if (fn.fn_type == FBN_SHOWN && hidden) {
					ioctl(tfd, TOSIOC_PAUSE, 0);
				}
				hidden = fn.fn_type == FBN_HIDDEN ? 1 : fn.fn_type == FBN_SHOWN ? 0 : hidden;
				have = have && !hidden;
			}
		}
		if (getppid() == 1)
			return;
		drain();
		/* a screen that stays the same is compared less often, input or a change resets it */
		per = REFRESH << (calm < 3 * CALM ? calm / CALM : 3);
		if (hidden || now() - t < per)
			continue;
		/* a fixed cadence: the clock moves in ticks, not ms */
		t = now() - t < 2 * per ? t + per : now();
		if (ioctl(tfd, TOSIOC_VIDEO, &kv) < 0) {
			if (errno == ENXIO)	/* the guest has not started yet */
				continue;
			return;
		}
		if (!video(&tv) || fvdi(&tv) || planar || !geom(&tv, &g)) {
			have = 0;
			continue;
		}
		k = !have || tv.tv_base != last.tv_base || tv.tv_ttmode != last.tv_ttmode;
		if (k && verbose)
			fprintf(stderr, "starttos: screen mode %x at %lx\n", tv.tv_ttmode, tv.tv_base);
		if (k)
			memset(fb + fi.fi_offset, 0, fi.fi_rowbytes * fi.fi_height);
		if (k || memcmp((char *)tv.tv_stpal, (char *)last.tv_stpal, sizeof tv.tv_stpal) ||
		    memcmp((char *)tv.tv_ttpal, (char *)last.tv_ttpal, sizeof tv.tv_ttpal)) {
			palette(&tv, &g);
			calm = 0;
		}
		if (refresh(&tv, &g, k))
			calm = 0;
		else if (calm < 3 * CALM)
			calm++;
		last = tv;
		have = 1;
	}
}
