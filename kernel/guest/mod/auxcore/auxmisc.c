/*
 * auxmisc.c -- A/UX calls built on AMIX internals: select, wait3 and
 * waitpid, record locks and flock, statfs, truncate, utimes, itimers,
 * setre[ug]id, shmsys, sigpending, and pty packet mode.
 *
 * Where an AMIX handler takes user pointers, the converted structure
 * goes into a scratch area below the user stack pointer (aux_gap).
 *
 * K&R C.
 */

#include "auxcore.h"
#include "sys/stat.h"
#include "sys/fcntl.h"
#include "sys/wait.h"
#include "sys/procset.h"
#include "sys/siginfo.h"
#include "sys/poll.h"
#include "sys/statvfs.h"
#include "sys/vfs.h"
#include "sys/pathname.h"
#include "sys/ipc.h"
#include "sys/shm.h"
#include "sys/stream.h"
#include "sys/stropts.h"
#include "sys/strsubr.h"
#include "sys/var.h"

extern int aux_amix();
extern int grow(), waitid(), strioctl(), ttimeout();
extern cred_t *crcopy();
extern clock_t lbolt;

#define	AFD_MAX		1024	/* select: descriptors looked at */
#define	GAPSKIP		512	/* gap: bytes left free below usp */
#define	SSMAX		0x10000	/* gap: sp this far below the sigstack top is on it */

static int
splhi_()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s;
}

static void
splx_(s)
	int s;
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}

/*
 * n bytes of user scratch below the caller's stack.  A handler on the
 * signal stack gets it below the sp the stack was entered from, since
 * the signal stack's size is unknown.
 */
caddr_t
aux_gap(r, n)
	char *r;
	int n;
{
	struct aux_proc *ap = AUXP(GUESTP(u.u_procp));
	u_long sp = GR_USP(r), a;

	if (ap->ap_ss_onstack && ap->ap_ss_isp && sp <= ap->ap_ss_sp &&
	    ap->ap_ss_sp - sp < SSMAX)
		sp = ap->ap_ss_isp;
	a = (sp - GAPSKIP - n) & ~7;
	(void)grow((int *)a);
	return (caddr_t)a;
}

/* ---- select(nfds, rd, wr, ex, tv) ---- */

int
aux_select(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	u_long *set[3], *out[3];
	caddr_t us[3], g;
	struct pollfd *pf, *rf;
	long tv[2];
	int n = a[0], w, i, k, m = 0, cnt = 0, ms = -1, e = 0, ev, re;
	u_int sz;

	if (n < 0)
		return EINVAL;
	if (n > u.u_rlimit[RLIMIT_NOFILE].rlim_cur)
		n = u.u_rlimit[RLIMIT_NOFILE].rlim_cur;
	if (n > AFD_MAX)
		n = AFD_MAX;
	if (a[4]) {
		if (copyin((caddr_t)a[4], (caddr_t)tv, sizeof tv))
			return EFAULT;
		if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000)
			return EINVAL;
		ms = tv[0] > 1000000 ? 1000000000 : tv[0] * 1000 + (tv[1] + 999) / 1000;
	}
	w = (n + 31) / 32;
	sz = 6 * w * 4 + 2 * n * sizeof (struct pollfd) + 4;
	set[0] = (u_long *)kmem_zalloc(sz, KM_SLEEP);
	pf = (struct pollfd *)(set[0] + 6 * w);
	rf = pf + n;
	for (k = 0; k < 3; k++) {
		us[k] = (caddr_t)a[1 + k];
		set[k] = set[0] + 2 * k * w;
		out[k] = set[k] + w;
		if (us[k] && w && copyin(us[k], (caddr_t)set[k], w * 4)) {
			e = EFAULT;
			goto done;
		}
	}
	for (i = 0; i < n; i++) {
		ev = 0;
		if (set[0][i >> 5] & 1L << (i & 31))
			ev |= POLLIN | POLLRDNORM;
		if (set[1][i >> 5] & 1L << (i & 31))
			ev |= POLLOUT;
		if (set[2][i >> 5] & 1L << (i & 31))
			ev |= POLLPRI | POLLRDBAND;
		if (ev) {
			pf[m].fd = i;
			pf[m].events = ev;
			pf[m].revents = 0;
			m++;
		}
	}
	g = aux_gap(r, m * sizeof (struct pollfd));
	if (m && copyout((caddr_t)pf, g, m * sizeof (struct pollfd))) {
		e = EFAULT;
		goto done;
	}
	a[0] = (long)g;
	a[1] = m;
	a[2] = ms;
	if ((e = aux_amix(87, a, rv)) != 0)
		goto done;
	/* only revents is taken back: the scratch is user memory */
	if (m && copyin(g, (caddr_t)rf, m * sizeof (struct pollfd))) {
		e = EFAULT;
		goto done;
	}
	for (i = 0; i < m; i++) {
		k = pf[i].fd;
		re = rf[i].revents;
		ev = pf[i].events;
		if (re & POLLNVAL) {
			e = EBADF;
			goto done;
		}
		if ((ev & POLLIN) && (re & (POLLIN | POLLRDNORM | POLLHUP | POLLERR))) {
			out[0][k >> 5] |= 1L << (k & 31);
			cnt++;
		}
		if ((ev & POLLOUT) && (re & (POLLOUT | POLLHUP | POLLERR))) {
			out[1][k >> 5] |= 1L << (k & 31);
			cnt++;
		}
		if ((ev & POLLPRI) && (re & (POLLPRI | POLLRDBAND))) {
			out[2][k >> 5] |= 1L << (k & 31);
			cnt++;
		}
	}
	for (k = 0; k < 3; k++)
		if (us[k] && w && copyout((caddr_t)out[k], us[k], w * 4)) {
			e = EFAULT;
			goto done;
		}
	rv->r_val1 = cnt;
