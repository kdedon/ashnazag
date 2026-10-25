/*
 * tospatch.c -- EmuTOS's hardware routines, found by structure in the
 * loaded ROM and patched to host calls.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include "hostcall.h"
#include "tosio.h"

#define	CART	0xfa0000
#define	CARTSZ	0x20000

enum {
	HC_PROBE, HC_GETTIME, HC_XBTIMER, HC_VBL, HC_VBLPAL, HC_VBLBASE,
	HC_PHYSBASE, HC_GETREZ, HC_SETSCREEN, HC_SETCOLOR, HC_ESETSHIFT, HC_EGETSHIFT,
	HC_ESETBANK, HC_ESETCOLOR, HC_ESETPAL, HC_EGETPAL, HC_ESETGRAY, HC_ESETSMEAR,
	HC_GIACCESS, HC_ONGIBIT, HC_OFFGIBIT, HC_DOSOUND, HC_NVMACCESS, HC_JDISINT, HC_JENABINT,
	HC_SNDIRQ
};

/*
 * XBIOS calls replaced outright; in: TOS calls the routine itself too,
 * so its entry jumps to the host call unless another XBIOS shares it.
 */
static struct { int x, hc, in; } xbhost[] = {
	{ 23, HC_GETTIME, 1 }, { 26, HC_JDISINT, 1 }, { 27, HC_JENABINT, 1 },
	{ 28, HC_GIACCESS, 1 }, { 29, HC_ONGIBIT, 1 }, { 30, HC_OFFGIBIT, 1 },
	{ 31, HC_XBTIMER, 1 }, { 32, HC_DOSOUND, 1 }, { 46, HC_NVMACCESS, 0 },
};
#define	NXH	(sizeof xbhost / sizeof xbhost[0])

char *tos_nvram;		/* the NVRAM's file, 0 for none */
static unsigned char nvm[50];	/* 48 bytes, then their checksum */
static int nvmread;

/* the TT shifter's XBIOS calls: number, host call, and argument bytes
 * when the original runs first */
static struct { int x, hc, args; } xbhook[] = {
	{ 2, HC_PHYSBASE }, { 4, HC_GETREZ }, { 5, HC_SETSCREEN, 12 }, { 7, HC_SETCOLOR },
	{ 80, HC_ESETSHIFT, 2 }, { 81, HC_EGETSHIFT }, { 82, HC_ESETBANK }, { 83, HC_ESETCOLOR },
	{ 84, HC_ESETPAL }, { 85, HC_EGETPAL }, { 86, HC_ESETGRAY }, { 87, HC_ESETSMEAR },
};
#define	NXB	(sizeof xbhook / sizeof xbhook[0])

/* copy the arguments, call the original, then the host call */
static unsigned short wrap[NXB][15];

static volatile struct tosvid *vd = (struct tosvid *)TOSVID;

/* the TT palette at reset; the ROM's own table when it has one */
static unsigned short dflt[256] = {
	0xfff, 0xf00, 0x0f0, 0xff0, 0x00f, 0xf0f, 0x0ff, 0xaaa,
	0x666, 0xf99, 0x9f9, 0xff9, 0x99f, 0xf9f, 0x9ff, 0x000
};

static unsigned long rbase, rsize, ramsz;
static int tfd;
static unsigned long palend, baseend;	/* the VBL's palette and base writes end here */
static unsigned long sndtab;		/* Dosound's sequence pointer, in TOS's variables */
extern int tos_bell;

/* MFP channel of each timer: its vector is $100 + 4 * channel */
static int tch[4] = { 13, 8, 5, 4 };

/* the chips this machine has, in the I/O pages */
static struct { unsigned long lo, hi; } chips[] = {
	{ 0xff8000, 0xff8100 },		/* memory controller */
	{ 0xff8200, 0xff8266 },		/* shifter */
	{ 0xff8400, 0xff8600 },		/* TT palette */
	{ 0xff8600, 0xff8610 },		/* FDC/ACSI DMA */
	{ 0xff8800, 0xff8900 },		/* YM2149 */
	{ 0xff8900, 0xff8940 },		/* DMA sound */
	{ 0xff8e00, 0xff8e10 },		/* SCU */
	{ 0xff9200, 0xff9202 },		/* DIP switches */
	{ 0xfffa00, 0xfffa30 },		/* MFP */
	{ 0xfffa80, 0xfffab0 },		/* TT MFP */
	{ 0xfffc00, 0xfffc08 },		/* ACIAs */
};

