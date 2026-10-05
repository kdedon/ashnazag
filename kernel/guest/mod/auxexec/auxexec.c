/*
 * auxexec.c -- exec of A/UX COFF programs (magic 0x150).
 *
 * Registered first for 0x150.  An SVR3 image (STYP_LIB section) is
 * declined and reaches the stock COFF loader.  An A/UX image is loaded
 * by the stock loader with guest_loading naming this process, so the
 * events stub inside it makes the process an A/UX guest.  The stock
 * loader skips the A/UX .lib section; its shared libraries are checked
 * here before the old image goes and mapped after the stock loader,
 * at their link addresses.
 *
 * A/UX .lib: s_size / 64 records, each a NUL-padded 64-byte path,
 * taken only if absolute.  A library: 0x150, text + data + bss, a.out
 * magic 0410 or 0413; F_EXEC is not required.
 *
 * K&R C.
 */

#include "kinc.h"
#include "sys/vnode.h"
#include "sys/uio.h"
#include "sys/exec.h"

#define	F_EXEC		0x0002
#define	F_AR16WR	0x0100	/* A/UX 2 tools; A/UX 3 sets F_AR32WR */
#define	STYP_TEXT	0x0020
#define	STYP_DATA	0x0040
#define	STYP_BSS	0x0080
#define	STYP_INFO	0x0200
#define	STYP_LIB	0x0800
#define	MAXSCN		16
#define	HDRMAX		(20 + 28 + 40 * MAXSCN)
#define	LIBREC		64
#define	AUX_NSHLIB	4

struct auxlib {
	struct vnode	*al_vp;
	u_long		al_taddr, al_tsize, al_toff;
	u_long		al_daddr, al_dsize, al_doff, al_bsize;
};

extern int coffexec(), execmap();
extern void dlm_cacheflush();
extern struct guest_profile aux_profile;
extern int aux_execaux2;

int	aux_coff_default = 1;	/* 0: leave every 0x150 file to the stock loader */

static int
rdhdr(vp, h)
	struct vnode *vp;
	char *h;
{
	int e, resid = 0;

	e = vn_rdwr(UIO_READ, vp, (caddr_t)h, HDRMAX, (off_t)0, UIO_SYSSPACE,
	    0, 0x7fffffffL, u.u_cred, &resid);
	return e ? -1 : HDRMAX - resid;
}

/* a library's text, data and bss from its headers */
static int
libhdr(vp, al)
	struct vnode *vp;
	struct auxlib *al;
{
	char h[HDRMAX], *s;
	int got, n, opt, i, m, seen = 0;

	if ((got = rdhdr(vp, h)) < 48)
		return ELIBBAD;
	n = G16(h + 2);
	opt = G16(h + 16);
	m = G16(h + 20);
	if (G16(h) != 0x150 || opt < 28 || n > MAXSCN ||
	    20 + opt + 40 * n > got || (m != 0x108 && m != 0x10b))
		return ELIBBAD;
	for (i = 0; i < n; i++) {
		s = h + 20 + opt + 40 * i;
		switch (G32(s + 36)) {
		case STYP_TEXT:
			al->al_taddr = G32(s + 12);
			al->al_tsize = G32(s + 16);
			al->al_toff = G32(s + 20);
			seen |= 1;
			break;
		case STYP_DATA:
			al->al_daddr = G32(s + 12);
			al->al_dsize = G32(s + 16);
			al->al_doff = G32(s + 20);
			seen |= 2;
			break;
		case STYP_BSS:
			al->al_bsize = G32(s + 16);
			seen |= 4;
			break;
		}
	}
	return seen == 7 ? 0 : ELIBBAD;
}

static void
librele(al, n)
	struct auxlib *al;
	int n;
{
	for (; n > 0; n--, al++)
		if (al->al_vp) {
			VN_RELE(al->al_vp);
			al->al_vp = 0;
		}
}

/* al_vp is set while the checks run, so the u_qsav path releases it */
static int
libopen(path, al)
	char *path;
	struct auxlib *al;
{
	struct vnode *vp;
	struct vattr va;
	int e = ELIBACC;

	if (path[0] != '/' ||
	    lookupname(path, UIO_SYSSPACE, FOLLOW, NULLVPP, &al->al_vp))
		return ELIBACC;
	vp = al->al_vp;
	va.va_mask = AT_MODE;
	if (vp->v_type == VREG && VOP_GETATTR(vp, &va, 0, u.u_cred) == 0 &&
	    (va.va_mode & 0111) && VOP_ACCESS(vp, VEXEC, 0, u.u_cred) == 0 &&
	    (e = libhdr(vp, al)) == 0)
		return 0;
	librele(al, 1);
	return e;
}

