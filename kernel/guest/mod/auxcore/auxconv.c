/*
 * auxconv.c -- A/UX <-> AMIX numbers and structures: errno, signals,
 * masks, wait status, open flags, termio, stat, directory entries.
 *
 * K&R C.
 */

#include "auxcore.h"
#include "sys/stat.h"
#include "sys/dirent.h"
#include "sys/termio.h"
#include "sys/utsname.h"
#include "sys/pathname.h"
#include "sys/procset.h"
#include "sys/conf.h"
#include "sys/var.h"

extern int aux_amix(), aux_sockctl();

/* ---- errno ---- */

/* AMIX value -> A/UX value, where they differ */
static short errmap[][2] = {
	{ 46, 101 }, { 56, 100 }, { 60, 87 }, { 61, 88 }, { 62, 89 },
	{ 63, 90 }, { 66, 96 }, { 78, 83 }, { 89, 102 }, { 90, 82 },
	{ 93, 86 }, { 94, 98 }, { 95, 58 }, { 96, 59 }, { 97, 60 },
	{ 98, 61 }, { 99, 62 }, { 120, 63 }, { 121, 64 }, { 122, 65 },
	{ 123, 66 }, { 124, 67 }, { 125, 68 }, { 126, 69 }, { 127, 70 },
	{ 128, 71 }, { 129, 72 }, { 130, 73 }, { 131, 74 }, { 132, 75 },
	{ 133, 76 }, { 134, 77 }, { 143, 78 }, { 144, 79 }, { 145, 80 },
	{ 146, 81 }, { 147, 84 }, { 148, 85 }, { 149, 57 }, { 150, 56 },
	{ 151, 95 },
	{ 79, 27 },			/* EOVERFLOW -> EFBIG */
	{ 88, 22 }, { 80, 22 },		/* EILSEQ, ENOTUNIQ -> EINVAL */
	{ 81, 9 },			/* EBADFD -> EBADF */
	{ 83, 8 }, { 84, 8 }, { 85, 8 }, { 86, 8 }, { 87, 8 },	/* ELIB* */
	{ 0, 0 }
};

int
aux_errno_out(e, ap)
	int e;
	struct aux_proc *ap;
{
	int i;

	if (e == EAGAIN)
		return ap->ap_compat & COMPAT_BSDNBIO ? AUX_EWOULDBLOCK : EAGAIN;
	if (e == AUXE_WOULDBLOCK)
		return AUX_EWOULDBLOCK;
	if (e == AUXE_AGAIN)
		return EAGAIN;
	if (e == ERESTART)
		return EINTR;
	if (e > 0 && e <= 45)
		return e;
	for (i = 0; errmap[i][0]; i++)
		if (errmap[i][0] == e)
			return errmap[i][1];
	return EIO;
}

/* ---- signals: A/UX 1..19 = AMIX 1..19 ---- */

static char sigin[32] = {		/* A/UX -> AMIX */
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
	24, 26, 27, 23, 30, 31, 28, 29, 20, 25, 21, 22
};
static char sigout[32] = {		/* AMIX -> A/UX */
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
	28, 30, 31, 23, 20, 29, 21, 22, 26, 27, 24, 25
};

int
aux_sig_in(s)
	int s;
{
	return s >= 0 && s < 32 ? sigin[s] : -1;
}

int
aux_sig_out(s)
	int s;
{
	return s >= 0 && s < 32 ? sigout[s] : 0;
}

u_long
aux_mask_in(m)
	u_long m;
{
	u_long r = 0;
	int s;

	for (s = 1; s < 32; s++)
		if (m & 1L << (s - 1))
			r |= 1L << (sigin[s] - 1);
	return r;
}

u_long
aux_mask_out(m)
	u_long m;
{
	u_long r = 0;
	int s;

	for (s = 1; s < 32; s++)
		if ((m & 1L << (s - 1)) && sigout[s])
			r |= 1L << (sigout[s] - 1);
	return r;
}

/* wait status: signal numbers inside */
int
aux_wstat_out(st)
	int st;
{
	if ((st & 0xff) == 0x7f)
		return (st & ~0xff00) | aux_sig_out((st >> 8) & 0xff) << 8;
	if (st & 0x7f)
		return (st & ~0x7f) | aux_sig_out(st & 0x7f);
	return st;
}

/* ---- open flags ---- */