static unsigned
w16(p)
	unsigned char *p;
{
	return p[0] << 8 | p[1];
}

static unsigned long
g32(p)
	unsigned char *p;
{
	return (unsigned long)w16(p) << 16 | w16(p + 2);
}

static void
put32(a, v)
	unsigned long a, v;
{
	unsigned char *p = (unsigned char *)a;

	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void
jmpto(a, n)
	unsigned long a;
	int n;
{
	unsigned char *p = (unsigned char *)a;
	unsigned long t = HC_STUB(n);

	p[0] = 0x4e;
	p[1] = 0xf9;
	p[2] = t >> 24;
	p[3] = t >> 16;
	p[4] = t >> 8;
	p[5] = t;
}

static int
present(a)
	unsigned long a;
{
	unsigned long o;
	int i;

	if (a < ramsz || a - rbase < rsize || a - CART < CARTSZ)
		return 1;
	if ((a & 0xff000000) == 0xff000000)
		o = a & 0xffffff;
	else if ((a & 0xfff00000) == 0xf00000)
		o = a;
	else
		return 0;
	for (i = 0; i < sizeof chips / sizeof chips[0]; i++)
		if (o >= chips[i].lo && o < chips[i].hi)
			return 1;
	return 0;
}

/* ST palette entries: the low bit of each component is its top bit */
static unsigned
st2tt(v)
	unsigned v;
{
	return (v & 0x777) << 1 | (v & 0x888) >> 3;
}

static unsigned
tt2st(v)
	unsigned v;
{
	return (v & 0xeee) >> 1 | (v & 0x111) << 3;
}

/* the screen TOS set up at boot, before its first VBL or XBIOS call */
static void
vidinit()
{
	if (vd->vd_seq)
		return;
	vd->vd_base = g32((unsigned char *)0x44e);		/* _v_bas_ad */
	vd->vd_mode = (*(unsigned char *)0x44c & 7) << 8;	/* sshiftmod */
	memcpy((char *)vd->vd_pal, (char *)dflt, sizeof dflt);
	vd->vd_seq = 1;
}

/* the ST palette is the current 16-colour bank of the TT one */
static volatile unsigned short *
stpal()
{
	return vd->vd_pal + (vd->vd_mode & 15) * 16;
}

/* XBIOS: the TT shifter, its ST view and the screen base */
static void
xbvideo(r, n)
	unsigned long *r;
	int n;
{
	unsigned char *p = (unsigned char *)&r[17];
	volatile unsigned short *pal;
	unsigned long a;
	int i, c, k;

	vidinit();
	switch (n) {
	case HC_PHYSBASE:
		r[0] = vd->vd_base;
		return;
	case HC_GETREZ:
		r[0] = vd->vd_mode >> 8 & 7;
		return;
	case HC_SETSCREEN:	/* after the original: (log, phys, rez, mode) */
		if ((long)g32(p + 4) > 0)
			vd->vd_base = g32(p + 4);
		k = (short)w16(p + 8);
		if (k >= 0 && k <= 7 && *(unsigned char *)0x44c == k)
			vd->vd_mode = (vd->vd_mode & 0xf8ff) | k << 8;
		break;
	case HC_SETCOLOR:
		pal = stpal() + (w16(p) & 15);
		r[0] = tt2st(*pal);
		if ((short)w16(p + 2) >= 0)
			*pal = st2tt(w16(p + 2) & 0xfff);
		break;
	case HC_ESETSHIFT:	/* after the original */
		r[0] = vd->vd_mode;
		vd->vd_mode = w16(p) & 0x970f;
		break;
	case HC_EGETSHIFT:
		r[0] = vd->vd_mode;
		return;
	case HC_ESETBANK:
		r[0] = vd->vd_mode & 15;
		if ((short)w16(p) >= 0)
			vd->vd_mode = (vd->vd_mode & ~15) | (w16(p) & 15);
		break;
	case HC_ESETCOLOR:
		pal = vd->vd_pal + (w16(p) & 255);
		r[0] = *pal;
		if ((short)w16(p + 2) >= 0)
			*pal = w16(p + 2) & 0xfff;
		break;
	case HC_ESETPAL:	/* (index, count, from) */
	case HC_EGETPAL:
		i = w16(p) & 255;
		c = (short)w16(p + 2);
		if (i + c > 256)
			c = 256 - i;
		a = g32(p + 4);
		if (c > 0 && a < ramsz && a + 2 * c <= ramsz)
			for (k = 0; k < c; k++, a += 2)
				if (n == HC_ESETPAL)
					vd->vd_pal[i + k] = w16((unsigned char *)a) & 0xfff;
				else {
					((unsigned char *)a)[0] = vd->vd_pal[i + k] >> 8;
					((unsigned char *)a)[1] = vd->vd_pal[i + k];
				}
		r[0] = 0;
		if (n == HC_EGETPAL)
			return;
		break;
	case HC_ESETGRAY:	/* bit 12, smear bit 15 */
	case HC_ESETSMEAR:
		k = n == HC_ESETGRAY ? 0x1000 : 0x8000;
		r[0] = (vd->vd_mode & k) != 0;
		if ((short)w16(p) > 0)
			vd->vd_mode |= k;
		else if (w16(p) == 0)
			vd->vd_mode &= ~k;
		break;
	}
	vd->vd_seq++;
}

/* the YM2149's registers for the pump, which is rung after a change */
static void
ym(r, n)
	unsigned long *r;
	int n;
{
	volatile struct tosym *y = (struct tosym *)TOSYM;
	unsigned char *p = (unsigned char *)&r[17];
	int k = p[3] & 15;

	switch (n) {
	case HC_GIACCESS:	/* (data, register | $80 to write) */
		r[0] = y->ym_reg[k];
		if (!(p[3] & 0x80))
			return;
		r[0] = y->ym_reg[k] = p[1];
		if (k == 13)
			y->ym_egen++;
		break;
	case HC_ONGIBIT:
		y->ym_reg[14] |= p[1];
		return;
	case HC_OFFGIBIT:
		y->ym_reg[14] &= p[1];
		return;
	case HC_DOSOUND:
		r[0] = y->ym_seq;
		if ((long)g32(p) < 0)
			return;
		y->ym_table = y->ym_seq = g32(p);
		y->ym_tgen++;
		break;
	case HC_SNDIRQ:		/* a sequence TOS started itself (bell, key click) */
		if (g32((unsigned char *)sndtab) == 0)
			return;
		y->ym_table = y->ym_seq = g32((unsigned char *)sndtab);
		y->ym_tgen++;
		put32(sndtab, 0L);
		break;
	}
	if (tos_bell >= 0)
		write(tos_bell, "y", 1);
}

static unsigned
nvmsum()
{
	unsigned s = 0;
	int i;

	for (i = 0; i < 48; i++)
		s += nvm[i];
	s &= 255;
	return (~s & 255) << 8 | s;
}

/* NVMaccess(op, start, count, buffer) on the environment's file */
static long
nvmaccess(p)
	unsigned char *p;
{
	int op = (short)w16(p), st = (short)w16(p + 2), n = (short)w16(p + 4), fd, i;
	unsigned long a = g32(p + 6);
	struct stat sb;
	char tmp[1100];

	if (!nvmread) {
		nvmread = 1;
		if (tos_nvram && lstat(tos_nvram, &sb) == 0 && (sb.st_mode & S_IFMT) != S_IFREG)
			tos_nvram = 0;		/* not ours to read or replace */
		if (tos_nvram && (fd = open(tos_nvram, O_RDONLY)) >= 0) {
			read(fd, (char *)nvm, sizeof nvm);
			close(fd);
		}
	}
	if (op == 2)
		memset((char *)nvm, 0, 48);
	else if (a == 0 || a >= ramsz || st < 0 || n < 1 || st + n > 48 || a + n > ramsz)
		return -5;
	else if (op == 0) {
		if (nvmsum() != w16(nvm + 48))
			return -12;
		memcpy((char *)a, (char *)nvm + st, n);
		return 0;
	} else if (op == 1)
		memcpy((char *)nvm + st, (char *)a, n);
	else
		return -5;
	i = nvmsum();
	nvm[48] = i >> 8;
	nvm[49] = i;
	if (!tos_nvram || strlen(tos_nvram) > 1000)
		return 0;
	sprintf(tmp, "%s.new", tos_nvram);
	unlink(tmp);
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0)
		return 0;
	i = write(fd, (char *)nvm, sizeof nvm) == sizeof nvm;
	if (close(fd) < 0 || !i || rename(tmp, tos_nvram) < 0)
		unlink(tmp);
	return 0;
}

