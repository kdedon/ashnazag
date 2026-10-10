/*
 * auxfid.c -- fidop, the File ID daemon's message queues, and csop,
 * the catalog-search daemon's, which answers as with no daemon.
 *
 *	Mac side --1 send--> requests --2 receive--> fidd
 *	Mac side <--4 receive-- replies <--3 reply-- fidd
 *
 * A message is {type, length, data}; the caller passes a 16-byte
 * header {type, buf, bufsize, len}.  Receives block until a message
 * is queued; a send returns the message's id, which 5 cancels.
 *
 * K&R C.
 */

#include "auxcore.h"
#include "sys/vfs.h"
#include "sys/dirent.h"

#define	FQ_MAX		20		/* messages a queue holds */
#define	FQ_MAXLEN	0x2000		/* bytes a message holds */

struct fidmsg {
	struct fidmsg	*m_next;
	long		m_id;
	long		m_type;
	long		m_len;
	/* m_len bytes follow */
};

struct fidq {
	struct fidmsg	*q_head;	/* oldest first */
	int		q_count;
};

static struct fidq fid_cq, fid_rq;	/* requests, replies */
static long fid_nextid;
static struct proc *fid_server;
static pid_t fid_pid;
static struct vnode *fid_rootdir;
static unsigned long fid_epoch;
static int fid_reserved;
static cred_t fid_root = { 1, 0 };
extern int strlen(), strcmp();
static uid_t fid_uid;			/* the client with a request out */

static void
fq_free(m)
	struct fidmsg *m;
{
	kmem_free((_VOID *)m, sizeof *m + m->m_len);
}

static void
fq_clear(q)
	struct fidq *q;
{
	struct fidmsg *m;

	while ((m = q->q_head) != 0) {
		q->q_head = m->m_next;
		fq_free(m);
	}
	q->q_count = 0;
}

/* queue the caller's message; its id in *idp */
static int
fq_send(q, uh, idp)
	struct fidq *q;
	caddr_t uh;
	long *idp;
{
	long h[4];
	struct fidmsg *m, **pp;

	if (copyin(uh, (caddr_t)h, sizeof h))
		return EFAULT;
	if (h[3] <= 0 || h[2] < h[3])
		return EINVAL;
	if (h[3] > FQ_MAXLEN || q->q_count + (q == &fid_cq ? fid_reserved : 0) >= FQ_MAX)
		return ENOMEM;
	m = (struct fidmsg *)kmem_alloc(sizeof *m + h[3], KM_SLEEP);
	m->m_len = h[3];
	if (copyin((caddr_t)h[1], (caddr_t)(m + 1), (u_int)h[3])) {
		fq_free(m);
		return EFAULT;
	}
	m->m_next = 0;
	m->m_type = h[0];
	m->m_id = *idp = fid_nextid++;
	for (pp = &q->q_head; *pp; pp = &(*pp)->m_next)
		;
	*pp = m;
	q->q_count++;
	wakeup((caddr_t)q);
	return 0;
}

/* wait for the oldest message and hand it over */
static int
fq_recv(q, uh)
	struct fidq *q;
	caddr_t uh;
{
	long h[4];
	struct fidmsg *m;

	if (copyin(uh, (caddr_t)h, sizeof h))
		return EFAULT;
	while (q->q_head == 0)
		if (sleep((caddr_t)q, (PZERO + 1) | PCATCH))
			return EINTR;
	m = q->q_head;
	if (h[2] > 0 && h[2] < m->m_len)
		return EINVAL;
	/* off the queue before copyout can sleep; back at the head on a fault */
	q->q_head = m->m_next;
	q->q_count--;
	h[0] = m->m_type;
	h[3] = m->m_len;
	if ((h[2] > 0 && copyout((caddr_t)(m + 1), (caddr_t)h[1], (u_int)m->m_len)) ||
	    copyout((caddr_t)h, uh, sizeof h)) {
		m->m_next = q->q_head;
		q->q_head = m;
		q->q_count++;
		return EFAULT;
	}
	fq_free(m);
	return 0;
}

static int
fq_cancel(q, id)
	struct fidq *q;
	long id;
{
	struct fidmsg *m, **pp;

	for (pp = &q->q_head; (m = *pp) != 0; pp = &m->m_next)
		if (m->m_id == id) {
			*pp = m->m_next;
			q->q_count--;
			fq_free(m);
			return 0;
		}
	return EINVAL;
}

#define FID_PATH 1024

