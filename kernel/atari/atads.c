/*
 * atads.c -- display service input on the Falcon.  IKBD keys reach the
 * service as ADB key codes and relative mouse packets as motion and
 * buttons, so clients see the same events as on the Mac.  The IKBD
 * interrupt only queues; the service's tick delivers.
 */
#include "sys/types.h"
#include "adb.h"
#include "sys/poll.h"
#include "fbcons.h"
#include "ds.h"

extern int ata_spltty();
extern void ata_splx(), ikbd_dsmode(), ikbd_cons(), ds_relmouse();

struct adbdev adb_dev[16] = { { ADB_ADDR_KBD, 2 }, { ADB_ADDR_MOUSE, 1 } };
struct fbpmode fbp_mode[FBP_NMODE];
int fbp_nmode = 0, fbp_cur = -1;

#define NONE	0xFF
#define I_CAPS	0x3A
#define A_CAPS	0x39

/* IKBD scancode to ADB key code; Alternate is Command */
static unsigned char ad_toadb[0x73] = {
	NONE, 0x35, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16,	/* 00 */
	0x1A, 0x1C, 0x19, 0x1D, 0x1B, 0x18, 0x33, 0x30,
	0x0C, 0x0D, 0x0E, 0x0F, 0x11, 0x10, 0x20, 0x22,	/* 10 */
	0x1F, 0x23, 0x21, 0x1E, 0x24, 0x36, 0x00, 0x01,
	0x02, 0x03, 0x05, 0x04, 0x26, 0x28, 0x25, 0x29,	/* 20 */
	0x27, 0x32, 0x38, 0x2A, 0x06, 0x07, 0x08, 0x09,
	0x0B, 0x2D, 0x2E, 0x2B, 0x2F, 0x2C, 0x7B, NONE,	/* 30 */
	0x37, 0x31, 0x39, 0x7A, 0x78, 0x63, 0x76, 0x60,
	0x61, 0x62, 0x64, 0x65, 0x6D, NONE, NONE, 0x73,	/* 40: F6.., ClrHome */
	0x3E, NONE, 0x4E, 0x3B, NONE, 0x3C, 0x45, NONE,
	0x3D, NONE, 0x72, 0x75, NONE, NONE, NONE, NONE,	/* 50: Insert, Delete */
	NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE,
	0x0A, 0x6B, 0x69, 0x47, 0x51, 0x4B, 0x43, 0x59,	/* 60: ISO, Undo, Help, ( ) */
	0x5B, 0x5C, 0x56, 0x57, 0x58, 0x53, 0x54, 0x55,
	0x52, 0x41, 0x4C,				/* 70 */
};
static unsigned char ad_toikbd[0x80] = { 1 };

#define NREC	64
static unsigned char ad_rec[NREC][3];
static int ad_put, ad_get, ad_caps;
static void (*ad_key)(), (*ad_mouse)();

static void
ad_mode()
{
	register int i;

	for (i = 0; i < 0x80; i++)
		ad_toikbd[i] = 0;
	for (i = 0; i < 0x73; i++)
		if (ad_toadb[i] != NONE)
			ad_toikbd[ad_toadb[i]] = i;
	ikbd_dsmode(ad_key != 0, ad_mouse != 0);
}

void (*
adb_keyhook(fn))()
void (*fn)();
{
	void (*o)() = ad_key;

	ad_key = fn;
	ad_mode();
	return o;
}

void (*
adb_mousehook(fn))()
void (*fn)();
{
	void (*o)() = ad_mouse;

	ad_mouse = fn;
	ad_mode();
	return o;
}

/* From the IKBD interrupt: key (0, scancode) or mouse (header, dx, dy). */
void
ata_dsrec(h, a, b)
int h, a, b;
{
	register int n = (ad_put + 1) % NREC;

	if (n == ad_get)
		return;
	ad_rec[ad_put][0] = h;
	ad_rec[ad_put][1] = a;
	ad_rec[ad_put][2] = b;
	ad_put = n;
}

static int
ad_sext(v)
int v;
{
	return (v & 0x80) ? v - 0x100 : v;
}

/*
 * Deliver what the interrupt queued.  Caps Lock latches as on ADB:
 * down when it locks, up when it unlocks.
 */