done:
	kmem_free((_VOID *)set[0], sz);
	return e;
}

/* ---- wait3 / waitpid ---- */

/* A/UX options: WNOHANG 1, WUNTRACED 2; status as the old wait's */
int
aux_waitcom(idtype, id, aopt, rv)
	int idtype, id, aopt;
	rval_t *rv;
{
	k_siginfo_t info;
	int e, opt = WEXITED | WTRAPPED, st, s;

	if (aopt & ~3)
		return EINVAL;
	if (aopt & 1)
		opt |= WNOHANG;
	if (aopt & 2)
		opt |= WSTOPPED;
	bzero((caddr_t)&info, sizeof info);
	if ((e = waitid(idtype, id, &info, opt)) != 0)
		return e;
	s = aux_sig_out(info.si_status & 0x7f);
	switch (info.si_code) {
	case CLD_EXITED:
		st = (info.si_status & 0xff) << 8;
		break;
	case CLD_KILLED:
		st = s;
		break;
	case CLD_DUMPED:
		st = s | 0x80;
		break;
	case CLD_STOPPED:
	case CLD_TRAPPED:
		st = s << 8 | 0x7f;
		break;
	default:
		st = 0;
	}
	rv->r_val1 = info.si_pid;
	rv->r_val2 = info.si_pid ? st : 0;
	return 0;
}

/* waitpid(pid, statusp, options): status in d1, libc stores it */
int
aux_waitpid(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	int pid = a[0];

	if (pid >= 30000 || pid <= -30000)
		return ECHILD;
	if (pid > 0)
		return aux_waitcom(P_PID, pid, (int)a[2], rv);
	if (pid == -1)
		return aux_waitcom(P_ALL, 0, (int)a[2], rv);
	return aux_waitcom(P_PGID, pid ? -pid : u.u_procp->p_pgrp, (int)a[2], rv);
}

/* ---- record locks and flock ---- */

#define	ALOCK_SIZE	16	/* short type, whence; long start, len, pid */

static int
frlock(fd, fp, cmd, bf, flag)
	int fd, cmd, flag;
	file_t *fp;
	struct flock *bf;
{
	if ((cmd == F_SETLK || cmd == F_SETLKW) && bf->l_type != F_UNLCK)
		setpof(fd, getpof(fd) | 2);		/* locks to drop on close */
	return VOP_FRLOCK(fp->f_vnode, cmd, bf, flag, fp->f_offset, fp->f_cred);
}

/* fcntl F_GETLK 5, F_SETLK 6, F_SETLKW 7 */
int
aux_fcntl_lk(ap, a, rv)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
{
	struct flock bf;
	file_t *fp;
	char b[ALOCK_SIZE];
	int e, cmd = a[1] == 5 ? F_GETLK : a[1];

	if ((e = getf((int)a[0], &fp)) != 0)
		return e;
	if (copyin((caddr_t)a[2], b, sizeof b))
		return EFAULT;
	bzero((caddr_t)&bf, sizeof bf);
	bf.l_type = (short)G16(b);
	bf.l_whence = (short)G16(b + 2);
	bf.l_start = G32(b + 4);
	bf.l_len = G32(b + 8);
	e = frlock((int)a[0], fp, cmd, &bf, fp->f_flag);
	if (e == EAGAIN)
		e = EACCES;
	if (e || cmd != F_GETLK)
		return e;
	P16(b, bf.l_type);
	P16(b + 2, bf.l_whence);
	P32(b + 4, bf.l_start);
	P32(b + 8, bf.l_len);
	P32(b + 12, bf.l_pid);
	return copyout(b, (caddr_t)a[2], sizeof b) ? EFAULT : 0;
}

