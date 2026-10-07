/*
 * amlibs.c -- utility.library, expansion.library and timer.device for
 * amilib.
 *
 * expansion: the host's Zorro boards become ConfigDevs on the private
 * BoardList (ExpansionBase + 60), mapped into kernel space; cd_BoardAddr
 * is the kernel address.  FindConfigDev walks that list, so ConfigDevs
 * a library adds or removes there itself (openpci does) are seen.
 *
 * timer.device: requests complete in BeginIO; TR_ADDREQUEST busy-waits.
 */

#include "amilib.h"

extern void am_iodone();

/* ------------------------------------------------------------ utility */

static unsigned char *
am_tagnext(lp)
	unsigned char **lp;		/* TagItem ** */
{
	unsigned char *t = *lp;

	for (;;) {
		if (t == 0)
			return 0;
		switch (AL(t, 0)) {
		case TAG_DONE:
			*lp = 0;
			return 0;
		case TAG_IGNORE:
			t += 8;
			break;
		case TAG_MORE:
			t = (unsigned char *)AL(t, 4);
			break;
		case TAG_SKIP:
			t += 8 * (AL(t, 4) + 1);
			break;
		default:
			*lp = t + 8;
			return t;
		}
	}
}

unsigned char *
am_tagfind(tag, l)
	unsigned long tag;
	unsigned char *l;
{
	unsigned char *t;

	while ((t = am_tagnext(&l)) != 0)
		if (AL(t, 0) == tag)
			return t;
	return 0;
}

static void
u_findtagitem(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_tagfind(r[D0], (unsigned char *)r[A0]);
}

static void
u_gettagdata(r)
	unsigned long *r;
{
	unsigned char *t = am_tagfind(r[D0], (unsigned char *)r[A0]);

	r[D0] = t ? AL(t, 4) : r[D1];
}

static void
u_nexttagitem(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_tagnext((unsigned char **)r[A0]);
}

static void
u_taginarray(r)
	unsigned long *r;
{
	unsigned long *a = (unsigned long *)r[A0];

	for (; *a != TAG_DONE; a++)
		if (*a == r[D0]) {
			r[D0] = 1;
			return;
		}
	r[D0] = 0;
}

static void
u_packbooltags(r)
	unsigned long *r;
{
	unsigned long flags = r[D0];
	unsigned char *l = (unsigned char *)r[A0], *t, *b;

	while ((t = am_tagnext(&l)) != 0) {
		b = am_tagfind(AL(t, 0), (unsigned char *)r[A1]);
		if (b) {
			if (AL(t, 4))
				flags |= AL(b, 4);
			else
				flags &= ~AL(b, 4);
		}
	}
	r[D0] = flags;
}

static void
u_callhookpkt(r)
	unsigned long *r;
{
	char *hook = (char *)r[A0];
	unsigned long c[16];
	int i;

	for (i = 0; i < 15; i++)
		c[i] = 0;
	c[A0] = (unsigned long)hook;
	c[A1] = r[A1];
	c[A2] = r[A2];
	c[A6] = r[A6];
	r[D0] = am_call(AP(hook, H_ENTRY), c);
}

static void
u_smult32(r)
	unsigned long *r;
{
	r[D0] = (long)r[D0] * (long)r[D1];
}

static void
u_udivmod32(r)
	unsigned long *r;
{
	unsigned long a = r[D0], b = r[D1];

	if (b == 0) {
		r[D0] = r[D1] = 0;
		return;
	}
	r[D0] = a / b;
	r[D1] = a % b;
}

static void
u_sdivmod32(r)
	unsigned long *r;
{
	long a = r[D0], b = r[D1];

	if (b == 0) {
		r[D0] = r[D1] = 0;
		return;
	}
	r[D0] = a / b;
	r[D1] = a % b;
}

/* 32 x 32 -> 64 from 16-bit halves (no 64-bit multiply on the 060) */
static void
umul64(a, b, hi, lo)
	unsigned long a, b, *hi, *lo;
{
	unsigned long al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16;
	unsigned long ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
	unsigned long mid = (ll >> 16) + (lh & 0xffff) + (hl & 0xffff);

	*lo = (ll & 0xffff) | (mid << 16);
	*hi = hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
}

static void
u_umult64(r)
	unsigned long *r;
{
	umul64(r[D0], r[D1], &r[D0], &r[D1]);	/* d0 high, d1 low */
}