void
ata_dspoll()
{
	unsigned char r[3];
	register int s, c, up;

	for (;;) {
		s = ata_spltty();
		if (ad_get == ad_put) {
			ata_splx(s);
			return;
		}
		r[0] = ad_rec[ad_get][0];
		r[1] = ad_rec[ad_get][1];
		r[2] = ad_rec[ad_get][2];
		ad_get = (ad_get + 1) % NREC;
		ata_splx(s);
		if (r[0]) {
			if (ad_mouse)	/* IKBD: bit 1 left, bit 0 right */
				ds_relmouse(((r[0] & 2) >> 1) | ((r[0] & 1) << 1),
				    ad_sext(r[1]), ad_sext(r[2]));
			continue;
		}
		up = r[1] & 0x80;
		c = r[1] & 0x7F;
		if (c >= 0x73 || ad_toadb[c] == NONE || ad_key == 0)
			continue;
		if (c == I_CAPS) {
			if (up)
				continue;
			ad_caps = !ad_caps;
			up = ad_caps ? 0 : 0x80;
		}
		(*ad_key)(0, KC_CHAR, ad_toadb[c] | up, 0);
	}
}

/* A key for the console session, as an ADB code; Caps Lock toggles. */
void
adbkbd_cons(b)
int b;
{
	register int c = ad_toikbd[b & 0x7F];

	if (c == 0)
		return;
	ikbd_cons(c == I_CAPS ? c : c | (b & 0x80));
}

/* The IKBD scancode of an ADB code, 0 none. */
int
ata_vnative(c)
int c;
{
	return ad_toikbd[c & 0x7F];
}

/* --------------------------------------------------------- Videl state */

#define V8(o)	(*(VOL unsigned char *)(0xFFFF0000 | (o)))
#define V16(o)	(*(VOL unsigned short *)(0xFFFF0000 | (o)))
#define V32(o)	(*(VOL unsigned long *)(0xFFFF0000 | (o)))
#define R16(d, o)	((d)->v_reg[(o) - 0x8200] << 8 | (d)->v_reg[(o) - 0x8200 + 1])

/* the timing registers, $FF8282-$FF828C and $FF82A2-$FF82AC */
static unsigned short ad_vtime[] = { 0x8282, 0x8284, 0x8286, 0x8288, 0x828A, 0x828C,
	0x82A2, 0x82A4, 0x82A6, 0x82A8, 0x82AA, 0x82AC, 0 };
/* the rest that make a mode */
static unsigned short ad_vword[] = { 0x820E, 0x8210, 0x8266, 0x82C0, 0x82C2, 0 };

/* Waits for the vertical counter to restart, at most about a second. */
static void
ad_vsync()
{
	register unsigned short v, n;
	register long i;

	v = V16(0x82A0);
	for (i = 0; i < 500000; i++) {
		n = V16(0x82A0);
		if (n < v)
			break;
		v = n;
	}
}

/* the Videl's mode and palette into d */
void
ata_vsave(d)
register struct dsvid *d;
{
	register int i;

	for (i = 0; i < 0xC4; i += 2)
		d->v_reg[i] = V8(0x8200 + i), d->v_reg[i + 1] = V8(0x8201 + i);
	for (i = 0; i < 256; i++)
		d->v_pal[i] = V32(0x9800 + 4 * i);
}

/*
 * d into the Videl, in a frame's blank: timings, then the shift mode
 * as it was set, with the other one cleared first, then what the
 * shift write resets.
 */
void
ata_vload(d)
register struct dsvid *d;
{
	register int i, x;

	ad_vsync();
	x = DS_SPL(7);
	V8(0x8201) = d->v_reg[0x01];
	V8(0x8203) = d->v_reg[0x03];
	V8(0x820D) = d->v_reg[0x0D];
	V8(0x820A) = d->v_reg[0x0A];
	for (i = 0; ad_vtime[i]; i++)
		V16(ad_vtime[i]) = R16(d, ad_vtime[i]);
	if (d->v_st) {
		V16(0x8266) = 0;
		V8(0x8260) = d->v_reg[0x60];
	} else {
		V8(0x8260) = 0;
		V16(0x8266) = R16(d, 0x8266);
	}
	V8(0x8265) = d->v_reg[0x65];
	for (i = 0; ad_vword[i]; i++)
		if (ad_vword[i] != 0x8266)
			V16(ad_vword[i]) = R16(d, ad_vword[i]);
	for (i = 0; i < 16; i++)
		V16(0x8240 + 2 * i) = R16(d, 0x8240 + 2 * i);
	for (i = 0; i < 256; i++)
		V32(0x9800 + 4 * i) = d->v_pal[i];
	DS_SPLX(x);
	if (!d->v_st && (R16(d, 0x8266) & 0x400)) {
		/* 2 colours, toggled across a frame or the picture can distort */
		ad_vsync();
		V16(0x8266) = 0;
		ad_vsync();
		V16(0x8266) = 0x400;
	}
}