/*
 * flock(fd, op): a whole-file record lock.  As in BSD, the descriptor's
 * mode does not limit the lock, but the caller must own the file or be
 * able to open it for the lock's kind: native record locks share these
 * locks, and a reader's exclusive lock would stall their writers.  Not
 * on files with mandatory locking.
 */
int
aux_flock(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	struct flock bf;
	struct vattr va;
	file_t *fp;
	cred_t *cr;
	int e, op = a[1], fl, own;

	if ((e = getf((int)a[0], &fp)) != 0)
		return e;
	va.va_mask = AT_MODE | AT_UID;
	if ((e = VOP_GETATTR(fp->f_vnode, &va, 0, fp->f_cred)) != 0)
		return e;
	fl = fp->f_flag;
	if (!MANDLOCK(fp->f_vnode, va.va_mode)) {
		cr = u.u_cred;
		own = cr->cr_uid == 0 || cr->cr_uid == va.va_uid;
		if (own || VOP_ACCESS(fp->f_vnode, VREAD, 0, cr) == 0)
			fl |= FREAD;
		if (own || VOP_ACCESS(fp->f_vnode, VWRITE, 0, cr) == 0)
			fl |= FWRITE;
	}
	bzero((caddr_t)&bf, sizeof bf);
	if (op & 8)
		bf.l_type = F_UNLCK;
	else if (op & 2)
		bf.l_type = F_WRLCK;
	else if (op & 1)
		bf.l_type = F_RDLCK;
	else
		return EINVAL;
	e = frlock((int)a[0], fp, op & 4 ? F_SETLK : F_SETLKW, &bf, fl);
	return e == EACCES || e == EAGAIN ? AUXE_WOULDBLOCK : e;
}

/* ---- statfs(path, buf), fstatfs(fd, buf): the 64-byte BSD form ---- */

static int
vstatfs(vp, ub)
	struct vnode *vp;
	caddr_t ub;
{
	struct statvfs sv;
	char b[64];
	int e, t = 0;

	bzero((caddr_t)&sv, sizeof sv);
	if ((e = VFS_STATVFS(vp->v_vfsp, &sv)) != 0)
		return e;
	if (strcmp(sv.f_basetype, "nfs") == 0)
		t = 1;
	else if (strcmp(sv.f_basetype, "ufs") == 0)
		t = 3;
	bzero(b, sizeof b);
	P32(b, t);
	P32(b + 4, sv.f_frsize ? sv.f_frsize : sv.f_bsize);
	P32(b + 8, sv.f_blocks);
	P32(b + 12, sv.f_bfree);
	P32(b + 16, sv.f_bavail);
	P32(b + 20, sv.f_files);
	P32(b + 24, sv.f_ffree);
	P32(b + 28, sv.f_fsid);
	return copyout(b, ub, sizeof b) ? EFAULT : 0;
}

int
aux_statfs(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	struct vnode *vp;
	int e;

	if ((e = lookupname((caddr_t)a[0], UIO_USERSPACE, FOLLOW, NULLVPP, &vp)) != 0)
		return e;
	e = vstatfs(vp, (caddr_t)a[1]);
	VN_RELE(vp);
	return e;
}

int
aux_fstatfs(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	file_t *fp;
	int e;

	if ((e = getf((int)a[0], &fp)) != 0)
		return e;
	return vstatfs(fp->f_vnode, (caddr_t)a[1]);
}

/* ---- truncate(path, len), ftruncate(fd, len): F_FREESP from len ---- */

static int
settrunc(vp, len, flag, cr)
	struct vnode *vp;
	long len;
	int flag;
	cred_t *cr;
{
	struct flock bf;

	if (len < 0)
		return EINVAL;
	if (vp->v_type == VDIR)
		return EISDIR;
	if (vp->v_type != VREG)
		return EINVAL;
	bzero((caddr_t)&bf, sizeof bf);
	bf.l_start = len;
	return VOP_SPACE(vp, F_FREESP, &bf, flag, (off_t)0, cr);
}