/* GEMDOS time and date, local */
static unsigned long
dostime()
{
	time_t now = time((time_t *)0);
	struct tm *t = localtime(&now);

	return (unsigned long)(t->tm_year - 80) << 25 | (unsigned long)(t->tm_mon + 1) << 21 |
	    (unsigned long)t->tm_mday << 16 | t->tm_hour << 11 | t->tm_min << 5 | t->tm_sec >> 1;
}

void
hostcall(r)
	unsigned long *r;
{
	unsigned char *p;
	unsigned long a;
	int n, i;

	switch (n = HC_CALL(r)) {
	case HC_PROBE:		/* from check_read_byte's entry: r[16] its return, r[17] the address */
		r[0] = present(r[17]);
		break;
	case HC_GETTIME:
		r[0] = dostime();
		break;
	case HC_XBTIMER:	/* (timer, control, data: words; vector: long) */
		p = (unsigned char *)&r[17];
		if ((n = w16(p)) > 3)
			break;
		if (g32(p + 6) != 0xffffffff) {
			*(unsigned long *)(0x100 + 4 * tch[n]) = g32(p + 6);
			ioctl(tfd, TOSIOC_INTEN, (unsigned long)tch[n] << 8 | 1);
		}
		ioctl(tfd, TOSIOC_TIMER, (unsigned long)n << 16 | (w16(p + 2) & 0xff) << 8 |
		    (w16(p + 4) & 0xff));
		break;
	case HC_JDISINT:
	case HC_JENABINT:
		ioctl(tfd, TOSIOC_INTEN, (unsigned long)(w16((unsigned char *)&r[17]) & 15) << 8 |
		    (n == HC_JENABINT));
		break;
	case HC_VBL:		/* in place of the VBL's monitor check: no monitor change */
		vidinit();
		break;
	case HC_VBLPAL:		/* the VBL's Setpalette: from d0, unless the Falcon flag is set */
		vidinit();
		r[16] = palend;
		if ((a = r[0]) & 1 || a >= ramsz || a + 32 > ramsz)
			break;
		for (i = 0; i < 16; i++)
			stpal()[i] = st2tt(w16((unsigned char *)a + 2 * i) & 0xfff);
		vd->vd_seq++;
		break;
	case HC_VBLBASE:	/* the VBL's screenpt: _v_bas_ad, then lsr.w #8,d0 */
		vidinit();
		*(unsigned long *)0x44e = r[0];
		vd->vd_base = r[0];
		vd->vd_seq++;
		r[16] = baseend;
		break;
	case HC_GIACCESS:
	case HC_ONGIBIT:
	case HC_OFFGIBIT:
	case HC_DOSOUND:
	case HC_SNDIRQ:
		ym(r, n);
		break;
	case HC_NVMACCESS:
		r[0] = nvmaccess((unsigned char *)&r[17]);
		break;
	default:
		xbvideo(r, n);
		break;
	}
}