/* a byte of the Videl's registers */
int
ata_vget(a)
unsigned long a;
{
	return V8(a & 0xFFFF);
}

/* one guest write, as it was made */
void
ata_vput(a, sz, v)
unsigned long a, v;
int sz;
{
	a &= 0xFFFF;
	if (sz == 4 && !(a & 3))
		V32(a) = v;
	else if (sz == 2 && !(a & 1))
		V16(a) = v;
	else
		while (sz-- > 0)
			V8(a++) = v >> 8 * sz;
}

/* a byte of the sound, codec and matrix registers */
int
ata_sget(a)
unsigned long a;
{
	return V8(a & 0xFFFF);
}

void
ata_sput(a, v)
unsigned long a, v;
{
	V8(a & 0xFFFF) = v;
}

/* MFP input 7, the DMA sound end: on or off, on the given edge; any pending one dropped */
void
ata_sirq(on, rise)
int on, rise;
{
	register int x = DS_SPL(7);

	V8(0xFA07) &= 0x7F;
	V8(0xFA13) &= 0x7F;
	if (on)
		V8(0xFA03) = (V8(0xFA03) & 0x7F) | (rise ? 0x80 : 0);
	V8(0xFA0B) = 0x7F;
	if (on) {
		V8(0xFA07) |= 0x80;
		V8(0xFA13) |= 0x80;
	}
	DS_SPLX(x);
}

/* ------------------------------------------------------ session modes */

/*
 * Modes a Unix session may set on a VGA monitor: id, size, depth, the
 * Falcon mode word and its timings ($FF8282-$FF828C, $FF82A2-$FF82AC).
 */
static struct advm {
	unsigned short	m_id, m_w, m_h, m_d, m_vm;
	unsigned short	m_t[12];
} ad_vm[] = {
	{ 0x81, 640, 480, 1, 0x18, { 0xC6, 0x8D, 0x15, 0x273, 0x50, 0x96,
	    0x419, 0x3FF, 0x3F, 0x3F, 0x3FF, 0x415 } },
	{ 0x82, 640, 480, 8, 0x1B, { 0xC6, 0x8D, 0x15, 0x2AB, 0x84, 0x96,
	    0x419, 0x3FF, 0x3F, 0x3F, 0x3FF, 0x415 } },
	{ 0x83, 320, 480, 16, 0x14, { 0xC6, 0x8D, 0x15, 0x2AC, 0x91, 0x96,
	    0x419, 0x3FF, 0x3F, 0x3F, 0x3FF, 0x415 } },
	{ 0x84, 320, 240, 16, 0x114, { 0xC6, 0x8D, 0x15, 0x2AC, 0x91, 0x96,
	    0x419, 0x3FF, 0x3F, 0x3F, 0x3FF, 0x415 } },
};
#define AD_NVM	(sizeof ad_vm / sizeof ad_vm[0])
#define PUT16(d, o, v)	((d)->v_reg[(o) - 0x8200] = (v) >> 8, (d)->v_reg[(o) - 0x8200 + 1] = (v) & 0xFF)

#ifdef ATA060
extern int ata_svfb;
#endif

/* the number of session modes: none unless the monitor is VGA */
int
ata_nvmode()
{
#ifdef ATA060
	if (ata_svfb)
		return 0;	/* Videl modes would end the SuperVidel's */
#endif
	return (V8(0x8006) >> 6) == 2 ? AD_NVM : 0;
}

/* index of mode id, or -1 */
int
ata_vmfind(id)
unsigned long id;
{
	register int i;

	for (i = 0; i < ata_nvmode(); i++)
		if (ad_vm[i].m_id == id)
			return i;
	return -1;
}

