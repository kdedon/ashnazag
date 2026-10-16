#include "sys/types.h"
#include "sys/errno.h"
#include "screen.h"
#include "fbcons.h"
#include "adb.h"
#include "sys/poll.h"
#include "ds.h"
#include "sys/amrtg.h"

extern void *AllocMem();
extern void FreeMem(), normcop(), __amix_kbintr();
extern struct screen *activescreen;
extern int splscr();
extern void ds_am_console();
extern void splx();

long mac_socktrace;
struct fbcons fbcons;
struct fbpmode fbp_mode[FBP_NMODE];
int fbp_nmode, fbp_cur = -1;
void (*fbcons_panicfn)();
struct adbdev adb_dev[16] = { { ADB_ADDR_KBD, 2 }, { ADB_ADDR_MOUSE, 1 } };
static struct screen *am_screen, *am_console;
static struct bitmap am_bitmap;
static struct coplist am_view[2];
static unsigned short *am_copper;
static void (*am_key)(), (*am_mouse)();
static unsigned char am_toadb[128];
static int am_busy, am_button;
struct am_rtg *am_rtg;
char *am_dsname = "Amiga ECS";
int am_dscmapbits = 4;
unsigned long am_dsvsize;
static int am_lines;		/* display lines, interlace doubles them */

/*
 * Session modes: 640 wide, 1 to AM_PLANES bitplanes, interlaced from
 * AM_NMODE / 2 on.  The planes lie plane-height apart in one chip RAM
 * block, so a session maps them all at once.
 */
#define AM_PLANES	4
#define AM_NMODE	(2 * AM_PLANES)
#define AM_MODEID	0x90	/* + 8 interlaced, + planes */
#define AM_RTGID	0xa0	/* + bits a pixel */
static unsigned char am_toraw[128] = {
 0x20,0x21,0x22,0x23,0x25,0x24,0x31,0x32,
 0x33,0x34,0x30,0x35,0x10,0x11,0x12,0x13,
 0x15,0x14,0x01,0x02,0x03,0x04,0x06,0x05,
 0x0c,0x09,0x07,0x0b,0x08,0x0a,0x1b,0x18,
 0x16,0x1a,0x17,0x19,0x44,0x28,0x26,0x2a,
 0x27,0x29,0x0d,0x38,0x3a,0x36,0x37,0x39,
 0x42,0x40,0x00,0x41,0x43,0x45,0x63,0x66,
 0x60,0x62,0x64,0x4f,0x4e,0x4d,0x4c,0xff,
 0xff,0x3c,0xff,0x5d,0xff,0x5e,0xff,0xff,
 0xff,0xff,0xff,0x5c,0x43,0xff,0x4a,0xff,
 0xff,0xff,0x0f,0x1d,0x1e,0x1f,0x2d,0x2e,
 0x2f,0x3d,0xff,0x3e,0x3f,0xff,0xff,0xff,
 0x54,0x55,0x56,0x52,0x57,0x58,0xff,0xff,
 0xff,0xff,0xff,0xff,0xff,0x59,0xff,0xff,
 0xff,0xff,0x5f,0xff,0xff,0x46,0x53,0xff,
 0x51,0xff,0x50,0x61,0x65,0x63,0x67,0xff
};

static void
am_rebuild()
{
	int i;

	for (i = 0; i < 2; i++) {
		if (am_view[i].flags & CopF_BUSY)
			continue;
		normcop(am_screen, &am_view[i], am_copper + i * 256, 0);
		am_view[i].screen = am_screen;
		am_screen->coplist = &am_view[i];
		am_screen->flags &= ~Sf_NEEDCOP;
		NewCop(am_screen);
		return;
	}
	am_screen->flags |= Sf_NEEDCOP;
}

static void
am_misc(sp, cmd)
struct screen *sp;
int cmd;
{
	if (cmd == SC_BLDCOP || (cmd == SC_CHANGE && (sp->flags & Sf_NEEDCOP)))
		am_rebuild();
	if (cmd == SC_CHANGE && !(sp->flags & Sf_DISPLAY))
		ds_am_console();
	if (cmd == SC_VBCALL)
		ds_vblintr();
}