/*
 * check_read_byte: saves sp in d1 and vector 2 in a1, points vector 2 at
 * its own tail and clears d0 before touching the address.
 */
static unsigned char crb[] = {
	0x22, 0x0f, 0x22, 0x78, 0x00, 0x08, 0x41, 0xfa, 0, 0, 0x21, 0xc8, 0x00, 0x08, 0x70, 0x00
};

static unsigned long
findcrb()
{
	unsigned char *p = (unsigned char *)rbase, *e = p + rsize - sizeof crb;

	for (; p < e; p += 2)
		if (memcmp(p, crb, 8) == 0 && memcmp(p + 10, crb + 10, 6) == 0)
			return (unsigned long)p;
	return 0;
}

/*
 * timeout_gpip, the FDC and ACSI completion wait: a loop reading MFP
 * GPIP bit 5 and branching back to its 200 Hz clock compare.  It times
 * out at once: no drive answers.
 */
static unsigned long
findgpip()
{
	unsigned char *p = (unsigned char *)rbase, *e = p + rsize - 10, *t;

	for (; p < e; p += 2)
		if (w16(p) == 0x1038 && w16(p + 2) == 0xfa01 && w16(p + 4) == 0x0800 &&
		    w16(p + 6) == 5 && p[8] == 0x66 && (signed char)p[9] < 0) {
			t = p + 10 + (signed char)p[9];
			if (t >= (unsigned char *)rbase && (w16(t) & 0xf1ff) == 0x2039 && g32(t + 2) == 0x4ba)
				return (unsigned long)p;
		}
	return 0;
}