struct fidrename {
	char oldpath[FID_PATH], newpath[FID_PATH];
	char input[FID_PATH], entries[2048];
	char oldinput[FID_PATH], newinput[FID_PATH];
	struct vnode *held[4];
	struct fidmsg *message;
	int reserved;
};

static void
fid_rele(ctx, n)
	struct fidrename *ctx;
	int n;
{
	struct vnode *vp = ctx->held[n];
	ctx->held[n] = 0;
	if (vp) VN_RELE(vp);
}

/* Match directory entries through vnodes across mounted filesystems. */
static int
fid_dirname(parent, child, name, ctx)
	struct vnode *parent, *child;
	char *name;
	struct fidrename *ctx;
{
	char *work = ctx->entries;
	struct uio io;
	struct iovec vec;
	struct dirent *d;
	struct vnode *vp;
	struct vattr va;
	int e, eof, n, off, pages, z;
	off_t next = 0;

	va.va_mask = AT_NODEID;
	if ((e = VOP_GETATTR(child, &va, 0, &fid_root)) != 0) return e;
	for (pages = 0; pages < 256; pages++) {
		bzero((caddr_t)&io, sizeof io);
		vec.iov_base = work; vec.iov_len = 2048;
		io.uio_iov = &vec; io.uio_iovcnt = 1;
		io.uio_offset = next; io.uio_segflg = UIO_SYSSPACE;
		io.uio_resid = 2048; io.uio_limit = 0x7fffffff;
		eof = 0;
		VOP_RWLOCK(parent);
		e = VOP_READDIR(parent, &io, &fid_root, &eof);
		VOP_RWUNLOCK(parent);
		if (e) return e;
		n = 2048 - io.uio_resid;
		if (n < 0 || n > 2048) return EIO;
		for (off = 0; off < n; off += d->d_reclen) {
			d = (struct dirent *)(work + off);
			if (n - off < 12 || d->d_reclen < 12 ||
			    d->d_reclen > n - off || (d->d_reclen & 3)) return EIO;
			for (z = 0; z < d->d_reclen - 10 && d->d_name[z]; z++) ;
			if (z == d->d_reclen - 10) return EIO;
			if (!d->d_ino || d->d_ino != va.va_nodeid || !z || z >= FID_PATH ||
			    (d->d_name[0] == '.' && (z == 1 ||
			     (z == 2 && d->d_name[1] == '.')))) continue;
			vp = 0;
			if (VOP_LOOKUP(parent, d->d_name, &ctx->held[2], 0, 0, rootdir, &fid_root)) {
				fid_rele(ctx, 2); continue;
			}
			vp = ctx->held[2];
			e = VN_CMP(vp, child);
			fid_rele(ctx, 2);
			if (e) { bcopy(d->d_name, name, z + 1); return 0; }
		}
		if (eof) return ENOENT;
		if (io.uio_offset <= next || !n) return EIO;
		next = io.uio_offset;
	}
	return ENAMETOOLONG;
}

static int
fid_path(up, out, ctx)
	caddr_t up;
	char *out;
	struct fidrename *ctx;
{
	struct vnode *dir = 0, *parent, *covered;
	char name[FID_PATH], *p;
	u_int n;
	int e, len, depth, used;

	n = strlen(up) + 1;
	bcopy(up, ctx->input, n);
	if (n < 2) return ENOENT;
	while (n > 2 && ctx->input[n - 2] == '/') ctx->input[--n - 1] = 0;
	p = ctx->input + n - 2;
	while (p > ctx->input && *p != '/') p--;
	if (*p == '/') {
		bcopy(p + 1, out, n - (p + 1 - ctx->input));
		if (p == ctx->input) p[1] = 0; else *p = 0;
	} else {
		bcopy(ctx->input, out, n);
		ctx->input[0] = '.'; ctx->input[1] = 0;
	}
	used = strlen(out);
	if (!used || (used == 1 && out[0] == '.') ||
	    (used == 2 && out[0] == '.' && out[1] == '.')) return EINVAL;
	if ((e = lookupname(ctx->input, UIO_SYSSPACE, FOLLOW, NULLVPP, &ctx->held[0])) != 0) return e;
	dir = ctx->held[0];
	for (depth = 0; depth < 64; depth++) {
		if (VN_CMP(dir, ctx->held[3])) {
			if (used + 2 > FID_PATH) { e = ENAMETOOLONG; break; }
			for (len = used; len >= 0; len--) out[len + 1] = out[len];
			out[0] = '/'; e = 0; break;
		}
		if (VN_CMP(dir, rootdir)) { e = EXDEV; break; }
		if ((dir->v_flag & VROOT) && dir->v_vfsp &&
		    (covered = dir->v_vfsp->vfs_vnodecovered) != 0) {
			VN_HOLD(covered); ctx->held[1] = covered;
			fid_rele(ctx, 0); dir = covered;
			ctx->held[0] = covered; ctx->held[1] = 0;
		}
		parent = 0;
		e = VOP_LOOKUP(dir, "..", &ctx->held[1], 0, 0, rootdir, &fid_root);
		if (e) break;
		parent = ctx->held[1];
		if (VN_CMP(dir, parent)) { fid_rele(ctx, 1); e = ELOOP; break; }
		e = fid_dirname(parent, dir, name, ctx);
		fid_rele(ctx, 0); dir = parent;
		ctx->held[0] = parent; ctx->held[1] = 0;
		if (e) break;
		len = strlen(name);
		if (used + len + 2 > FID_PATH) { e = ENAMETOOLONG; break; }
		for (n = used + 1; n > 0; n--) out[n + len] = out[n - 1];
		bcopy(name, out, len); out[len] = '/'; used += len + 1;
	}
	if (depth == 64) e = ELOOP;
	fid_rele(ctx, 0);
	return e;
}

