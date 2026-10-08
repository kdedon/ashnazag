/*
 * scr_fb.c -- the screen and input on the display service: a session on
 * /dev/fb0, its keyboards and mice through /dev/kbd and /dev/mouse.
 *
 * We draw in an 8-bit shadow of the screen (the system palette) and
 * copy what changed to the frame buffer at each flush, converting to
 * its depth and layout (8-bit packed with the palette in the CLUT;
 * 16 or 32-bit direct colour; 1-bit; 4 or 8 interleaved bitplanes),
 * then draw the pointer over it.  Key codes of each keyboard family
 * (ADB, Amiga, IKBD) become virtual keys; a key's PC scan code goes in
 * lParam as Windows has it.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "win.h"
#include "scr.h"
#include "dsio.h"

struct surf screen;
int scr_mx, scr_my;
char *scr_shot, *scr_script;

static int fbfd = -1, kfd = -1, mfd = -1, kset;
static struct fbinfo fi;
static u8 *fb;
static struct rect dirty;
static int hidden, ncol;
static struct cursor *cur;
static struct rect curr;		/* where the pointer is drawn */
static u8 map16[256];			/* a palette index to the device's, when it has fewer */
static u32 truecol[256];		/* direct colour pixels */
static struct ev pend[64];
static int npend, ppos;
static int btns;

/* ---- key codes ---- */

/* ADB (US) to virtual keys; Command and Option are Alt */
static u8 adbvk[128] = {
	'A', 'S', 'D', 'F', 'H', 'G', 'Z', 'X', 'C', 'V', 0xe2, 'B', 'Q', 'W', 'E', 'R',
	'Y', 'T', '1', '2', '3', '4', '6', '5', 0xbb, '9', '7', 0xbd, '8', '0', 0xdd, 'O',
	'U', 0xdb, 'I', 'P', VK_RETURN, 'L', 'J', 0xde, 'K', 0xba, 0xdc, 0xbc, 0xbf, 'N', 'M', 0xbe,
	VK_TAB, VK_SPACE, 0xc0, VK_BACK, VK_RETURN, VK_ESCAPE, VK_CONTROL, VK_MENU,
	VK_SHIFT, VK_CAPITAL, VK_MENU, VK_LEFT, VK_RIGHT, VK_DOWN, VK_UP, 0,
	0, VK_DECIMAL, 0, VK_MULTIPLY, 0, VK_ADD, 0, VK_NUMLOCK, 0, 0, 0, VK_DIVIDE, VK_RETURN, 0, VK_SUBTRACT, 0,
	0, 0xbb, VK_NUMPAD0, VK_NUMPAD0 + 1, VK_NUMPAD0 + 2, VK_NUMPAD0 + 3, VK_NUMPAD0 + 4, VK_NUMPAD0 + 5,
	VK_NUMPAD0 + 6, VK_NUMPAD0 + 7, 0, VK_NUMPAD0 + 8, VK_NUMPAD0 + 9, 0, 0, 0,
	VK_F1 + 4, VK_F1 + 5, VK_F1 + 6, VK_F1 + 2, VK_F1 + 7, VK_F1 + 8, 0, VK_F1 + 10,
	0, VK_F1 + 12, 0, VK_F1 + 13, 0, VK_F1 + 9, 0, VK_F1 + 11,
	0, VK_F1 + 14, VK_INSERT, VK_HOME, VK_PRIOR, VK_DELETE, VK_F1 + 3, VK_END,
	VK_F1 + 1, VK_NEXT, VK_F1, VK_SHIFT, VK_MENU, VK_CONTROL, 0, 0
};