static void
am_input(sp, code, qual)
struct screen *sp;
unsigned long code, qual;
{
	int x, y;

	if ((code >> 24) == 1 && (code & 127) == 0x7c && am_mouse) {
		am_button = !(code & 128);
		(*am_mouse)(0, MOUSE_CHANGE, am_button ? 0 : 0x8000, 1);
	}
	if ((code >> 24) == 2 && am_mouse) {
		x = (code >> 8) & 255; y = code & 255;
		if (x & 128) x -= 256;
		if (y & 128) y -= 256;
		while (x || y) {
			int dx = x > 63 ? 63 : x < -64 ? -64 : x;
			int dy = y > 63 ? 63 : y < -64 ? -64 : y;
			(*am_mouse)(0, MOUSE_CHANGE, ((unsigned long)(dy & 127) << 8) | (dx & 127) |
			    (am_button ? 0 : 0x8000), 2);
			x -= dx; y -= dy;
		}
	}
}

/* the card's depths when it has more than 8 bits; then each session maps the deepest */
int am_dsfull;
unsigned long am_dsphys, am_dsvram;

/* a free frame of VRAM for a new session, its offset; 0 if none */
unsigned long
am_vslot()
{
	unsigned long off;
	int i;

	if (!am_rtg || !am_rtg->r_pan)
		return 0;
	for (off = ds_disp.d_vsize; off + ds_disp.d_vsize <= am_dsvram; off += ds_disp.d_vsize) {
		for (i = 1; i <= DS_NSESS; i++)
			if (ds_sess[i].s_used && ds_sess[i].s_voff == off)
				break;
		if (i > DS_NSESS)
			return off;
	}
	return 0;
}

/* bits a pixel of the card's mode n */
static int
am_rtgbits(n)
int n;
{
	int i;

	for (i = 0; i < 3; i++)
		if ((am_rtg->r_depths & 1 << i) && n-- == 0)
			return 8 << i;
	return 0;
}

int
am_nvmode()
{
	int i, n = 0;

	if (!am_rtg)
		return AM_NMODE;
	if (am_dsfull)
		for (i = 0; i < 3; i++)
			if (am_rtg->r_depths & 1 << i)
				n++;
	return n;
}

void
am_vminfo(n, fi)
int n;
struct fbinfo *fi;
{
	int d = n % AM_PLANES + 1, lace = n >= AM_PLANES;

	*fi = ds_disp.d_info;
	if (am_rtg) {
		d = am_rtgbits(n);
		fi->fi_mode = AM_RTGID + d;
		fi->fi_depth = d;
		fi->fi_rowbytes = am_rtg->r_row * (d / 8);
		if (d > 8) {
			fi->fi_visual = FBV_TRUE;
			fi->fi_rmask = am_rtg->r_rgb[d / 32][0];
			fi->fi_gmask = am_rtg->r_rgb[d / 32][1];
			fi->fi_bmask = am_rtg->r_rgb[d / 32][2];
			fi->fi_cmapsize = 0;
			fi->fi_flags &= ~(FBF_CMAP | FBF_BLANK);
		}
		return;
	}
	fi->fi_layout = FBL_PLANES;
	fi->fi_mode = AM_MODEID + (lace ? 8 : 0) + d;
	fi->fi_height = am_lines << lace;
	fi->fi_depth = d;
	fi->fi_planebytes = fi->fi_rowbytes * fi->fi_height;
	fi->fi_size = (fi->fi_offset + d * fi->fi_planebytes + DS_PGOFF) & ~DS_PGOFF;
	fi->fi_cmapsize = 1 << d;
	fi->fi_visual = d == 1 ? FBV_MONO : FBV_PSEUDO;
}

int
am_vmfind(id)
unsigned long id;
{
	int n;

	for (n = 0; n < am_nvmode(); n++)
		if (am_rtg ? id == AM_RTGID + am_rtgbits(n) :
		    id == AM_MODEID + (n >= AM_PLANES ? 8 : 0) + n % AM_PLANES + 1)
			return n;
	return -1;
}