int
aux_oflags_in(f, ap)
	int f;
	struct aux_proc *ap;
{
	int r = f & 0x70b;			/* access, APPEND, CREAT/TRUNC/EXCL */

	if (f & 0x4)
		r |= ap->ap_compat & COMPAT_BSDNBIO ? 0x80 : 0x4;
	if (f & 0x80)
		r |= 0x10;			/* O_SYNC */
	if (f & 0x4000)
		r |= 0x80;			/* O_NONBLOCK */
	if (f & 0x8000)
		r |= 0x800;			/* O_NOCTTY */
	return r;
}

int
aux_oflags_out(f, ap)
	int f;
	struct aux_proc *ap;
{
	int r = f & 0x70b;

	if (f & 0x4)
		r |= 0x4;
	if (f & 0x10)
		r |= 0x80;
	if (f & 0x80)
		r |= ap->ap_compat & COMPAT_BSDNBIO ? 0x4 : 0x4000;
	if (f & 0x800)
		r |= 0x8000;
	return r;
}

int
aux_open(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	char b[80];
	u_int n;
	int e, global = a[1] & AUX_OGLOBAL;
	long d[3], fd;
	rval_t dv;

	a[1] = aux_oflags_in((int)a[1], ap);
	e = aux_amix(5, a, rv);
	/*
	 * O_GLOBAL: from fd 128 on, as the Mac side's shared files; else
	 * where it is.  Closed on exec: a new image leaves the layer.
	 */
	if (e == 0 && global && rv->r_val1 < AUX_GFD) {
		d[0] = rv->r_val1;
		d[1] = 0;			/* F_DUPFD */
		d[2] = AUX_GFD;
		if (aux_amix(62, d, &dv) == 0) {
			fd = dv.r_val1;
			aux_amix(6, d, &dv);	/* close */
			d[0] = rv->r_val1 = fd;
			d[1] = 2;		/* F_SETFD */
			d[2] = 1;		/* FD_CLOEXEC */
			(void)aux_amix(62, d, &dv);
		}
	}
	if ((aux_trace & 4) && copyinstr((caddr_t)a[0], b + 5, sizeof b - 5, &n) == 0) {
		bcopy("open ", b, 5);
		aux_tlog((int)u.u_procp->p_pid, b, e ? (long)-e : (long)rv->r_val1);
	}
	return e;
}

int
aux_fcntl(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	int e;

	switch (a[1]) {
	case 0: case 1: case 2:			/* F_DUPFD, F_GETFD, F_SETFD */
		return aux_amix(62, a, rv);
	case 3:
		if ((e = aux_amix(62, a, rv)) == 0)
			rv->r_val1 = aux_oflags_out(rv->r_val1, ap);
		return e;
	case 4:
		a[2] = aux_oflags_in((int)a[2], ap);
		return aux_amix(62, a, rv);
	case 5: case 6: case 7:			/* F_GETLK, F_SETLK, F_SETLKW */
		return aux_fcntl_lk(ap, a, rv);
	case 8: case 9:				/* F_GETOWN, F_SETOWN: 23, 24 */
		a[1] += 15;
		return aux_amix(62, a, rv);
	}
	return EINVAL;
}

/* ---- termio ---- */

#define	TIO_SIZE	18

static void
tio_out(b)
	char *b;
{
	int i, f = G16(b + 6), c = G16(b + 4), l;

	P16(b, G16(b) & ~0xa000);			/* IMAXBEL, DOSMODE */
	c &= ~(010000 | 0100000);			/* RCV1EN, XCLUDE */
	if (c & 040000)
		c = (c & ~040000) | 010000;		/* LOBLK */
	P16(b + 4, c);
	l = f & 0xff;
	if (f & 0x8000)
		l |= 0x200;				/* IEXTEN */
	if (f & 0x100)
		l |= 0x8000;				/* TOSTOP */
	P16(b + 6, l);
	for (i = 0; i < 8; i++)
		if ((i != 4 && i != 5) || (l & 2))
			if (b[9 + i] == 0)
				b[9 + i] = (char)0377;
}

static void
tio_in(b)
	char *b;
{
	int i, c = G16(b + 4), f = G16(b + 6), l;

	c &= ~(040000 | 0100000);
	if (c & 010000)
		c = (c & ~010000) | 040000;
	P16(b + 4, c);
	l = f & 0xff;
	if (f & 0x200)
		l |= 0x8000;
	if (f & 0x8000)
		l |= 0x100;
	P16(b + 6, l);
	for (i = 0; i < 8; i++)
		if ((i != 4 && i != 5) || (l & 2))
			if ((b[9 + i] & 0xff) == 0377)
				b[9 + i] = 0;
}