/*
 * The MFP reset: `movea.w #$fa01,a0`, a loop zeroing every odd register
 * up to $FA2D.  Skipped, so GPIP and TSR keep their constants.
 */
static unsigned char *
findmfpreset()
{
	unsigned char *p = (unsigned char *)rbase, *e = p + rsize - 16;
	int i;

	for (; p < e; p += 2)
		if (w16(p) == 0x307c && w16(p + 2) == 0xfa01)
			for (i = 4; i <= 10; i += 2)
				if (w16(p + i) == 0xb0fc && w16(p + i + 2) == 0xfa2d &&
				    p[i + 4] == 0x63)
					return p;
	return 0;
}

/*
 * The boot's Videl probe: `lea handler(pc),a0; move.l a0,$8.w; tst.b
 * $ffff8282.w`.  The I/O page answers it, so it goes to its bus error
 * handler instead: no Videl, no Falcon memory controller.
 */
static unsigned char *
findvidel()
{
	unsigned char *p = (unsigned char *)rbase, *e = p + rsize - 12;

	for (; p < e; p += 2)
		if (w16(p) == 0x41fa && w16(p + 4) == 0x21c8 && w16(p + 6) == 8 &&
		    w16(p + 8) == 0x4a38 && w16(p + 10) == 0x8282)
			return p + 8;
	return 0;
}

/*
 * Microwire waits: `move.w $ffff8922.w,d0; bne.s` back to itself, until
 * the mixer's shift register empties.  It is empty at once.
 */
static void
microwire()
{
	unsigned char *p = (unsigned char *)rbase, *e = p + rsize - 6;

	for (; p < e; p += 2)
		if (w16(p) == 0x3038 && w16(p + 2) == 0x8922 && w16(p + 4) == 0x66fa) {
			p[0] = 0x70;	/* moveq #0,d0; nop; nop */
			p[1] = 0;
			p[2] = p[4] = 0x4e;
			p[3] = p[5] = 0x71;
		}
}

/*
 * The BIOS and XBIOS trap handlers: `move.w count,d1; lea table,a0`, the
 * BIOS one then a short branch into their shared tail.
 */
static unsigned long
xbios(np)
	int *np;
{
	unsigned char *p = (unsigned char *)rbase, *e = p + rsize - 26;
	unsigned long t;

	for (; p < e; p += 2)
		if (w16(p) == 0x3239 && w16(p + 6) == 0x41f9 && (w16(p + 12) & 0xff00) == 0x6000 &&
		    w16(p + 14) == 0x3239 && w16(p + 20) == 0x41f9) {
			t = g32(p + 22);
			if (g32(p + 16) - rbase > rsize - 2 || t - rbase >= rsize)
				continue;
			if ((*np = w16((unsigned char *)g32(p + 16))) > 256)
				*np = 256;
			if (t + 4 * *np > rbase + rsize)
				continue;
			return t;
		}
	return 0;
}

/*
 * The VBL handler: `addq.l #1,_vbclock.w; jsr detect_monitor_change`, its
 * palette load `lea $ffff8240.w,a1; moveq #7,d1` and its screenpt store
 * `move.l d0,_v_bas_ad.w; lsr.w #8,d0`.  0: one is missing.
 */