/* the screen shows s's planes: its mode's, else one */
void
am_planes(s)
struct dssess *s;
{
	struct fbinfo fi;
	int i;

	if (am_rtg) {
		if (am_dsfull)
			(*am_rtg->r_setdepth)(s->s_vmode ? am_rtgbits(s->s_vmode - 1) : 8);
		return;
	}
	if (s->s_vmode)
		am_vminfo(s->s_vmode - 1, &fi);
	else {
		fi = ds_disp.d_info;
		fi.fi_planebytes = 0;
	}
	am_bitmap.depth = am_screen->depth = fi.fi_depth;
	am_bitmap.height = am_screen->height = fi.fi_height;
	for (i = 0; i < fi.fi_depth; i++)
		am_bitmap.bpl[i] = (unsigned char *)ds_disp.d_base + i * fi.fi_planebytes;
	if (fi.fi_height > am_lines)
		am_screen->modes |= Sm_ILACE;
	else
		am_screen->modes &= ~Sm_ILACE;
	am_rebuild();
}

/* the card shows s's frame from its pan offset */
void
am_pan(s)
struct dssess *s;
{
	if (am_rtg && am_rtg->r_pan)
		(*am_rtg->r_pan)(s->s_voff + s->s_vpan);
}

static void
am_front(s)
struct dssess *s;
{
	struct screen *sp;

	if (s != &ds_sess[0]) {
		am_planes(s);
		if (activescreen != am_screen)
			am_console = activescreen;
		sp = am_screen;
	} else {
		if (activescreen != am_screen && activescreen && (activescreen->flags & Sf_INUSE))
			am_console = activescreen;
		sp = am_console;
		if (sp == 0 || !(sp->flags & Sf_INUSE))
			return;
	}
	DisplayScreen(sp);
	SelectScreen(sp);
	am_pan(s);
	if (am_rtg)
		(*am_rtg->r_show)(s != &ds_sess[0]);
}

int
am_rtgattach(r)
struct am_rtg *r;
{
	if (am_screen || am_rtg)
		return EBUSY;
	am_rtg = r;
	return 0;
}

/* EBUSY when an attach would fail, before a card is touched */
int
am_rtgbusy()
{
	return am_screen || am_rtg ? EBUSY : 0;
}

int
am_rtgdetach(r)
struct am_rtg *r;
{
	if (am_rtg != r)
		return 0;
	if (am_screen)
		return EBUSY;
	am_rtg = 0;
	return 0;
}