/* A/UX ioctl command -> AMIX command with the same argument */
static long iocmap[][2] = {
	{ 0x20005405, 0x5405 }, { 0x20005406, 0x5406 }, { 0x20005407, 0x5407 },
	{ 0x40047477, 0x7414 }, { 0x80047476, 0x7415 },	/* TIOC[GS]PGRP */
	{ 0x40047466, 0x7414 }, { 0x80047465, 0x7415 },	/* TC_PX_[GS]ETPGRP */
	{ 0x40087468, 0x5468 }, { 0x80087467, 0x5467 },	/* TIOC[GS]WINSZ */
	{ 0x40067474, 0x7474 }, { 0x80067475, 0x7475 },	/* TIOC[GS]LTC */
	{ 0x20007471, 0x7471 },				/* TIOCNOTTY */
	{ 0x7408, 0x7408 }, { 0x7409, 0x7409 },		/* TIOC[GS]ETP */
	{ 0x40067408, 0x7408 }, { 0x80067409, 0x7409 },
	{ 0x4004667f, 0x4004667f },			/* FIONREAD */
	{ 0x8004667d, 0x8004667d },			/* FIOASYNC */
	{ 0x8004667c, 0x8004667c }, { 0x4004667b, 0x4004667b },
	{ 0x20004400, 0x4400 }, { 0x20004401, 0x4401 }, { 0x20004402, 0x4402 },
	{ 0x40064408, 0x4408 }, { 0x80064409, 0x4409 },	/* LD[GS]ETT */
	{ 0, 0 }
};

/* fd is a character device without STREAMS */
static int
plaindev(fd)
	int fd;
{
	file_t *fp;
	struct vnode *vp;

	if (getf(fd, &fp))
		return 0;
	vp = fp->f_vnode;
	return vp->v_type == VCHR && getmajor(vp->v_rdev) < cdevcnt &&
	    cdevsw[getmajor(vp->v_rdev)].d_str == 0;
}

/* fd is character major m */
static int
majdev(fd, m)
	int fd, m;
{
	file_t *fp;
	struct vnode *vp;

	if (getf(fd, &fp))
		return 0;
	vp = fp->f_vnode;
	return vp->v_type == VCHR && getmajor(vp->v_rdev) == m;
}

/* fd is /dev/otbridge (character major 55) */
static int
otbdev(fd)
	int fd;
{
	file_t *fp;
	struct vnode *vp;

	if (getf(fd, &fp))
		return 0;
	vp = fp->f_vnode;
	return vp->v_type == VCHR && getmajor(vp->v_rdev) == 55;
}

int
aux_ioctl(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	char b[TIO_SIZE];
	caddr_t g;
	file_t *fp;
	long cmd = a[1];
	int e, i, v;

	if (aux_sockctl(a, rv, r, &e))
		return e;
	switch (cmd) {
	case 0x40125401:			/* TCGETA */
		a[1] = 0x5401;
		if ((e = aux_amix(54, a, rv)) != 0)
			return e;
		if (copyin((caddr_t)a[2], b, TIO_SIZE))
			return EFAULT;
		tio_out(b);
		return copyout(b, (caddr_t)a[2], TIO_SIZE) ? EFAULT : 0;
	case 0x80125402: case 0x80125403: case 0x80125404:	/* TCSETA* */
		if (copyin((caddr_t)a[2], b, TIO_SIZE))
			return EFAULT;
		tio_in(b);
		g = aux_gap(r, TIO_SIZE);
		if (copyout(b, g, TIO_SIZE))
			return EFAULT;
		a[1] = cmd & 0xffff;
		a[2] = (long)g;
		return aux_amix(54, a, rv);
	case 0x20006601: case 0x20006602:	/* FIOCLEX, FIONCLEX */
		a[1] = 2;
		a[2] = cmd == 0x20006601;
		return aux_amix(62, a, rv);
	case 0x80047470:			/* TIOCPKT */
		return aux_tiocpkt((int)a[0], (caddr_t)a[2]);
	case 0x8004667e:			/* FIONBIO */
		if ((e = getf((int)a[0], &fp)) != 0)
			return e;
		if (copyin((caddr_t)a[2], (caddr_t)&v, sizeof v))
			return EFAULT;
		if (v)
			fp->f_flag |= FNONBLOCK;
		else
			fp->f_flag &= ~FNONBLOCK;
		return 0;
	}
	if ((cmd & 0xff00) == 0x5100 && plaindev((int)a[0]))
		return aux_amix(54, a, rv);	/* 'Q': /dev/uinter0, unchanged */
	if ((cmd & 0xff00) == ('o' << 8) && otbdev((int)a[0]))
		return aux_amix(54, a, rv);	/* 'o': /dev/otbridge, unchanged */
	if ((cmd & 0xff00) == ('w' << 8) && majdev((int)a[0], 47))
		return aux_amix(54, a, rv);	/* 'w': /dev/snd, unchanged */
	for (i = 0; iocmap[i][0]; i++)
		if (iocmap[i][0] == cmd) {
			a[1] = iocmap[i][1];
			return aux_amix(54, a, rv);
		}
	if (aux_trace & 3)
		printf("aux %d: ioctl %x\n", (int)u.u_procp->p_pid, (int)cmd);
	return EINVAL;
}