static int
vblvideo()
{
	unsigned char *p = (unsigned char *)rbase, *e = p + rsize - 6, *v = 0, *pl = 0, *b = 0;
	unsigned long t;

	for (; p < e; p += 2)
		if (!v && w16(p) == 0x52b8 && w16(p + 2) == 0x462 && w16(p + 4) == 0x4eb9)
			v = p + 6;
		else if (!pl && w16(p) == 0x43f8 && w16(p + 2) == 0x8240 && w16(p + 4) == 0x7207)
			pl = p;
		else if (!b && w16(p) == 0x21c0 && w16(p + 2) == 0x44e && w16(p + 4) == 0xe048)
			b = p;
	if (!v || !pl || !b)
		return 0;
	/* past the palette copy `move.l (a0)+,(a1)+; dbra d1` and the shifter base writes */
	for (p = pl + 6; p < pl + 40 && p < e; p += 2)
		if (w16(p) == 0x22d8 && w16(p + 2) == 0x51c9 && w16(p + 4) == 0xfffc) {
			palend = (unsigned long)p + 6;
			break;
		}
	for (p = b + 6; p < b + 64 && p < e; p += 2)
		if (w16(p) == 0x11c0 && w16(p + 2) == 0x8201)
			baseend = (unsigned long)p + 4;
		else if (w16(p) == 0x11c0 && w16(p + 2) == 0x820d) {
			baseend = (unsigned long)p + 4;
			break;
		}
	if (!palend || !baseend)
		return 0;
	t = HC_STUB(HC_VBL);
	v[0] = t >> 24;
	v[1] = t >> 16;
	v[2] = t >> 8;
	v[3] = t;
	jmpto((unsigned long)pl, HC_VBLPAL);
	jmpto((unsigned long)b, HC_VBLBASE);
	pl[1] = b[1] = 0xb9;	/* jsr */
	/* the ROM's reset palette: the first 16 are the ST colours */
	for (p = (unsigned char *)rbase; p < e - 512; p += 2)
		if (w16(p) == dflt[0] && w16(p + 2) == dflt[1] && w16(p + 14) == dflt[7] &&
		    w16(p + 30) == dflt[15] && w16(p + 32) == 0xfff && w16(p + 34) == 0xeee) {
			for (t = 0; t < 256; t++)
				dflt[t] = w16(p + 2 * t) & 0xfff;
			break;
		}
	return 1;
}

/*
 * Timer C's in-service clear at the end of its handler: `move.b
 * #$df,$fffffa11.w` or `bclr #5,$fffffa11.w`, then rte.  It becomes
 * `clr.l` of pv_tcsvc, the in-service state the module keeps.
 */
int	tos_tceoi;	/* patched */

static void
timerc()
{
	unsigned char *p = (unsigned char *)rbase, *e = p + rsize - 8;
	unsigned long a = TOSPV + (int)&((struct tospv *)0)->pv_tcsvc;

	for (; p < e; p += 2)
		if (((w16(p) == 0x11fc && w16(p + 2) == 0xdf) ||
		    (w16(p) == 0x08b8 && w16(p + 2) == 5)) && w16(p + 4) == 0xfa11 &&
		    w16(p + 6) == 0x4e73) {
			p[0] = 0x42;
			p[1] = 0xb9;
			p[2] = a >> 24;
			p[3] = a >> 16;
			p[4] = a >> 8;
			p[5] = a;
			tos_tceoi = 1;
		}
}

/*
 * The Timer C handler's Dosound step: `move.l d2,-(sp); movea.l
 * sndtable,a0; cmpa.w #0,a0; beq`, then YM register writes.  It hands a
 * sequence TOS set without Dosound to the sound pump instead.
 */
static void
sndirq()
{
	unsigned char *p = (unsigned char *)rbase, *e = p + rsize - 64, *q;

	for (; p < e; p += 2)
		if (w16(p) == 0x2f02 && w16(p + 2) == 0x2079 && w16(p + 8) == 0xb0fc &&
		    w16(p + 10) == 0 && p[12] == 0x67 && g32(p + 4) < ramsz)
			for (q = p + 14; q < p + 64; q += 2)
				if (w16(q) == 0x11c0 && w16(q + 2) == 0x8800) {
					sndtab = g32(p + 4);
					jmpto((unsigned long)p, HC_SNDIRQ);
					return;
				}
}

/* XBIOS n through the TT shifter host calls; the original first for
 * those with side effects */