extern int aux_amix();

int
aux_rename(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	struct fidrename *ctx;
	struct fidmsg *m, **pp;
	struct vnode *vp;
	label_t saved;
	struct vattr va;
	long ino;
	int e, len, oldlen, newlen;
	u_int copied;
	pid_t server;
	unsigned long epoch;

	if (!fid_server || fid_server == u.u_procp || prfind(fid_pid) != fid_server)
		return aux_amix(134, a, rv);
	if (fid_cq.q_count + fid_reserved >= FQ_MAX) return ENOMEM;
	ctx = (struct fidrename *)kmem_alloc(sizeof *ctx, KM_NOSLEEP);
	if (!ctx) return ENOMEM;
	bzero((caddr_t)ctx->held, sizeof ctx->held);
	ctx->message = 0; ctx->reserved = 0;
	ctx->held[3] = fid_rootdir ? fid_rootdir : rootdir;
	VN_HOLD(ctx->held[3]);
	bcopy((caddr_t)&u.u_qsav, (caddr_t)&saved, sizeof saved);
	if (setjmp(&u.u_qsav)) {
		e = u.u_error & 0xff;
		if (!e) e = EINTR;
		goto out;
	}
	m = 0; server = fid_pid; epoch = fid_epoch;
	e = copyinstr((caddr_t)a[0], ctx->oldinput, FID_PATH, &copied);
	if (!e) e = copyinstr((caddr_t)a[1], ctx->newinput, FID_PATH, &copied);
	if (!e) e = fid_path(ctx->oldinput, ctx->oldpath, ctx);
	if (!e) e = fid_path(ctx->newinput, ctx->newpath, ctx);
	if (e == EXDEV) {
		u.u_syscall = 134;
		e = vn_rename(ctx->oldinput, ctx->newinput, UIO_SYSSPACE);
		goto out;
	}
	if (!e) e = lookupname(ctx->oldinput, UIO_SYSSPACE, NO_FOLLOW, NULLVPP, &ctx->held[2]);
	if (!e) {
		vp = ctx->held[2];
		va.va_mask = AT_NODEID;
		e = VOP_GETATTR(vp, &va, 0, u.u_cred);
		if (!e) ino = va.va_nodeid;
		fid_rele(ctx, 2);
	}
	if (!e) {
		oldlen = strlen(ctx->oldpath) + 1;
		newlen = strlen(ctx->newpath) + 1;
		len = 4 + oldlen + newlen + 1;
		m = (struct fidmsg *)kmem_alloc(sizeof *m + len, KM_NOSLEEP);
		ctx->message = m;
		if (!m) e = ENOMEM;
		else {
			m->m_next = 0; m->m_id = -1; m->m_type = 1; m->m_len = len;
			P32(m + 1, ino);
			bcopy(ctx->oldpath, (char *)(m + 1) + 4, oldlen);
			bcopy(ctx->newpath, (char *)(m + 1) + 4 + oldlen, newlen);
			*((char *)(m + 1) + len - 1) = 0;
		}
	}
	if (!e && fid_cq.q_count + fid_reserved >= FQ_MAX) e = ENOMEM;
	if (!e) {
		fid_reserved++; ctx->reserved = 1;
		u.u_syscall = 134;
		e = vn_rename(ctx->oldinput, ctx->newinput, UIO_SYSSPACE);
		if (!e && epoch == fid_epoch && server == fid_pid && prfind(server) == fid_server) {
			for (pp = &fid_cq.q_head; *pp; pp = &(*pp)->m_next) ;
			*pp = m; ctx->message = 0; fid_cq.q_count++;
			wakeup((caddr_t)&fid_cq);
		}
	}
out:
	if (ctx->reserved) { fid_reserved--; ctx->reserved = 0; }
	fid_rele(ctx, 0); fid_rele(ctx, 1); fid_rele(ctx, 2); fid_rele(ctx, 3);
	bcopy((caddr_t)&saved, (caddr_t)&u.u_qsav, sizeof saved);
	if (ctx->message) fq_free(ctx->message);
	kmem_free(ctx, sizeof *ctx);
	return e;
}