/* ---- stat ---- */

#define	ASTAT_SIZE	58
#define	AXSTAT_SIZE	98

static int
auxdev(d)
	dev_t d;
{
	u_long mj = getmajor(d), mn = getminor(d);

	if (mj < 256 && mn < 256)
		return mj << 8 | mn;
	return 0x8000 | ((mj * 131 + mn) & 0x7fff);
}

static int
ifmt(t)
	int t;
{
	switch (t) {
	case VREG:	return S_IFREG;
	case VDIR:	return S_IFDIR;
	case VBLK:	return S_IFBLK;
	case VCHR:	return S_IFCHR;
	case VLNK:	return S_IFLNK;
	case VFIFO:	return S_IFIFO;
	}
	return 0;
}

/* sz: the A/UX stat (58) or xstat (98, st_xerror = EINVAL) */
static int
vstat(vp, ub, sz)
	struct vnode *vp;
	caddr_t ub;
	int sz;
{
	struct vattr va;
	char b[AXSTAT_SIZE];
	int e;

	va.va_mask = AT_ALL;
	if ((e = VOP_GETATTR(vp, &va, 0, u.u_cred)) != 0)
		return e;
	bzero(b, sizeof b);
	P16(b + 0x00, auxdev(va.va_fsid));
	P16(b + 0x02, va.va_nodeid);
	P16(b + 0x04, (va.va_mode & 07777) | ifmt((int)va.va_type));
	P16(b + 0x06, va.va_nlink > 0x7fff ? 0x7fff : va.va_nlink);
	P16(b + 0x08, va.va_uid);
	P16(b + 0x0a, va.va_gid);
	P16(b + 0x0c, auxdev(va.va_rdev));
	P32(b + 0x0e, va.va_size);
	P32(b + 0x12, va.va_atime.tv_sec);
	P32(b + 0x16, va.va_nodeid);
	P32(b + 0x1a, va.va_mtime.tv_sec);
	P32(b + 0x22, va.va_ctime.tv_sec);
	P32(b + 0x2a, va.va_blksize);
	P32(b + 0x2e, va.va_nblocks);
	P32(b + 0x32, va.va_uid);
	P32(b + 0x36, va.va_gid);
	if (sz == AXSTAT_SIZE)
		P32(b + 0x3a, EINVAL);
	return copyout(b, ub, sz) ? EFAULT : 0;
}

static int
pstat(path, ub, follow, sz)
	caddr_t path, ub;
	enum symfollow follow;
	int sz;
{
	struct vnode *vp;
	int e;

	if ((e = lookupname(path, UIO_USERSPACE, follow, NULLVPP, &vp)) != 0)
		return e;
	e = vstat(vp, ub, sz);
	VN_RELE(vp);
	return e;
}

static int
fstat_(fd, ub, sz)
	int fd;
	caddr_t ub;
	int sz;
{
	file_t *fp;
	int e;

	if ((e = getf(fd, &fp)) != 0)
		return e;
	return vstat(fp->f_vnode, ub, sz);
}

int
aux_stat(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return pstat((caddr_t)a[0], (caddr_t)a[1], FOLLOW, ASTAT_SIZE);
}

int
aux_lstat(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return pstat((caddr_t)a[0], (caddr_t)a[1], NO_FOLLOW, ASTAT_SIZE);
}