static void
xbvid(t, n)
	unsigned long t;
	int n;
{
	unsigned char *e = (unsigned char *)t + 4 * xbhook[n].x;
	unsigned short *w = wrap[n];
	unsigned long a = g32(e), s = HC_STUB(xbhook[n].hc);
	int k;

	if (xbhook[n].args == 0)
		a = s;
	else {
		for (k = 0; k < xbhook[n].args / 4; k++) {
			*w++ = 0x2f2f;		/* move.l 12(sp),-(sp) */
			*w++ = 12;
		}
		if (xbhook[n].args & 2) {
			*w++ = 0x3f2f;		/* move.w 4(sp),-(sp) */
			*w++ = 4;
		}
		*w++ = 0x4eb9;			/* jsr original */
		*w++ = a >> 16;
		*w++ = a;
		*w++ = 0x4fef;			/* lea args(sp),sp */
		*w++ = xbhook[n].args;
		*w++ = 0x4ef9;			/* jmp host call */
		*w++ = s >> 16;
		*w = s;
		a = (unsigned long)wrap[n];
	}
	e[0] = a >> 24;
	e[1] = a >> 16;
	e[2] = a >> 8;
	e[3] = a;
}

static unsigned long xbtab[256];	/* the XBIOS table as the ROM has it */

/* XBIOS x to host call hc in its table; with in, at the routine's entry too */
static int
xbhook1(t, nx, x, hc, in)
	unsigned long t;
	int nx, x, hc, in;
{
	unsigned long a = xbtab[x];
	int i;

	if (x >= nx || a - rbase >= rsize)
		return -1;
	put32(t + 4 * x, HC_STUB(hc));
	for (i = 0; i < nx; i++)
		if (i != x && xbtab[i] == a)
			in = 0;
	if (in)
		jmpto(a, hc);
	return 0;
}

/* -1: a routine was not found, named on stderr */
int
tos_patch(base, size, ram, fd)
	unsigned long base, size, ram;
	int fd;
{
	unsigned long a, t;
	unsigned char *p;
	int n, nx;

	rbase = base;
	rsize = size;
	ramsz = ram;
	tfd = fd;
	if ((a = findcrb()) == 0) {
		fprintf(stderr, "starttos: no check_read_byte in the ROM\n");
		return -1;
	}
	if ((t = xbios(&nx)) == 0 || nx < 32) {
		fprintf(stderr, "starttos: no XBIOS table in the ROM\n");
		return -1;
	}
	jmpto(a, HC_PROBE);
	for (n = 0; n < nx; n++)
		xbtab[n] = g32((unsigned char *)t + 4 * n);
	if ((a = findgpip()) == 0) {
		fprintf(stderr, "starttos: no timeout_gpip in the ROM\n");
		return -1;
	}
	p = (unsigned char *)a;
	p[0] = 0x70;		/* moveq #1,d0; rts */
	p[1] = 0x01;
	p[2] = 0x4e;
	p[3] = 0x75;
	if ((p = findmfpreset()) == 0) {
		fprintf(stderr, "starttos: no MFP reset in the ROM\n");
		return -1;
	}
	for (n = 4; w16(p + n) != 0xb0fc; n += 2)
		;
	p[1] = n + 4;		/* bra.s past the loop */
	p[0] = 0x60;
	microwire();
	if ((p = findvidel()) != 0) {
		p[0] = 0x4e;	/* jmp (a0); nop */
		p[1] = 0xd0;
		p[2] = 0x4e;
		p[3] = 0x71;
	}
	timerc();
	sndirq();
	if (!vblvideo()) {
		fprintf(stderr, "starttos: no VBL screen update in the ROM\n");
		return -1;
	}
	for (n = 0; n < NXH; n++)
		if (xbhook1(t, nx, xbhost[n].x, xbhost[n].hc, xbhost[n].in) < 0 &&
		    (xbhost[n].x == 23 || xbhost[n].x == 31)) {
			fprintf(stderr, "starttos: no %s in the ROM\n",
			    xbhost[n].x == 23 ? "Gettime" : "Xbtimer");
			return -1;
		}
	for (n = 0; n < NXB; n++)
		if (xbhook[n].x < nx)
			xbvid(t, n);
	return 0;
}

HC_ENTRY