/* Amiga raw codes to virtual keys; the Amiga keys are Alt too */
static u8 amigavk[128] = {
	0xc0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xbd, 0xbb, 0xdc, 0, VK_NUMPAD0,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xdb, 0xdd, 0, VK_NUMPAD0 + 1, VK_NUMPAD0 + 2, VK_NUMPAD0 + 3,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xba, 0xde, 0, 0, VK_NUMPAD0 + 4, VK_NUMPAD0 + 5, VK_NUMPAD0 + 6,
	0xe2, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 0xbc, 0xbe, 0xbf, 0, VK_DECIMAL, VK_NUMPAD0 + 7, VK_NUMPAD0 + 8, VK_NUMPAD0 + 9,
	VK_SPACE, VK_BACK, VK_TAB, VK_RETURN, VK_RETURN, VK_ESCAPE, VK_DELETE, 0, 0, 0, VK_SUBTRACT, 0, VK_UP, VK_DOWN, VK_RIGHT, VK_LEFT,
	VK_F1, VK_F1 + 1, VK_F1 + 2, VK_F1 + 3, VK_F1 + 4, VK_F1 + 5, VK_F1 + 6, VK_F1 + 7,
	VK_F1 + 8, VK_F1 + 9, VK_NUMLOCK, VK_SCROLL, VK_DIVIDE, VK_MULTIPLY, VK_ADD, VK_HELP,
	VK_SHIFT, VK_SHIFT, VK_CAPITAL, VK_CONTROL, VK_MENU, VK_MENU, VK_MENU, VK_MENU
};