int
aux_fstat(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return fstat_((int)a[0], (caddr_t)a[1], ASTAT_SIZE);
}

int
aux_xstat(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return pstat((caddr_t)a[0], (caddr_t)a[1], FOLLOW, AXSTAT_SIZE);
}

int
aux_xlstat(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return pstat((caddr_t)a[0], (caddr_t)a[1], NO_FOLLOW, AXSTAT_SIZE);
}

int
aux_xfstat(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return fstat_((int)a[0], (caddr_t)a[1], AXSTAT_SIZE);
}

/*
 * setxinfo/fsetxinfo(file, xinfo): no file system here keeps Finder
 * info (xstat's st_xerror), so after the checks this fails as on
 * A/UX's SVFS; the Mac side keeps it in its AppleDouble files.
 */
static int
xinfo_in(ub)
	caddr_t ub;
{
	char b[36];

	return copyin(ub, b, sizeof b) ? EFAULT : EINVAL;
}

int
aux_setxinfo(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	struct vnode *vp;
	int e;

	if ((e = lookupname((caddr_t)a[0], UIO_USERSPACE, FOLLOW, NULLVPP, &vp)) != 0)
		return e;
	VN_RELE(vp);
	return xinfo_in((caddr_t)a[1]);
}

int
aux_fsetxinfo(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	file_t *fp;
	int e;

	if ((e = getf((int)a[0], &fp)) != 0)
		return e;
	return xinfo_in((caddr_t)a[1]);
}

/* ---- getdirentries(fd, buf, nbytes, basep) ---- */

#define	DIRMAX	8192

int
aux_getdirentries(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	file_t *fp;
	struct vnode *vp;
	struct uio uio;
	struct iovec iov;
	char *kb, *ob, *d;
	long base, cur;
	int e, n, got, off, o, nl, rl, ol, eof;

	if ((e = getf((int)a[0], &fp)) != 0)
		return e;
	vp = fp->f_vnode;
	if (!(fp->f_flag & FREAD))
		return EBADF;
	if (vp->v_type != VDIR)
		return ENOTDIR;
	n = a[2] > DIRMAX ? DIRMAX : a[2];
	if (n < 16)
		return EINVAL;
	kb = (char *)kmem_alloc((size_t)(2 * n), KM_SLEEP);
	ob = kb + n;
	iov.iov_base = kb;
	iov.iov_len = n;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = base = fp->f_offset;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_fmode = 0;
	uio.uio_limit = 0x7fffffff;
	uio.uio_resid = n;
	VOP_RWLOCK(vp);
	e = VOP_READDIR(vp, &uio, fp->f_cred, &eof);
	VOP_RWUNLOCK(vp);
	if (e)
		goto out;
	got = n - uio.uio_resid;
	cur = base;
	for (off = o = 0; off + 10 < got; off += rl) {
		d = kb + off;
		rl = G16(d + 8);
		if (rl < 12 || off + rl > got)
			break;
		for (nl = 0; 10 + nl < rl && d[10 + nl]; nl++)
			;
		ol = 8 + ((nl + 1 + 3) & ~3);
		if (o + ol > n)
			break;
		P32(ob + o, G32(d));
		P16(ob + o + 4, ol);
		P16(ob + o + 6, nl);
		bzero(ob + o + 8, ol - 8);
		bcopy(d + 10, ob + o + 8, nl);
		o += ol;
		cur = G32(d + 4);
	}
	if (o == 0 && off < got)
		e = EINVAL;			/* first entry exceeds nbytes */
	else if (copyout(ob, (caddr_t)a[1], o) ||
	    (a[3] && copyout((caddr_t)&base, (caddr_t)a[3], sizeof base)))
		e = EFAULT;
	else {
		fp->f_offset = off < got ? cur : uio.uio_offset;
		rv->r_val1 = o;
	}
out:
	kmem_free((_VOID *)kb, (size_t)(2 * n));
	return e;
}

/* ---- names and time ---- */

extern struct utsname utsname;
extern timestruc_t hrestime;

static void
cpname(d, s, n)
	char *d, *s;
	int n;
{
	int i;

	for (i = 0; i < n - 1 && s[i]; i++)
		d[i] = s[i];
	for (; i < n; i++)
		d[i] = 0;
}

/*
 * utssys(buf, 0, 33): A/UX uvar(), a 512-byte struct var.  Filled:
 * v_call, v_proc, v_maxup, v_hz, v_pageshift, v_pagemask, v_maxpmem
 * (physical memory in pages, which the Mac side sizes its RAM from).
 */