int
am_dsinit()
{
	unsigned char *mem;
	unsigned long size, height;
	int i;

	if (am_screen)
		return 0;
	height = (displaytype & DT_PAL) ? 256 : 200;
	size = am_rtg ? 80 * height : 80 * 2 * height * AM_PLANES;
	size = (size + DS_PGOFF) & ~DS_PGOFF;
	mem = (unsigned char *)AllocMem(size + DS_PGOFF, 2 | 0x10000);
	if (!mem)
		return ENOMEM;
	am_copper = (unsigned short *)AllocMem(1024, 2 | 0x10000);
	if (!am_copper) {
		FreeMem(mem, size + DS_PGOFF);
		return ENOMEM;
	}
	am_screen = OpenScreen();
	if (!am_screen) {
		FreeMem(am_copper, 1024); am_copper = 0;
		FreeMem(mem, size + DS_PGOFF);
		return ENOMEM;
	}
	am_bitmap.width = 640; am_bitmap.height = height; am_bitmap.depth = 1;
	am_bitmap.bpl[0] = (unsigned char *)(((unsigned long)mem + DS_PGOFF) & ~DS_PGOFF);
	am_bitmap.flags = Bf_ACTIVE;
	am_screen->bmap = &am_bitmap;
	am_screen->width = 640; am_screen->height = height; am_screen->depth = 1;
	am_screen->modes = Sm_HIRES;
	am_screen->flags |= Sf_RAWKEY | Sf_VBCALL;
	am_screen->mifunc = am_misc; am_screen->kbfunc = am_input;
	am_screen->color[0] = 0xfff; am_screen->color[1] = 0;
	am_rebuild();
	for (i = 0; i < 128; i++) am_toadb[i] = 255;
	for (i = 0; i < 128; i++)
		if (am_toraw[i] != 255) am_toadb[am_toraw[i]] = i;
	am_console = activescreen;
	am_lines = height;
	am_dsvsize = am_rtg ? 0 : size;
	am_dsfull = am_rtg && am_rtg->r_setdepth && (am_rtg->r_depths & 6);
	/* a page over: the guest's allocator keeps bytes between bitmaps, so two screens at half depth fit */
	if (am_dsfull)
		am_dsvsize = (am_rtg->r_base & DS_PGOFF) + (unsigned long)am_rtg->r_row *
		    am_rtg->r_height * (am_rtg->r_depths & 4 ? 4 : 2) + DS_PGSIZE;
	if (am_rtg) {
		am_dsphys = am_rtg->r_phys ? am_rtg->r_phys - am_rtg->r_base : 0;
		am_dsvram = am_rtg->r_pan ? am_rtg->r_vram : 0;
		fbcons.fc_m.fm_base = am_rtg->r_base;
		fbcons.fc_m.fm_width = am_rtg->r_width; fbcons.fc_m.fm_height = am_rtg->r_height;
		fbcons.fc_m.fm_depth = am_rtg->r_depth; fbcons.fc_m.fm_row = am_rtg->r_row;
		am_dsname = am_rtg->r_name;
		am_dscmapbits = 8;
	} else {
		fbcons.fc_m.fm_base = (unsigned long)am_bitmap.bpl[0];
		fbcons.fc_m.fm_width = 640; fbcons.fc_m.fm_height = height;
		fbcons.fc_m.fm_depth = 1; fbcons.fc_m.fm_row = 80;
	}
	fbcons.fc_on = 1;
	ds_frontfn = am_front;
	return 0;
}

void
am_dsclut(s, lo, hi)
struct dssess *s;
int lo, hi;
{
	int i;

	if (!am_screen) return;
	if (am_rtg) {
		for (i = lo; i < hi && i < 256; i++)
			if (s->s_blank)
				(*am_rtg->r_clut)(i, 0, 0, 0);
			else
				(*am_rtg->r_clut)(i, s->s_cmap[0][i] >> 8, s->s_cmap[1][i] >> 8,
				    s->s_cmap[2][i] >> 8);
		return;
	}
	for (i = lo; i < hi && i < 1 << am_screen->depth; i++)
		am_screen->color[i] = s->s_blank ? 0 :
		    (s->s_cmap[0][i] >> 12) << 8 | (s->s_cmap[1][i] >> 12) << 4 |
		    (s->s_cmap[2][i] >> 12);
	am_rebuild();
}

void (*adb_keyhook(fn))()
void (*fn)();
{
	void (*old)() = am_key;
	am_key = fn;
	return old;
}
void (*adb_mousehook(fn))()
void (*fn)();
{
	void (*old)() = am_mouse;
	am_mouse = fn;
	return old;
}
void adbkbd_setleds(v) int v; { }
void adbkbd_cons(v) int v; { }
int fbcons_grab() { if (am_busy) return 0; am_busy = 1; return 1; }
void fbcons_unlock() { am_busy = 0; }
/* the console is a screen of its own */
void fbcons_hide() { }
void fbcons_show() { }

/* Keep the native handshake and console path while mirroring raw keys. */
void
kbintr()
{
	unsigned char raw, code, adb;

	raw = ~*(__volatile__ unsigned char *)0xbfec01;	/* CIA-A serial data */
	code = raw >> 1;
	if (am_key && code < 0x68) {
		adb = am_toadb[code];
		if (adb != 255) (*am_key)(0, KC_CHAR, adb | ((raw & 1) ? 128 : 0), 0);
	}
	__amix_kbintr();
}