/* mode n's geometry into fi */
void
ata_vminfo(n, fi)
int n;
register struct fbinfo *fi;
{
	register struct advm *m = &ad_vm[n];

	fi->fi_width = m->m_w;
	fi->fi_height = m->m_h;
	fi->fi_depth = m->m_d;
	fi->fi_rowbytes = m->m_w * m->m_d / 8;
	fi->fi_layout = m->m_d == 1 || m->m_d > 8 ? FBL_PACKED : FBL_IPLAN2;
	fi->fi_planebytes = m->m_d == 1 || m->m_d > 8 ? 0 : 2;
	fi->fi_visual = m->m_d == 1 ? FBV_MONO : m->m_d <= 8 ? FBV_PSEUDO : FBV_TRUE;
	fi->fi_rmask = m->m_d == 16 ? 0xF800 : 0;
	fi->fi_gmask = m->m_d == 16 ? 0x07E0 : 0;
	fi->fi_bmask = m->m_d == 16 ? 0x001F : 0;
	fi->fi_cmapsize = m->m_d <= 8 ? 1 << m->m_d : 0;
	fi->fi_offset = 0;
	fi->fi_mode = m->m_id;
	fi->fi_flags = FBF_SETMODE | (m->m_d <= 8 ? FBF_BLANK | FBF_CMAP : 0);
}

/*
 * Mode n into d, showing base: the timings, then the line width,
 * video control and shift mode, as the ROM sets them.
 */
void
ata_vmset(d, n, base)
register struct dsvid *d;
int n;
unsigned long base;
{
	register struct advm *m = &ad_vm[n];
	register int i, sh;

	for (i = 0; i < 12; i++)
		PUT16(d, ad_vtime[i], m->m_t[i]);
	d->v_reg[0x01] = base >> 16;
	d->v_reg[0x03] = base >> 8;
	d->v_reg[0x0D] = base;
	d->v_reg[0x0A] = 0;
	d->v_reg[0x60] = 0;
	d->v_reg[0x65] = 0;
	PUT16(d, 0x820E, 0);
	PUT16(d, 0x8210, ((m->m_vm & 8) ? 40 : 20) << (m->m_vm & 7));
	PUT16(d, 0x82C2, ((m->m_vm & 8) ? 8 : 4) | ((m->m_vm & 0x100) ? 1 : 0));
	PUT16(d, 0x82C0, 0x186);
	sh = m->m_d == 1 ? 0x400 : m->m_d == 8 ? 0x10 : m->m_d == 16 ? 0x100 : 0;
	PUT16(d, 0x8266, sh);
	d->v_st = 0;
}

/* ------------------------------------------------------------ blitter */

/* Waits for the blitter to go idle; 0, or -1 if it does not. */
static int
ab_idle()
{
	register long i;

	for (i = 0; i < 1000000; i++)
		if (!(V8(0x8A3C) & 0x80))
			return 0;
	V8(0x8A3C) = 0;
	return -1;
}

/* the blitter's registers, $FF8A00-$FF8A3D, into b once it is idle */
void
ata_bsave(b)
register unsigned char *b;
{
	register int i;

	(void)ab_idle();
	for (i = 0; i < 0x3E; i++)
		b[i] = V8(0x8A00 + i);
}

/* b into the blitter, not started */
void
ata_bload(b)
register unsigned char *b;
{
	register int i;

	(void)ab_idle();
	for (i = 0; i < 0x3C; i++)
		V8(0x8A00 + i) = b[i];
	V8(0x8A3D) = b[0x3D];
	V8(0x8A3C) = b[0x3C] & 0x7F;
}

/*
 * One line, or part of one, in HOG mode: the CPU waits while the
 * blitter has the bus.  0, or -1 if it did not finish.
 */
#ifdef ATA060
extern void ata_dmasync();
#endif

int
ata_brun(r)
register struct dsbrun *r;
{
	register int i;

	if (ab_idle())
		return -1;
	for (i = 0; i < 16; i++)
		V16(0x8A00 + 2 * i) = r->r_ht[i];
	V16(0x8A20) = r->r_sxi;
	V16(0x8A22) = r->r_syi;
	V32(0x8A24) = r->r_sa;
	V16(0x8A28) = r->r_em[0];
	V16(0x8A2A) = r->r_em[1];
	V16(0x8A2C) = r->r_em[2];
	V16(0x8A2E) = r->r_dxi;
	V16(0x8A30) = r->r_dyi;
	V32(0x8A32) = r->r_da;
	V16(0x8A36) = r->r_xn;
	V16(0x8A38) = 1;
	V8(0x8A3A) = r->r_hop;
	V8(0x8A3B) = r->r_op;
	V8(0x8A3D) = r->r_skew;
	V8(0x8A3C) = 0xC0 | r->r_ctl;
#ifdef ATA060
	i = ab_idle();
	ata_dmasync();
	return i;
#else
	return ab_idle();
#endif
}

/* The IKBD keyboard has no LEDs. */
/* ARGSUSED */
void
adbkbd_setleds(m)
int m;
{
}