static int
uvar(ub)
	caddr_t ub;
{
	char *b;
	int e;

	b = kmem_zalloc(512, KM_SLEEP);
	P32(b + 0x04, v.v_call);
	P32(b + 0x20, v.v_proc);
	P32(b + 0x38, v.v_maxup);
	P32(b + 0x68, HZ);
	P32(b + 0x70, 12);			/* 4 KB pages */
	P32(b + 0x74, 0xfff);
	P32(b + 0xc4, physmem);
	e = copyout(b, ub, 512) ? EFAULT : 0;
	kmem_free((_VOID *)b, 512);
	return e;
}

/* utssys(buf, 0, 0): uname in 5 x 9 bytes */
int
aux_utssys(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	char b[45];

	if (a[2] == 33)
		return uvar((caddr_t)a[0]);
	if (a[2] != 0)
		return EINVAL;
	cpname(b, utsname.sysname, 9);
	cpname(b + 9, utsname.nodename, 9);
	cpname(b + 18, utsname.release, 9);
	cpname(b + 27, utsname.version, 9);
	cpname(b + 36, utsname.machine, 9);
	return copyout(b, (caddr_t)a[0], sizeof b) ? EFAULT : 0;
}

int
aux_gethostname(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	char b[SYS_NMLN];
	int n = a[1];

	if (n <= 0)
		return EINVAL;
	if (n > sizeof b)
		n = sizeof b;
	cpname(b, utsname.nodename, n);
	return copyout(b, (caddr_t)a[0], n) ? EFAULT : 0;
}

int
aux_getdomainname(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return a[1] > 0 && subyte((caddr_t)a[0], 0) ? EFAULT : 0;
}

/* _gettimeofday(tv) and gettimeofday(tv, tz) */
int
aux_gettimeofday(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	long tv[2];

	tv[0] = hrestime.tv_sec;
	tv[1] = hrestime.tv_nsec / 1000;
	if (a[0] && copyout((caddr_t)tv, (caddr_t)a[0], sizeof tv))
		return EFAULT;
	tv[0] = tv[1] = 0;
	if (a[1] && copyout((caddr_t)tv, (caddr_t)a[1], sizeof tv))
		return EFAULT;
	return 0;
}

int
aux_getdtablesize(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	rv->r_val1 = u.u_rlimit[RLIMIT_NOFILE].rlim_cur;
	return 0;
}

int
aux_getcompat(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	rv->r_val1 = ap->ap_compat;
	return 0;
}

int
aux_setcompat(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	u_long f = a[0];

	if ((f & (COMPAT_BSDTTY | COMPAT_SYSCALLS)) && !(f & COMPAT_BSDSIGNALS))
		return EINVAL;
	rv->r_val1 = ap->ap_compat;
	ap->ap_compat = f & ~COMPAT_CLRPGROUP;
	return 0;
}

/* wait(): status in d1 */
int
aux_wait(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	int e;

	if ((GR_SR(r) & 0x1e) == 0x1e && GR_VEC(r) == 47)
		return aux_waitcom(P_ALL, 0, (int)a[0], rv);	/* wait3 */
	if ((e = aux_amix(7, a, rv)) == 0)
		rv->r_val2 = aux_wstat_out(rv->r_val2);
	return e;
}

/* setpgrp(cmd, pid, pgrp) */
int
aux_setpgrp(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	if (a[0] == 0)
		return aux_amix(39, a, rv);
	if (a[0] != 1)
		return EINVAL;
	if (ap->ap_compat & COMPAT_BSDTTY) {
		a[0] = 5;
		return aux_amix(39, a, rv);
	}
	return aux_amix(39, a, rv);
}

int
aux_setsid(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	a[0] = 3;
	return aux_amix(39, a, rv);
}

int
aux_setpgid(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	a[2] = a[1];
	a[1] = a[0];
	a[0] = 5;
	return aux_amix(39, a, rv);
}

int
aux_swapmmumode(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	rv->r_val1 = 1 - a[0];
	return 0;
}

/* sysslotmanager(selector, SpBlock *): the Slot Manager result in d0 */
int
aux_slotmanager(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	int e, res = 0;

	if (aux_slotmgr == 0)
		return EINVAL;
	e = (*aux_slotmgr)((int)a[0], (caddr_t)a[1], &res);
	rv->r_val1 = res;
	return e;
}