int
aux_truncate(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	struct vnode *vp;
	int e;

	if ((e = lookupname((caddr_t)a[0], UIO_USERSPACE, FOLLOW, NULLVPP, &vp)) != 0)
		return e;
	if ((e = VOP_ACCESS(vp, VWRITE, 0, u.u_cred)) == 0)
		e = settrunc(vp, a[1], FREAD | FWRITE, u.u_cred);
	VN_RELE(vp);
	return e;
}

int
aux_ftruncate(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	file_t *fp;
	int e;

	if ((e = getf((int)a[0], &fp)) != 0)
		return e;
	if (!(fp->f_flag & FWRITE))
		return EINVAL;
	return settrunc(fp->f_vnode, a[1], fp->f_flag, fp->f_cred);
}

/* utimes(path, tv[2]): AMIX utime with whole seconds */
int
aux_utimes(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	long tv[4], ut[2];
	caddr_t g;

	if (a[1]) {
		if (copyin((caddr_t)a[1], (caddr_t)tv, sizeof tv))
			return EFAULT;
		ut[0] = tv[0];
		ut[1] = tv[2];
		g = aux_gap(r, sizeof ut);
		if (copyout((caddr_t)ut, g, sizeof ut))
			return EFAULT;
		a[1] = (long)g;
	}
	return aux_amix(30, a, rv);
}

/* ---- itimers: ITIMER_REAL at clock-tick resolution ---- */

/*
 * One callout per armed timer.  timeout() panics on a full table, so
 * these use ttimeout() and at most a quarter of the table.
 */

static int aux_nit;		/* armed timers */
static void itexpire();

/* arm ap's timer for t ticks, at splhi; 0 if no callout is free */
static int
itarm(ap, gp, t)
	struct aux_proc *ap;
	struct guest_proc *gp;
	long t;
{
	int id;

	if (aux_nit >= v.v_call / 4 ||
	    (id = ttimeout(itexpire, (caddr_t)gp, t)) == -1)
		return 0;
	aux_nit++;
	ap->ap_itexp = lbolt + t;
	ap->ap_itid = id;
	return 1;
}

static long
tv2tick(tv)
	long *tv;
{
	long t;

	if (tv[0] > 0x7fffffff / HZ - 1)
		return 0x7fffffff;
	t = tv[0] * HZ + (tv[1] * HZ + 999999) / 1000000;
	return t == 0 && (tv[0] || tv[1]) ? 1 : t;
}

static void
tick2tv(t, tv)
	long t, *tv;
{
	tv[0] = t / HZ;
	tv[1] = (t % HZ) * 1000000 / HZ;
}

static void
itexpire(gp)
	struct guest_proc *gp;
{
	struct aux_proc *ap = AUXP(gp);

	psignal(gp->gp_proc, SIGALRM);
	ap->ap_itid = 0;
	aux_nit--;
	if (ap->ap_itint)
		(void)itarm(ap, gp, ap->ap_itint);
}

void
aux_itstop(ap)
	struct aux_proc *ap;
{
	int s = splhi_();

	if (ap->ap_itid) {
		untimeout(ap->ap_itid);
		ap->ap_itid = 0;
		aux_nit--;
	}
	splx_(s);
}

/* the current itimerval of ITIMER_REAL into v[4] */
static void
itget(ap, v)
	struct aux_proc *ap;
	long *v;
{
	long left = 0, it = 0;
	int s = splhi_();

	if (ap->ap_itid) {
		left = ap->ap_itexp - lbolt;
		if (left <= 0)
			left = 1;
		it = ap->ap_itint;
	}
	splx_(s);
	tick2tv(it, v);
	tick2tv(left, v + 2);
}

int
aux_getitimer(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	long v[4];

	if (a[0] < 0 || a[0] > 2)
		return EINVAL;
	bzero((caddr_t)v, sizeof v);
	if (a[0] == 0)
		itget(ap, v);
	return copyout((caddr_t)v, (caddr_t)a[1], sizeof v) ? EFAULT : 0;
}