static void
u_smult64(r)
	unsigned long *r;
{
	long a = r[D0], b = r[D1];
	int neg = (a < 0) != (b < 0);
	unsigned long hi, lo;

	umul64((unsigned long)(a < 0 ? -a : a), (unsigned long)(b < 0 ? -b : b),
	    &hi, &lo);
	if (neg) {
		hi = ~hi;
		lo = ~lo + 1;
		if (lo == 0)
			hi++;
	}
	r[D0] = hi;
	r[D1] = lo;
}

static void
u_stricmp(r)
	unsigned long *r;
{
	r[D0] = am_stricmp((char *)r[A0], (char *)r[A1], -1L);
}

static void
u_strnicmp(r)
	unsigned long *r;
{
	r[D0] = am_stricmp((char *)r[A0], (char *)r[A1], (long)r[D0]);
}

static void
u_toupper(r)
	unsigned long *r;
{
	int c = r[D0] & 0xff;

	if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7))
		c -= 0x20;
	r[D0] = c;
}

static void
u_tolower(r)
	unsigned long *r;
{
	int c = r[D0] & 0xff;

	if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
		c += 0x20;
	r[D0] = c;
}

static struct amfn am_utiltab[] = {
	{ -30, u_findtagitem },
	{ -36, u_gettagdata },
	{ -42, u_packbooltags },
	{ -48, u_nexttagitem },
	{ -90, u_taginarray },
	{ -102, u_callhookpkt },
	{ -138, u_smult32 },
	{ -144, u_smult32 },		/* UMult32: same low 32 bits */
	{ -150, u_sdivmod32 },
	{ -156, u_udivmod32 },
	{ -162, u_stricmp },
	{ -168, u_strnicmp },
	{ -174, u_toupper },
	{ -180, u_tolower },
	{ -198, u_smult64 },
	{ -204, u_umult64 },
	{ 0, 0 }
};

struct amlib am_utility = {
	"utility.library", NT_LIBRARY, 40, 40, LIB_SIZE + 2, am_utiltab
};

/* ---------------------------------------------------------- expansion */

#define	AM_NBOARD	16

static char *am_expbase;
static char *am_cd[AM_NBOARD];		/* the ConfigDevs we made */
static unsigned long am_cdpa[AM_NBOARD];	/* their physical bases */
static unsigned char am_bind[16];	/* CurrentBinding */

static void
e_findconfigdev(r)
	unsigned long *r;
{
	char *l = am_expbase + EXB_BOARDLIST, *cd = (char *)r[A0];
	long m = r[D0], p = r[D1];

	cd = cd ? AP(cd, LN_SUCC) : AP(l, LH_HEAD);
	for (; cd && AP(cd, LN_SUCC); cd = AP(cd, LN_SUCC))
		if ((m == -1 || AW(cd, ER_MANUFACTURER) == (m & 0xffff)) &&
		    (p == -1 || AB(cd, ER_PRODUCT) == (p & 0xff))) {
			r[D0] = (unsigned long)cd;
			return;
		}
	r[D0] = 0;
}

static void
e_allocconfigdev(r)
	unsigned long *r;
{
	r[D0] = (unsigned long)am_alloc((unsigned long)CD_SIZE,
	    MEMF_PUBLIC | MEMF_CLEAR);
}

static void
e_freeconfigdev(r)
	unsigned long *r;
{
	char *cd = (char *)r[A0];
	int i;

	for (i = 0; i < AM_NBOARD; i++)
		if (am_cd[i] == cd)
			return;			/* ours: freed by am_expfini */
	am_free(cd, (unsigned long)CD_SIZE);
}

static void
e_addconfigdev(r)
	unsigned long *r;
{
	am_addtail(am_expbase + EXB_BOARDLIST, (char *)r[A0]);
}

static void
e_remconfigdev(r)
	unsigned long *r;
{
	am_remove((char *)r[A0]);
}

static void
e_setcurrentbinding(r)
	unsigned long *r;
{
	char *b = (char *)r[A0];
	unsigned long i, n = r[D0] < 16 ? r[D0] : 16;

	for (i = 0; i < 16; i++)
		am_bind[i] = i < n ? b[i] : 0;
}

static void
e_getcurrentbinding(r)
	unsigned long *r;
{
	char *b = (char *)r[A0];
	unsigned long i, n = r[D0] < 16 ? r[D0] : 16;

	for (i = 0; i < n; i++)
		b[i] = am_bind[i];
	r[D0] = 16;
}