static int
fid_clientbusy()
{
	struct fidmsg *m;
	for (m = fid_cq.q_head; m; m = m->m_next)
		if (m->m_id >= 0) return 1;
	return 0;
}

static void
fid_dropsilent()
{
	struct fidmsg *m, **pp = &fid_cq.q_head;
	while ((m = *pp) != 0) {
		if (m->m_id < 0) {
			*pp = m->m_next; fid_cq.q_count--; fq_free(m);
		} else pp = &m->m_next;
	}
}

static void
fid_register()
{
	struct vnode *old, *next;

	if (fid_server != u.u_procp || fid_pid != u.u_procp->p_pid ||
	    !VN_CMP(fid_rootdir, u.u_rdir ? u.u_rdir : rootdir)) {
		fid_epoch++; fid_dropsilent();
		next = u.u_rdir ? u.u_rdir : rootdir;
		VN_HOLD(next); old = fid_rootdir; fid_rootdir = next;
		fid_server = u.u_procp; fid_pid = u.u_procp->p_pid;
		if (old) VN_RELE(old);
	}
	fid_server = u.u_procp; fid_pid = u.u_procp->p_pid;
}

void
aux_fidexit(p)
	struct proc *p;
{
	struct vnode *old;

	if (p != fid_server || p->p_pid != fid_pid) return;
	old = fid_rootdir; fid_rootdir = 0;
	fid_server = 0; fid_pid = 0; fid_epoch++;
	fid_dropsilent();
	if (old) VN_RELE(old);
}

/* fidop(op, hdr): 1 send, 2 serve, 3 reply, 4 take reply, 5 cancel */
int
aux_fidop(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	int e;

	switch (a[0]) {
	case 1: case 4: case 5:
		/* the client's own queue: another user waits until it is empty */
		if ((fid_clientbusy() || fid_rq.q_count) && u.u_cred->cr_uid != fid_uid &&
		    !suser(u.u_cred))
			return EPERM;
		break;
	case 2: case 3:
		/* the daemon's side: root */
		if (!suser(u.u_cred))
			return EPERM;
		break;
	}
	switch (a[0]) {
	case 1:
		/* one client: a new request drops stale replies */
		fq_clear(&fid_rq);
		if ((e = fq_send(&fid_cq, (caddr_t)a[1], &rv->r_val1)) == 0)
			fid_uid = u.u_cred->cr_uid;
		return e;
	case 2:
		fid_register();
		return fq_recv(&fid_cq, (caddr_t)a[1]);
	case 3:
		return fq_send(&fid_rq, (caddr_t)a[1], &rv->r_val1);
	case 4:
		return fq_recv(&fid_rq, (caddr_t)a[1]);
	case 5:
		if (a[1] < 0) return EINVAL;
		return fq_cancel(&fid_cq, a[1]);
	}
	return EINVAL;
}

/*
 * csop(sel, buf, len): no catalog-search daemon.  "Ready" and its pid
 * read 0, so the Mac side searches by itself; requests fail.
 */
int
aux_csop(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	switch (a[0]) {
	case 8: case 10:		/* GET_READY, GET_PID */
		rv->r_val1 = 0;
		return 0;
	case 1:				/* SEND_REQUEST */
		return AUXE_AGAIN;
	case 6:				/* POLL_REPLY */
		return AUXE_WOULDBLOCK;
	}
	return EINVAL;
}

void
aux_fidfree()
{
	struct vnode *old = fid_rootdir;

	fid_rootdir = 0;
	fid_server = 0; fid_pid = 0; fid_epoch++;
	fq_clear(&fid_cq);
	fq_clear(&fid_rq);
	if (old) VN_RELE(old);
}