/* ITIMER_VIRTUAL and ITIMER_PROF read as zero and can only be cleared */
int
aux_setitimer(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	long v[4], o[4], it, t;
	int s, e;

	if (a[0] < 0 || a[0] > 2)
		return EINVAL;
	if (a[1] == 0)
		bzero((caddr_t)v, sizeof v);
	else if (copyin((caddr_t)a[1], (caddr_t)v, sizeof v))
		return EFAULT;
	if (v[0] < 0 || v[1] < 0 || v[1] >= 1000000 ||
	    v[2] < 0 || v[3] < 0 || v[3] >= 1000000)
		return EINVAL;
	bzero((caddr_t)o, sizeof o);
	if (a[0] != 0) {
		if (v[2] || v[3])
			return EINVAL;
	} else {
		itget(ap, o);
		aux_itstop(ap);
		t = tv2tick(v + 2);
		it = tv2tick(v);
		if (t) {
			s = splhi_();
			ap->ap_itint = it;
			e = itarm(ap, GUESTP(u.u_procp), t);
			splx_(s);
			if (!e)
				return EAGAIN;
		}
	}
	if (a[2] && copyout((caddr_t)o, (caddr_t)a[2], sizeof o))
		return EFAULT;
	return 0;
}

/*
 * setreuid(ruid, euid), setregid(rgid, egid): BSD rules, -1 keeps.  The
 * saved id follows the new effective one when the real id is given or
 * the effective one leaves it, so setreuid(r, r) drops privilege for good.
 */

static int
setre(a, gid)
	long *a;
	int gid;
{
	cred_t *cr = u.u_cred;
	long r = a[0], e = a[1], or, oe, os;
	int sv;

	or = gid ? cr->cr_rgid : cr->cr_ruid;
	oe = gid ? cr->cr_gid : cr->cr_uid;
	os = gid ? cr->cr_sgid : cr->cr_suid;
	sv = r != -1 || (e != -1 && e != or);
	if (r == -1)
		r = or;
	if (e == -1)
		e = oe;
	if (r < 0 || r > MAXUID || e < 0 || e > MAXUID)
		return EINVAL;
	if (((r != or && r != oe) || (e != or && e != oe && e != os)) &&
	    !suser(cr))
		return EPERM;
	cr = u.u_cred = crcopy(cr);
	if (gid) {
		if (sv)
			cr->cr_sgid = e;
		cr->cr_rgid = r;
		cr->cr_gid = e;
	} else {
		if (sv)
			cr->cr_suid = e;
		cr->cr_ruid = r;
		cr->cr_uid = e;
	}
	return 0;
}

int
aux_setreuid(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return setre(a, 0);
}

int
aux_setregid(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return setre(a, 1);
}

/* ---- shmsys(op, ...): 0 shmat, 1 shmctl, 2 shmdt, 3 shmget ---- */

#define	ASHM_SIZE	44

/* shmctl(id, cmd, buf): IPC_RMID/SET/STAT 0/1/2 are 10/11/12 on AMIX */
static int
shmctl_(a, rv, r)
	long *a;
	rval_t *rv;
	char *r;
{
	struct shmid_ds ds;
	char b[ASHM_SIZE];
	caddr_t ub = (caddr_t)a[3], g;
	long cmd = a[2];
	int e;

	if (cmd == SHM_LOCK || cmd == SHM_UNLOCK)
		return aux_amix(52, a, rv);
	if (cmd == 0) {
		a[2] = IPC_RMID;
		return aux_amix(52, a, rv);
	}
	if (cmd != 1 && cmd != 2)
		return EINVAL;
	g = aux_gap(r, sizeof ds);
	a[2] = IPC_STAT;
	a[3] = (long)g;
	if ((e = aux_amix(52, a, rv)) != 0)
		return e;
	if (copyin(g, (caddr_t)&ds, sizeof ds))
		return EFAULT;
	if (cmd == 1) {
		if (copyin(ub, b, sizeof b))
			return EFAULT;
		ds.shm_perm.uid = G16(b);
		ds.shm_perm.gid = G16(b + 2);
		ds.shm_perm.mode = (ds.shm_perm.mode & ~0777) | (G16(b + 8) & 0777);
		if (copyout((caddr_t)&ds, g, sizeof ds))
			return EFAULT;
		a[2] = IPC_SET;
		return aux_amix(52, a, rv);
	}
	bzero(b, sizeof b);
	P16(b, ds.shm_perm.uid);
	P16(b + 2, ds.shm_perm.gid);
	P16(b + 4, ds.shm_perm.cuid);
	P16(b + 6, ds.shm_perm.cgid);
	P16(b + 8, ds.shm_perm.mode);
	P16(b + 10, ds.shm_perm.seq);
	P32(b + 12, ds.shm_perm.key);
	P32(b + 16, ds.shm_segsz);
	P16(b + 24, ds.shm_lpid);
	P16(b + 26, ds.shm_cpid);
	P16(b + 28, ds.shm_nattch);
	P16(b + 30, ds.shm_cnattch);
	P32(b + 32, ds.shm_atime);
	P32(b + 36, ds.shm_dtime);
	P32(b + 40, ds.shm_ctime);
	return copyout(b, ub, sizeof b) ? EFAULT : 0;
}