static void
e_nop(r)
	unsigned long *r;
{
	r[D0] = 0;
}

static struct amfn am_exptab[] = {
	{ -30, e_addconfigdev },
	{ -48, e_allocconfigdev },
	{ -72, e_findconfigdev },
	{ -84, e_freeconfigdev },
	{ -108, e_remconfigdev },
	{ -120, e_nop },		/* ObtainConfigBinding */
	{ -126, e_nop },		/* ReleaseConfigBinding */
	{ -132, e_setcurrentbinding },
	{ -138, e_getcurrentbinding },
	{ 0, 0 }
};

struct amlib am_expansion = {
	"expansion.library", NT_LIBRARY, 40, 30, EXB_SIZE, am_exptab
};

int
am_expinit()
{
	struct amx_zboard zb;
	char *cd, *va;
	int i, n = 0;

	if ((am_expbase = am_addlib(&am_expansion)) == 0)
		return -1;
	am_newlist(am_expbase + EXB_BOARDLIST);
	am_newlist(am_expbase + EXB_MOUNTLIST);
	for (i = 0; i < AM_NBOARD; i++)
		am_cd[i] = 0;
	for (i = 0; n < AM_NBOARD && amx_zorro(i, &zb) == 0; i++) {
		va = amx_iomap(zb.zb_pa, zb.zb_size);
		if (va == 0) {
			amx_log("amilib: board %d/%d at %x: cannot map %x bytes\n",
			    (long)zb.zb_manuf, (long)zb.zb_prod, (long)zb.zb_pa,
			    (long)zb.zb_size);
			continue;
		}
		cd = am_alloc((unsigned long)CD_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
		if (cd == 0) {
			amx_iounmap(va, zb.zb_size);
			break;
		}
		AB(cd, ER_TYPE) = zb.zb_type;
		AB(cd, ER_PRODUCT) = zb.zb_prod;
		AB(cd, ER_FLAGS) = zb.zb_flags;
		AW(cd, ER_MANUFACTURER) = zb.zb_manuf;
		AL(cd, ER_SERIAL) = zb.zb_serial;
		AW(cd, ER_INITDIAGVEC) = zb.zb_diagvec;
		AP(cd, CD_BOARDADDR) = va;
		AL(cd, CD_BOARDSIZE) = zb.zb_size;
		am_addtail(am_expbase + EXB_BOARDLIST, cd);
		am_cdpa[n] = zb.zb_pa;
		am_cd[n++] = cd;
	}
	return 0;
}

int
am_inboard(va)
	char *va;
{
	int i;
	char *cd;

	for (i = 0; i < AM_NBOARD; i++)
		if ((cd = am_cd[i]) != 0 && va >= AP(cd, CD_BOARDADDR) &&
		    va - AP(cd, CD_BOARDADDR) < AL(cd, CD_BOARDSIZE))
			return 1;
	return 0;
}

/* the physical address behind a board's kernel address, or ~0 */
unsigned long
am_boardpa(va)
	char *va;
{
	int i;
	char *cd;

	for (i = 0; i < AM_NBOARD; i++)
		if ((cd = am_cd[i]) != 0 && va >= AP(cd, CD_BOARDADDR) &&
		    va - AP(cd, CD_BOARDADDR) < AL(cd, CD_BOARDSIZE))
			return am_cdpa[i] + (va - AP(cd, CD_BOARDADDR));
	return ~0UL;
}

void
am_expfini()
{
	int i;
	char *cd;

	for (i = 0; i < AM_NBOARD; i++)
		if ((cd = am_cd[i]) != 0) {
			amx_iounmap(AP(cd, CD_BOARDADDR), AL(cd, CD_BOARDSIZE));
			am_free(cd, (unsigned long)CD_SIZE);
			am_cd[i] = 0;
		}
	am_dellib(&am_expansion);
	am_expbase = 0;
}

/* ------------------------------------------------------- timer.device */

#define	TV_SECS		0
#define	TV_MICRO	4

static void
t_open(r)
	unsigned long *r;
{
	char *io = (char *)r[A1], *d = (char *)r[A6];

	if (r[D0] > 4) {			/* UNIT_MICROHZ .. UNIT_WAITECLOCK */
		AB(io, IO_ERROR) = 0xff;
		return;
	}
	AP(io, IO_UNIT) = d + LIB_SIZE;		/* any non-null token */
	AB(io, IO_ERROR) = 0;
	AW(d, LIB_OPENCNT)++;
}

static void
t_close(r)
	unsigned long *r;
{
	char *d = (char *)r[A6];

	if (AW(d, LIB_OPENCNT))
		AW(d, LIB_OPENCNT)--;
	r[D0] = 0;
}

static void
t_beginio(r)
	unsigned long *r;
{
	char *io = (char *)r[A1];
	unsigned long tv[2], s;

	AB(io, IO_ERROR) = 0;
	switch (AW(io, IO_COMMAND)) {
	case TR_ADDREQUEST:
		s = AL(io, TR_SECS);
		if (s > 10) {
			amx_log("amilib: timer wait of %d s cut to 10\n", (long)s,
			    0L, 0L, 0L);
			s = 10;
		}
		while (s--)
			amx_delayus(1000000L);
		amx_delayus((long)(AL(io, TR_MICRO) % 1000000));
		break;
	case TR_GETSYSTIME:
		amx_time(tv);
		AL(io, TR_SECS) = tv[0];
		AL(io, TR_MICRO) = tv[1];
		break;
	default:
		AB(io, IO_ERROR) = 0xfd;		/* IOERR_NOCMD */
		break;
	}
	am_iodone(io);
}

static void
t_abortio(r)
	unsigned long *r;
{
	r[D0] = 0;
}

static void
t_addtime(r)
	unsigned long *r;
{
	char *d = (char *)r[A0], *s = (char *)r[A1];

	AL(d, TV_SECS) += AL(s, TV_SECS);
	AL(d, TV_MICRO) += AL(s, TV_MICRO);
	if (AL(d, TV_MICRO) >= 1000000) {
		AL(d, TV_MICRO) -= 1000000;
		AL(d, TV_SECS)++;
	}
}

static void
t_subtime(r)
	unsigned long *r;
{
	char *d = (char *)r[A0], *s = (char *)r[A1];

	if (AL(d, TV_MICRO) < AL(s, TV_MICRO)) {
		AL(d, TV_MICRO) += 1000000;
		AL(d, TV_SECS)--;
	}
	AL(d, TV_MICRO) -= AL(s, TV_MICRO);
	AL(d, TV_SECS) -= AL(s, TV_SECS);
}

/* CmpTime(dest, src): 0 equal, -1 dest later than src, 1 earlier */
static void
t_cmptime(r)
	unsigned long *r;
{
	char *d = (char *)r[A0], *s = (char *)r[A1];
	long c;

	if (AL(d, TV_SECS) != AL(s, TV_SECS))
		c = AL(d, TV_SECS) > AL(s, TV_SECS) ? -1 : 1;
	else if (AL(d, TV_MICRO) != AL(s, TV_MICRO))
		c = AL(d, TV_MICRO) > AL(s, TV_MICRO) ? -1 : 1;
	else
		c = 0;
	r[D0] = c;
}

/* ReadEClock: E-clock ticks (709379 Hz) since the epoch of amx_time */
static void
t_readeclock(r)
	unsigned long *r;
{
	char *d = (char *)r[A0];
	unsigned long tv[2], hi, lo, f;

	amx_time(tv);
	umul64(tv[0], 709379L, &hi, &lo);
	/* micro < 10^6: micro * 0.709379 in two exact 32-bit pieces */
	f = tv[1] * 709 / 1000 + tv[1] * 379 / 1000000;
	lo += f;
	if (lo < f)
		hi++;
	AL(d, 0) = hi;
	AL(d, 4) = lo;
	r[D0] = 709379;
}

static void
t_getsystime(r)
	unsigned long *r;
{
	char *d = (char *)r[A0];
	unsigned long tv[2];

	amx_time(tv);
	AL(d, TV_SECS) = tv[0];
	AL(d, TV_MICRO) = tv[1];
}

static struct amfn am_timertab[] = {
	{ -6, t_open },
	{ -12, t_close },
	{ -30, t_beginio },
	{ -36, t_abortio },
	{ -42, t_addtime },
	{ -48, t_subtime },
	{ -54, t_cmptime },
	{ -60, t_readeclock },
	{ -66, t_getsystime },
	{ 0, 0 }
};

struct amlib am_timer = {
	"timer.device", NT_DEVICE, 40, 11, LIB_SIZE + 2, am_timertab
};