/* Atari IKBD scan codes (the PC's set 1, mostly) to virtual keys */
static u8 ikbdvk[128] = {
	0, VK_ESCAPE, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xbd, 0xbb, VK_BACK, VK_TAB,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xdb, 0xdd, VK_RETURN, VK_CONTROL, 'A', 'S',
	'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xba, 0xde, 0xc0, VK_SHIFT, 0xdc, 'Z', 'X', 'C', 'V',
	'B', 'N', 'M', 0xbc, 0xbe, 0xbf, VK_SHIFT, 0, VK_MENU, VK_SPACE, VK_CAPITAL, VK_F1, VK_F1 + 1, VK_F1 + 2, VK_F1 + 3, VK_F1 + 4,
	VK_F1 + 5, VK_F1 + 6, VK_F1 + 7, VK_F1 + 8, VK_F1 + 9, 0, 0, VK_HOME, VK_UP, 0, VK_SUBTRACT, VK_LEFT, 0, VK_RIGHT, VK_ADD, 0,
	VK_DOWN, 0, VK_INSERT, VK_DELETE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, VK_F1 + 10, VK_F1 + 11, 0, 0, VK_DIVIDE, VK_MULTIPLY, VK_NUMPAD0 + 7, VK_NUMPAD0 + 8, VK_NUMPAD0 + 9,
	VK_NUMPAD0 + 4, VK_NUMPAD0 + 5, VK_NUMPAD0 + 6, VK_NUMPAD0 + 1, VK_NUMPAD0 + 2, VK_NUMPAD0 + 3,
	VK_NUMPAD0, VK_DECIMAL, VK_RETURN, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* the PC scan code of a virtual key, for lParam */
static int
vkscan(vk)
	int vk;
{
	static char *row = "QWERTYUIOP\0\0\0\0ASDFGHJKL\0\0\0\0\0ZXCVBNM";
	char *p;
	int i;

	if (vk >= 'A' && vk <= 'Z' && (p = strchr(row, vk)) != 0) {
		i = p - row;
		return i < 10 ? 0x10 + i : i < 23 ? 0x1e + i - 14 : 0x2c + i - 28;
	}
	if (vk >= '1' && vk <= '9')
		return 2 + vk - '1';
	if (vk == '0')
		return 11;
	if (vk >= VK_F1 && vk < VK_F1 + 10)
		return 0x3b + vk - VK_F1;
	for (i = 0; i < 128; i++)
		if (ikbdvk[i] == vk)
			return i;
	return 0;
}

/* ---- opening ---- */

int
scr_open(w, h, flags)
	int w, h, flags;
{
	struct fbacq acq;
	struct fbcmap cm;
	struct evinfo ei;
	u16 r[256], g[256], b[256];
	int i, j, best, d, bd;

	if ((fbfd = open("/dev/fb0", O_RDWR)) < 0) {
		perror("startwin: /dev/fb0");
		return -1;
	}
	memset((char *)&acq, 0, sizeof acq);
	acq.fa_kind = FBK_USER;
	acq.fa_flags = FBA_FRONT;
	strcpy(acq.fa_name, "win16");
	if (ioctl(fbfd, FBIOACQUIRE, &acq) < 0 || ioctl(fbfd, FBIOGINFO, &fi) < 0) {
		perror("startwin: display session");
		return -1;
	}
	if (fi.fi_layout == FBL_PACKED && fi.fi_depth == 8)
		ioctl(fbfd, FBIOCACHE, FBC_WT);
	else
		ioctl(fbfd, FBIOCACHE, FBC_CI);
	fb = (u8 *)mmap((caddr_t)0, fi.fi_size, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fb == (u8 *)-1) {
		perror("startwin: mmap /dev/fb0");
		return -1;
	}
	fb += fi.fi_offset;
	/* the screen is the display's size (as much of it as we draw) */
	screen.w = fi.fi_width > 1280 ? 1280 : fi.fi_width;
	screen.h = fi.fi_height > 1024 ? 1024 : fi.fi_height;
	screen.rowb = screen.w;
	screen.pix = (u8 *)calloc(1, screen.w * screen.h);
	/* colours */
	ncol = fi.fi_visual == FBV_PSEUDO ? (fi.fi_cmapsize < 256 ? fi.fi_cmapsize : 256) : 0;
	if (ncol) {
		for (i = 0; i < 256; i++) {
			r[i] = syspal[i][0] * 0x101;
			g[i] = syspal[i][1] * 0x101;
			b[i] = syspal[i][2] * 0x101;
		}
		if (ncol < 256) {
			/* the device's first entries get the static colours, the rest map to the nearest */
			static int keep[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 248, 249, 250, 251, 252, 253, 254, 255 };

			for (i = 0; i < ncol && i < 16; i++) {
				r[i] = syspal[keep[i]][0] * 0x101;
				g[i] = syspal[keep[i]][1] * 0x101;
				b[i] = syspal[keep[i]][2] * 0x101;
			}
			for (i = 0; i < 256; i++) {
				bd = 1 << 30;
				best = 0;
				for (j = 0; j < ncol && j < 16; j++) {
					d = (syspal[i][0] - syspal[keep[j]][0]) * (syspal[i][0] - syspal[keep[j]][0]) +
					    (syspal[i][1] - syspal[keep[j]][1]) * (syspal[i][1] - syspal[keep[j]][1]) +
					    (syspal[i][2] - syspal[keep[j]][2]) * (syspal[i][2] - syspal[keep[j]][2]);
					if (d < bd)
						bd = d, best = j;
				}
				map16[i] = best;
			}
		}
		cm.cm_start = 0;
		cm.cm_count = ncol;
		cm.cm_red = r;
		cm.cm_green = g;
		cm.cm_blue = b;
		ioctl(fbfd, FBIOPUTCMAP, &cm);
	} else if (fi.fi_visual == FBV_TRUE) {
		for (i = 0; i < 256; i++) {
			u32 px = 0, m;
			int c;

			for (c = 0; c < 3; c++) {
				m = c == 0 ? fi.fi_rmask : c == 1 ? fi.fi_gmask : fi.fi_bmask;
				if (m) {
					int sh = 0, bits = 0;

					while (!(m >> sh & 1))
						sh++;
					while (m >> (sh + bits) & 1)
						bits++;
					px |= ((u32)syspal[i][c] >> (8 - bits)) << sh;
				}
			}
			truecol[i] = px;
		}
	}
	/* input */
	kfd = open("/dev/kbd", O_RDONLY);
	mfd = open("/dev/mouse", O_RDONLY);
	if (kfd >= 0) {
		ioctl(kfd, EVIOCBIND, fbfd);
		if (ioctl(kfd, EVIOCGINFO, &ei) == 0)
			kset = ei.ei_kset;
	}
	if (mfd >= 0)
		ioctl(mfd, EVIOCBIND, fbfd);
	scr_mx = screen.w / 2;
	scr_my = screen.h / 2;
	r_set(&dirty, 0, 0, screen.w, screen.h);
	return 0;
}

void
scr_close()
{
	if (fbfd >= 0) {
		ioctl(fbfd, FBIORELEASE, 0);
		close(fbfd);
	}
	fbfd = -1;
}

void
scr_dirty(r)
	struct rect *r;
{
	if (R_EMPTY(r))
		return;
	if (R_EMPTY(&dirty))
		dirty = *r;
	else {
		if (r->l < dirty.l) dirty.l = r->l;
		if (r->t < dirty.t) dirty.t = r->t;
		if (r->r > dirty.r) dirty.r = r->r;
		if (r->b > dirty.b) dirty.b = r->b;
	}
}

/* ---- copying to the device ---- */

static void
putrow(y, x0, x1, src)
	int y, x0, x1;
	u8 *src;		/* palette indexes for x0..x1 */
{
	u8 *d = fb + y * fi.fi_rowbytes;
	int x, p, k;

	if (fi.fi_layout == FBL_PACKED) {
		switch (fi.fi_depth) {
		case 8:
			if (ncol == 256)
				memcpy(d + x0, src, x1 - x0);
			else
				for (x = x0; x < x1; x++)
					d[x] = map16[src[x - x0]];
			return;
		case 4:
			for (x = x0; x < x1; x++) {
				int v = ncol ? map16[src[x - x0]] : src[x - x0] & 15;

				d[x >> 1] = x & 1 ? (d[x >> 1] & 0xf0) | v : (d[x >> 1] & 0x0f) | v << 4;
			}
			return;
		case 1:
			for (x = x0; x < x1; x++) {
				int i = src[x - x0], white = syspal[i][0] + syspal[i][1] + syspal[i][2] >= 384;
				/* FBV_MONO: 0 is white */
				int on = fi.fi_visual == FBV_MONO ? !white : white;

				if (on)
					d[x >> 3] |= 0x80 >> (x & 7);
				else
					d[x >> 3] &= ~(0x80 >> (x & 7));
			}
			return;
		case 16:
			for (x = x0; x < x1; x++)
				((u16 *)d)[x] = truecol[src[x - x0]];
			return;
		case 32:
			for (x = x0; x < x1; x++)
				((u32 *)d)[x] = truecol[src[x - x0]];
			return;
		}
		return;
	}
	if (fi.fi_layout == FBL_IPLAN2) {
		/* interleaved planes: 16 pixels a group of fi_depth words */
		int planes = fi.fi_depth, g0 = x0 >> 4, g1 = (x1 + 15) >> 4, gi;

		for (gi = g0; gi < g1; gi++) {
			u16 w[8];
			u8 *wp = d + gi * 2 * planes;

			for (p = 0; p < planes; p++)
				w[p] = wp[2 * p] << 8 | wp[2 * p + 1];
			for (k = 0; k < 16; k++) {
				x = gi * 16 + k;
				if (x < x0 || x >= x1)
					continue;
				{
					int v = ncol && ncol < 256 ? map16[src[x - x0]] : src[x - x0];

					for (p = 0; p < planes; p++)
						if (v >> p & 1)
							w[p] |= 0x8000 >> k;
						else
							w[p] &= ~(0x8000 >> k);
				}
			}
			for (p = 0; p < planes; p++) {
				wp[2 * p] = w[p] >> 8;
				wp[2 * p + 1] = w[p];
			}
		}
	}
}

/* the pointer over the frame buffer at (scr_mx, scr_my) */
static void
drawcursor()
{
	u8 row[64];
	int x, y, w, h, x0, y0, k;

	if (!cur)
		return;
	x0 = scr_mx - cur->hx;
	y0 = scr_my - cur->hy;
	w = cur->w > 64 ? 64 : cur->w;
	h = cur->h;
	r_set(&curr, x0, y0, x0 + w, y0 + h);
	for (y = 0; y < h; y++) {
		int sy = y0 + y, xa = x0 < 0 ? -x0 : 0, xb = x0 + w > screen.w ? screen.w - x0 : w;

		if (sy < 0 || sy >= screen.h || xb <= xa)
			continue;
		for (x = xa; x < xb; x++) {
			k = y * cur->w + x;
			row[x] = screen.pix[sy * screen.rowb + x0 + x];
			if (!cur->and[k])
				row[x] = cur->xor[k] ? 255 : 0;
			else if (cur->xor[k])
				row[x] = 255 - row[x];
		}
		putrow(sy, x0 + xa, x0 + xb, row + xa);
	}
}

void
scr_flush()
{
	struct rect r;
	int y;

	if (hidden || !fb)
		return;
	r = dirty;
	/* where the pointer was comes back from the shadow */
	if (!R_EMPTY(&curr))
		scr_dirty(&curr), r = dirty;
	if (r.l < 0) r.l = 0;
	if (r.t < 0) r.t = 0;
	if (r.r > screen.w) r.r = screen.w;
	if (r.b > screen.h) r.b = screen.h;
	if (!R_EMPTY(&r))
		for (y = r.t; y < r.b; y++)
			putrow(y, r.l, r.r, screen.pix + y * screen.rowb + r.l);
	r_set(&dirty, 0, 0, 0, 0);
	r_set(&curr, 0, 0, 0, 0);
	drawcursor();
}

void
scr_setcursor(c)
	struct cursor *c;
{
	if (c == cur)
		return;
	if (!R_EMPTY(&curr))
		scr_dirty(&curr);
	cur = c;
}

void
scr_warp(x, y)
	int x, y;
{
	if (!R_EMPTY(&curr))
		scr_dirty(&curr);
	scr_mx = x;
	scr_my = y;
}

void
scr_beep()
{
}

/* ---- input ---- */

static void
add(type, x, y, down, vk, btn)
	int type, x, y, down, vk, btn;
{
	struct ev *e;

	if (npend >= 64)
		return;
	e = &pend[npend++];
	memset(e, 0, sizeof *e);
	e->type = type;
	e->x = x;
	e->y = y;
	e->down = down;
	e->vk = vk;
	e->sc = vkscan(vk);
	e->btn = btn;
}

static void
readinput(fd)
	int fd;
{
	struct inev ev[16];
	int n, i, vk, moved = 0;

	n = read(fd, (char *)ev, sizeof ev);
	if (n <= 0)
		return;
	n /= sizeof ev[0];
	for (i = 0; i < n; i++) {
		switch (ev[i].ie_type) {
		case IE_KEY:
			vk = ev[i].ie_code < 128 ? (kset == EVK_AMIGA ? amigavk : kset == EVK_IKBD ? ikbdvk :
			    adbvk)[ev[i].ie_code] : 0;
			if (vk)
				add(EV_KEY, 0, 0, ev[i].ie_value != 0, vk, 0);
			break;
		case IE_REL:
			if (!R_EMPTY(&curr))
				scr_dirty(&curr);
			if (ev[i].ie_code == IE_RELX)
				scr_mx += ev[i].ie_value * (abs(ev[i].ie_value) > 6 ? 2 : 1);
			else
				scr_my += ev[i].ie_value * (abs(ev[i].ie_value) > 6 ? 2 : 1);
			if (scr_mx < 0) scr_mx = 0;
			if (scr_my < 0) scr_my = 0;
			if (scr_mx >= screen.w) scr_mx = screen.w - 1;
			if (scr_my >= screen.h) scr_my = screen.h - 1;
			moved = 1;
			break;
		case IE_BTN:
			if (moved) {
				add(EV_MOVE, scr_mx, scr_my, 0, 0, 0);
				moved = 0;
			}
			{
				int b = ev[i].ie_code == 1 ? 0 : ev[i].ie_code == 2 ? 1 : 2;

				if (ev[i].ie_value)
					btns |= 1 << b;
				else
					btns &= ~(1 << b);
				add(EV_BTN, scr_mx, scr_my, ev[i].ie_value != 0, 0, b);
			}
			break;
		}
	}
	if (moved)
		add(EV_MOVE, scr_mx, scr_my, 0, 0, 0);
}

int
scr_poll(e, timeout)
	struct ev *e;
	int timeout;
{
	struct pollfd p[3];
	struct fbnote fn;
	int n;

	for (;;) {
		if (ppos < npend) {
			*e = pend[ppos++];
			if (ppos == npend)
				ppos = npend = 0;
			return 1;
		}
		p[0].fd = kfd;
		p[1].fd = mfd;
		p[2].fd = fbfd;
		p[0].events = p[1].events = p[2].events = POLLIN;
		p[0].revents = p[1].revents = p[2].revents = 0;
		if (!hidden)
			scr_flush();
		n = poll(p, 3L, timeout);
		if (n <= 0)
			return 0;
		if (p[0].revents & POLLIN)
			readinput(kfd);
		if (p[1].revents & POLLIN)
			readinput(mfd);
		if ((p[2].revents & POLLIN) && read(fbfd, (char *)&fn, sizeof fn) == sizeof fn) {
			if (fn.fn_type == FBN_HIDDEN) {
				hidden = 1;
				add(EV_HIDDEN, 0, 0, 0, 0, 0);
			} else if (fn.fn_type == FBN_SHOWN) {
				hidden = 0;
				r_set(&dirty, 0, 0, screen.w, screen.h);
				add(EV_SHOWN, 0, 0, 0, 0, 0);
			}
		}
		if (npend == 0 && timeout >= 0)
			timeout = 0;
	}
}