int
aux_shmsys(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	switch (a[0]) {
	case 0: case 2: case 3:
		return aux_amix(52, a, rv);
	case 1:
		return shmctl_(a, rv, r);
	}
	return EINVAL;
}

/* sigpending(set): blocked pending signals, A/UX mask */
int
aux_sigpending(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	struct proc *p = u.u_procp;
	long m = aux_mask_out((u_long)(p->p_sig & p->p_hold));

	return copyout((caddr_t)&m, (caddr_t)a[0], sizeof m) ? EFAULT : 0;
}

/* ---- pty packet mode (TIOCPKT): pckt on the master ---- */

static char *
strtop(fp)
	file_t *fp;
{
	struct stdata *st = fp->f_vnode->v_stream;
	queue_t *q;

	if (st == 0 || (q = st->sd_wrq->q_next) == 0)
		return "";
	return q->q_qinfo->qi_minfo->mi_idname;
}

static char *
strbottom(fp)
	file_t *fp;
{
	struct stdata *st = fp->f_vnode->v_stream;
	queue_t *q;

	if (st == 0)
		return "";
	for (q = st->sd_wrq; q->q_next; q = q->q_next)
		;
	return q->q_qinfo->qi_minfo->mi_idname;
}

/* TIOCPKT: on a pty master, push or pop pckt; one message per read */
int
aux_tiocpkt(fd, arg)
	int fd;
	caddr_t arg;
{
	file_t *fp;
	int e, on, rval, pk;

	if ((e = getf(fd, &fp)) != 0)
		return e;
	if (strcmp(strbottom(fp), "ptm") != 0)
		return ENOTTY;
	if (copyin(arg, (caddr_t)&on, sizeof on))
		return EFAULT;
	pk = strcmp(strtop(fp), "pckt") == 0;
	if (on && !pk) {
		if ((e = strioctl(fp->f_vnode, I_PUSH, (int)"pckt", fp->f_flag,
		    K_TO_K, fp->f_cred, &rval)) != 0)
			return e;
		return strioctl(fp->f_vnode, I_SRDOPT, RMSGN | RPROTDAT, fp->f_flag,
		    K_TO_K, fp->f_cred, &rval);
	}
	if (!on && pk) {
		if ((e = strioctl(fp->f_vnode, I_POP, 0, fp->f_flag, K_TO_K,
		    fp->f_cred, &rval)) != 0)
			return e;
		return strioctl(fp->f_vnode, I_SRDOPT, RNORM, fp->f_flag,
		    K_TO_K, fp->f_cred, &rval);
	}
	return 0;
}

/*
 * read(): on a master in packet mode each message arrives as its
 * STREAMS type byte and body.  Data keeps its 0 byte (TIOCPKT_DATA);
 * flush, stop and start become the one BSD control byte; others are
 * skipped.
 */
int
aux_read(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	file_t *fp;
	long b[3];
	int e, c, f;

	if (getf((int)a[0], &fp) || fp->f_vnode->v_stream == 0 ||
	    strcmp(strtop(fp), "pckt") != 0)
		return aux_amix(3, a, rv);
	bcopy((caddr_t)a, (caddr_t)b, sizeof b);
	for (;;) {
		if ((e = aux_amix(3, a, rv)) != 0 || rv->r_val1 == 0)
			return e;
		if ((c = fubyte((caddr_t)b[1])) == M_DATA)
			return 0;
		f = rv->r_val1 > 1 ? fubyte((caddr_t)b[1] + 1) : 0;
		if (c == M_FLUSH)
			c = f & (FLUSHR | FLUSHW);	/* TIOCPKT_FLUSHREAD 1, FLUSHWRITE 2 */
		else if (c == M_STOP)
			c = 4;
		else if (c == M_START)
			c = 8;
		else
			c = 0;
		if (c) {
			rv->r_val1 = 1;
			return subyte((caddr_t)b[1], c) ? EFAULT : 0;
		}
		bcopy((caddr_t)b, (caddr_t)a, sizeof b);
	}
}