/* the .lib records at off: open each library */
static int
libs(vp, off, size, al, np)
	struct vnode *vp;
	u_long off, size;
	struct auxlib *al;
	int *np;
{
	char b[LIBREC * AUX_NSHLIB];
	int n = size / LIBREC, i, e, resid = 0;

	if (n > AUX_NSHLIB)
		return ELIBMAX;
	if (n == 0)
		return 0;
	if (vn_rdwr(UIO_READ, vp, (caddr_t)b, n * LIBREC, (off_t)off,
	    UIO_SYSSPACE, 0, 0x7fffffffL, u.u_cred, &resid) || resid)
		return ELIBSCN;
	for (i = 0; i < n; i++) {
		if (b[LIBREC * i + LIBREC - 1] != 0) {
			e = ELIBSCN;
			break;
		}
		if ((e = libopen(b + LIBREC * i, al + i)) != 0)
			break;
	}
	if (i < n) {
		librele(al, i);
		return e;
	}
	*np = n;
	return 0;
}

/* 0: A/UX image, its libraries opened; ENOEXEC: not ours; else an error */
static int
aux_classify(vp, al, np)
	struct vnode *vp;
	struct auxlib *al;
	int *np;
{
	char h[HDRMAX], *s;
	int got, n, i, opt, fl;
	u_long loff = 0, lsize = 0;

	*np = 0;
	if ((got = rdhdr(vp, h)) < 20)
		return ENOEXEC;
	n = G16(h + 2);
	opt = G16(h + 16);
	if (G16(h) != 0x150 || !(G16(h + 18) & F_EXEC) || n > MAXSCN ||
	    20 + opt + 40 * n > got)
		return ENOEXEC;
	for (i = 0; i < n; i++) {
		s = h + 20 + opt + 40 * i;
		fl = G32(s + 36);
		if (fl == STYP_LIB)
			return ENOEXEC;
		if (strncmp(s, ".low24", 8) == 0)
			return ENOEXEC;
		if (fl == STYP_INFO && strncmp(s, ".lib", 8) == 0) {
			lsize = G32(s + 16);
			loff = G32(s + 20);
		}
	}
	if (!aux_coff_default)
		return ENOEXEC;
	aux_execaux2 = (G16(h + 18) & F_AR16WR) != 0;
	return libs(vp, loff, lsize, al, np);
}

/* after the stock loader: libraries at their link addresses */
static int
libmap(al, n)
	struct auxlib *al;
	int n;
{
	int e = 0;

	for (; n > 0 && e == 0; n--, al++) {
		if (al->al_tsize)
			e = execmap(al->al_vp, al->al_taddr, al->al_tsize, 0,
			    al->al_toff, 0xd);
		if (e == 0 && (al->al_dsize || al->al_bsize))
			e = execmap(al->al_vp, al->al_daddr, al->al_dsize,
			    al->al_bsize, al->al_doff, 0xf);
	}
	if (e)
		psignal(u.u_procp, SIGKILL);
	else
		dlm_cacheflush();		/* copied text */
	return e;
}

int
aux_coffexec(a0, a1, a2, a3, a4, a5, a6, a7)
	long a0, a1, a2, a3, a4, a5, a6, a7;
{
	struct auxlib al[AUX_NSHLIB];
	label_t save;
	int e, n;

	while (guest_loading)
		(void)sleep((caddr_t)&guest_loading, PZERO);
	guest_loading = u.u_procp;
	bzero((caddr_t)al, sizeof al);
	bcopy((caddr_t)&u.u_qsav, (caddr_t)&save, sizeof save);
	if (setjmp(&u.u_qsav)) {
		librele(al, AUX_NSHLIB);
		guest_loading = 0;
		wakeup((caddr_t)&guest_loading);
		bcopy((caddr_t)&save, (caddr_t)&u.u_qsav, sizeof save);
		longjmp(&u.u_qsav);
	}
	if ((e = aux_classify((struct vnode *)a0, al, &n)) == 0) {
		guest_loadprof = &aux_profile;
		e = coffexec(a0, a1, a2, a3, a4, a5, a6, a7);
		if (e == 0)
			e = libmap(al, n);
	}
	librele(al, AUX_NSHLIB);
	guest_loading = 0;
	wakeup((caddr_t)&guest_loading);
	bcopy((caddr_t)&save, (caddr_t)&u.u_qsav, sizeof save);
	return e;
}

struct mod_exec_data auxexec_execdata[] = {
	{ 0x150, 0, aux_coffexec, 0 },
	{ 0, 0, 0, 0 }
};

MOD_EXEC_WRAPPER(auxexec, 0, 0, "A/UX COFF exec");
