/*
 * hsock.c -- sockets as libsocket builds them: a transport device with
 * sockmod pushed, driven by I_STR, putmsg and getmsg.  The calls run as
 * the caller's own system calls on scratch below its stack, so blocking,
 * signals, O_NDELAY and SIGPOLL registration behave as for any stream.
 *
 * K&R C.
 */

#include "kinc.h"
#include "sys/vnode.h"
#include "sys/file.h"
#include "sys/stream.h"
#include "sys/strsubr.h"
#include "sys/stropts.h"
#include "sys/tihdr.h"
#include "sys/timod.h"
#include "sys/poll.h"
#include "hsock.h"

/* user scratch: control structures, ioctl and control part, discard buffer */
#define	G_SIO	0
#define	G_CTL	64
#define	HS_CTL	256
#define	G_DAT	320

#define	TSYSERR		8
#define	T_CLTS		3
#define	SI_GETUDATA	(('I' << 8) | 101)
#define	SI_SHUTDOWN	(('I' << 8) | 102)
#define	SI_LISTEN	(('I' << 8) | 103)
#define	SI_SETPEERNAME	(('I' << 8) | 105)
#define	AF_INET		2
#define	SOCK_DGRAM	1
#define	SOCK_STREAM	2
#define	MSG_OOB		1
#define	MSG_PEEK	2
#define	SOL_SOCKET	0xffff
#define	SO_TYPE		0x1008
#define	SO_ERROR	0x1007
#define	SO_PROTOTYPE	0x1009
#define	T_NEGOTIATE	4
#define	T_CHECK		8
#define	SS_ISCONNECTED	0x002
#define	SS_ISBOUND	0x080
#define	FIONBIO		0x8004667e
#define	FIOASYNC	0x8004667d
#define	FIONREAD	0x4004667f
#define	SIOCSPGRP	0x80047308
#define	SIOCGPGRP	0x40047309
#define	SIOCGIFCONF	0xc0086914
#define	AMIX_IOCTL	54
#define	AMIX_GETMSG	85
#define	AMIX_PUTMSG	86
#define	AMIX_POLL	87
#define	IPPORT_RESERVED	1024
#define	SM_UDATA	104	/* si_udata in sockmod's per-stream data */

#undef	splstr
extern file_t *file;
extern int splstr(), drv_priv();

/* a nonblocking connect in progress, or a socket whose peer released */
#define	HS_NST	16
static struct hsst {
	file_t	*s_fp;
	struct stdata *s_sd;
	int	s_err;
	char	s_conn;
	char	s_eof;
} hst[HS_NST];

static int
hsys(n, a0, a1, a2, a3, rp)
	int n;
	long a0, a1, a2, a3, *rp;
{
	long a[6];
	rval_t rv;
	int e;

	a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; a[4] = a[5] = 0;
	rv.r_val1 = rv.r_val2 = 0;
	u.u_syscall = n;
	e = (*sysent[n].sy_call)(a, &rv);
	if (rp)
		*rp = rv.r_val1;
	return e;
}

static int
put(g, k, n)
	caddr_t g, k;
	int n;
{
	return n > 0 && copyout(k, g, (u_int)n) ? EFAULT : 0;
}

static int
get(g, k, n)
	caddr_t g, k;
	int n;
{
	return n > 0 && copyin(g, k, (u_int)n) ? EFAULT : 0;
}

static int
tlierr(t, ux)
	int t, ux;
{
	static short map[] = {
		EPROTO, EADDRNOTAVAIL, EINVAL, EACCES, EBADF, EADDRNOTAVAIL,
		EINVAL, EINVAL, EPROTO, EPROTO, EMSGSIZE, ENOBUFS, EAGAIN,
		EAGAIN, EPROTO, EPROTO, EINVAL, EPROTO, EOPNOTSUPP, EAGAIN
	};

	if (t == TSYSERR)
		return ux ? ux : EPROTO;
	return t > 0 && t < sizeof map / sizeof map[0] ? map[t] : EPROTO;
}

/* s's file is still open on the same stream */
static int
live(s)
	struct hsst *s;
{
	file_t *fp;

	for (fp = file; fp; fp = fp->f_next)
		if (fp == s->s_fp)
			return fp->f_vnode && fp->f_vnode->v_stream == s->s_sd;
	return 0;
}

/* fp's entry; with make, a new one, reusing entries of closed files */
static struct hsst *
st(fp, make)
	file_t *fp;
	int make;
{
	struct stdata *sd = fp->f_vnode->v_stream;
	struct hsst *s, *f = 0;

	for (s = hst; s < hst + HS_NST; s++) {
		if (s->s_fp == fp && s->s_sd == sd)
			return s;
		if (s->s_fp == fp)
			s->s_fp = 0;
		if (s->s_fp == 0 && f == 0)
			f = s;
	}
	for (s = hst; make && f == 0 && s < hst + HS_NST; s++)
		if (!live(s))
			f = s;
	if (make && f) {
		bzero((caddr_t)f, sizeof *f);
		f->s_fp = fp;
		f->s_sd = sd;
	}
	return make ? f : 0;
}

/* free s once it holds nothing */
static void
idle(s)
	struct hsst *s;
{
	if (s && !s->s_conn && !s->s_err && !s->s_eof)
		s->s_fp = 0;
}

/*
 * The first message at the stream head without taking it: -1 none,
 * 0 data, -3 empty data (sockmod's EOF), else its primitive (-2 unknown).
 * With dpp, a copy of it.
 */
static long
qfirst(h, dpp)
	struct hs *h;
	mblk_t **dpp;
{
	queue_t *q = RD(h->h_fp->f_vnode->v_stream->sd_wrq);
	mblk_t *mp, *bp;
	long t = -1;
	int s;

	s = splstr();
	if ((mp = q->q_first) != 0) {
		t = -2;
		if (mp->b_datap->db_type == M_DATA)
			for (t = -3, bp = mp; bp; bp = bp->b_cont)
				if (bp->b_wptr > bp->b_rptr)
					t = 0;
		else if (mp->b_wptr - mp->b_rptr >= 4)
			bcopy((caddr_t)mp->b_rptr, (caddr_t)&t, 4);
		if (dpp)
			*dpp = dupmsg(mp);
	}
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
	return t;
}

/* why an empty stream will get no input: -1 hangup, else an error or 0 */
static int
qdead(h)
	struct hs *h;
{
	struct stdata *sd = h->h_fp->f_vnode->v_stream;

	if (sd->sd_flag & STPLEX)
		return EINVAL;
	if (sd->sd_flag & STRDERR)
		return sd->sd_rerror;
	return sd->sd_flag & STRHUP ? -1 : 0;
}

static void
unst(fp)
	file_t *fp;
{
	struct hsst *s = st(fp, 0);

	if (s)
		s->s_fp = 0;
}

int
hs_issock(fp)
	file_t *fp;
{
	struct stdata *sd = fp->f_vnode->v_stream;
	queue_t *q;

	return sd && (q = sd->sd_wrq->q_next) != 0 &&
	    strcmp(q->q_qinfo->qi_minfo->mi_idname, "sockmod") == 0;
}

/* I_STR cmd with in/out data, decoding sockmod's TLI error replies */
static int
kstr(h, cmd, buf, len, olen)
	struct hs *h;
	int cmd, len, *olen;
	caddr_t buf;
{
	long s[4], r;
	int e;

	s[0] = cmd;
	s[1] = -1;
	s[2] = len;
	s[3] = (long)(h->h_g + G_CTL);
	if (len > HS_CTL)
		return EINVAL;
	if ((e = put(h->h_g + G_SIO, (caddr_t)s, sizeof s)) != 0 ||
	    (e = put(h->h_g + G_CTL, buf, len)) != 0 ||
	    (e = hsys(AMIX_IOCTL, (long)h->h_fd, (long)I_STR, (long)(h->h_g + G_SIO), 0L, &r)))
		return e;
	if (r > 0)
		return tlierr((int)(r & 0xff), (int)((r >> 8) & 0xff));
	if (olen == 0)
		return 0;
	if ((e = get(h->h_g + G_SIO, (caddr_t)s, sizeof s)) != 0)
		return e;
	*olen = s[2] < 0 ? 0 : s[2] > *olen ? *olen : s[2];
	return get(h->h_g + G_CTL, buf, *olen);
}

static int
udata(h)
	struct hs *h;
{
	struct stdata *sd = h->h_fp->f_vnode->v_stream;
	int n = sizeof h->h_ud, e, s;

	bzero((caddr_t)h->h_ud, sizeof h->h_ud);
	if ((e = kstr(h, SI_GETUDATA, (caddr_t)h->h_ud, n, &n)) == 0)
		return 0;
	/*
	 * After shutdown(1) the stream refuses ioctls with its write error,
	 * but the socket can still read: take sockmod's copy directly.
	 */
	s = splstr();
	if ((sd->sd_flag & (STRDERR | STWRERR | STPLEX)) == STWRERR &&
	    sd->sd_wrq->q_next && sd->sd_wrq->q_next->q_ptr &&
	    strcmp(sd->sd_wrq->q_next->q_qinfo->qi_minfo->mi_idname, "sockmod") == 0) {
		bcopy(sd->sd_wrq->q_next->q_ptr + SM_UDATA, (caddr_t)h->h_ud, sizeof h->h_ud);
		e = 0;
	}
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
	return e;
}

int
hs_attach(h, fd, g)
	struct hs *h;
	int fd;
	caddr_t g;
{
	int e;

	if ((e = getf(fd, &h->h_fp)) != 0)
		return e;
	if (!hs_issock(h->h_fp))
		return ENOTSOCK;
	h->h_fd = fd;
	h->h_g = g;
	return udata(h);
}

/* getmsg: control part into ctl (*clen in: room, out: length, -1 none), data to ubuf */
static int
getmsg(h, ctl, clen, ubuf, dlen, flags, rp)
	struct hs *h;
	caddr_t ctl, ubuf;
	int *clen, *dlen;
	long flags, *rp;
{
	caddr_t g = h->h_g;
	long b[7];
	int e, room = *clen;

	b[0] = room; b[1] = 0; b[2] = (long)(g + G_CTL);
	b[3] = *dlen; b[4] = 0; b[5] = (long)ubuf;
	b[6] = flags;
	if ((e = put(g + G_SIO, (caddr_t)b, sizeof b)) != 0 ||
	    (e = hsys(AMIX_GETMSG, (long)h->h_fd, (long)g + G_SIO,
	    ubuf ? (long)g + G_SIO + 12 : 0L, (long)g + G_SIO + 24, rp)) != 0 ||
	    (e = get(g + G_SIO, (caddr_t)b, sizeof b)) != 0)
		return e;
	*clen = b[1] > room ? room : b[1];
	*dlen = b[4];
	return get(g + G_CTL, ctl, b[1] > room ? room : (int)b[1]);
}

static int
putmsg(h, ctl, clen, ubuf, dlen)
	struct hs *h;
	caddr_t ctl, ubuf;
	int clen, dlen;
{
	caddr_t g = h->h_g;
	long b[6];
	int e;

	b[0] = 0; b[1] = clen; b[2] = (long)(g + G_CTL);
	b[3] = 0; b[4] = dlen; b[5] = (long)ubuf;
	if ((e = put(g + G_SIO, (caddr_t)b, sizeof b)) != 0 ||
	    (e = put(g + G_CTL, ctl, clen)) != 0)
		return e;
	return hsys(AMIX_PUTMSG, (long)h->h_fd, (long)g + G_SIO,
	    ubuf ? (long)g + G_SIO + 12 : 0L, 0L, (long *)0);
}

/* wait for events on the socket; ms -1 forever */
static int
hpoll(h, ev, ms)
	struct hs *h;
	int ev;
	long ms;
{
	long p[2], n;
	int e;

	p[0] = h->h_fd;
	p[1] = (long)ev << 16;
	if ((e = put(h->h_g + G_SIO, (caddr_t)p, sizeof p)) != 0 ||
	    (e = hsys(AMIX_POLL, (long)h->h_g + G_SIO, 1L, ms, 0L, &n)) != 0)
		return e;
	return n ? 0 : ETIMEDOUT;
}

/* the T_OK_ACK or T_ERROR_ACK for prim, ahead of normal messages */
static int
okack(h)
	struct hs *h;
{
	long a[4], r;
	int e, cl, dl;

	do {
		if ((e = hpoll(h, POLLPRI, 30000L)) != 0)
			return e;
		cl = sizeof a;
		dl = -1;
		a[0] = -1;
		e = getmsg(h, (caddr_t)a, &cl, (caddr_t)0, &dl, (long)RS_HIPRI, &r);
	} while (e == EAGAIN);
	if (e)
		return e;
	if (a[0] == T_OK_ACK)
		return 0;
	if (a[0] == T_ERROR_ACK && cl >= 16)
		return tlierr((int)a[2], (int)a[3]);
	return EPROTO;
}

static int
bindreq(h, cmd, addr, len, conind, ack)
	struct hs *h;
	int cmd, len, conind;
	caddr_t addr, ack;
{
	long b[(16 + HS_ADDR) / 4];
	int n = sizeof b, e;

	if (len < 0 || len > HS_ADDR)
		return EINVAL;
	b[0] = T_BIND_REQ;
	b[1] = len;
	b[2] = len ? 16 : 0;
	b[3] = conind;
	bcopy(addr, (caddr_t)&b[4], (u_int)len);
	if ((e = kstr(h, cmd, (caddr_t)b, 16 + len, &n)) != 0)
		return e;
	if (ack && n >= 16 + 4 && b[1] >= 4 && b[2] >= 16 && b[2] <= n - 4)
		bcopy((caddr_t)b + b[2], ack, 4);
	return 0;
}

static int
isnb(h)
	struct hs *h;
{
	return (h->h_fp->f_flag & (FNONBLOCK | FNDELAY)) != 0;
}

/* finish a nonblocking connect whose answer has arrived */
static int
settle(h)
	struct hs *h;
{
	struct hsst *s = st(h->h_fp, 0);
	long b[7], c[(20 + HS_ADDR) / 4], r;
	int e, cl, dl;

	if (s == 0 || !s->s_conn)
		return 0;
	b[0] = sizeof c; b[1] = 0; b[2] = (long)(h->h_g + G_CTL);
	b[3] = 0; b[4] = 0; b[5] = 0; b[6] = 0;
	if ((e = put(h->h_g + G_SIO, (caddr_t)b, sizeof b)) != 0)
		return e;
	/* a refused connection may leave the stream in error before we see why */
	if ((e = hsys(AMIX_IOCTL, (long)h->h_fd, (long)I_PEEK, (long)h->h_g + G_SIO,
	    0L, &r)) != 0) {
		s->s_conn = 0;
		s->s_err = ECONNREFUSED;
		return 0;
	}
	if (r == 0)
		return 0;
	if ((e = get(h->h_g + G_SIO, (caddr_t)b, sizeof b)) != 0 ||
	    b[1] < 4 || (e = get(h->h_g + G_CTL, (caddr_t)c, 4)) != 0)
		return e;
	if (c[0] != T_CONN_CON && c[0] != T_DISCON_IND)
		return 0;
	cl = sizeof c;
	dl = -1;
	if ((e = getmsg(h, (caddr_t)c, &cl, (caddr_t)0, &dl, 0L, &r)) != 0)
		return e;
	s->s_conn = 0;
	if (c[0] == T_DISCON_IND)
		s->s_err = c[1] <= 0 || c[1] == ENXIO ? ECONNREFUSED : (int)c[1];
	idle(s);
	(void)udata(h);
	return 0;
}

/* a failed nonblocking connect's error, once */
static int
pending(h)
	struct hs *h;
{
	struct hsst *s = st(h->h_fp, 0);
	int e;

	if (s == 0 || s->s_conn || s->s_err == 0)
		return 0;
	e = s->s_err;
	s->s_err = 0;
	idle(s);
	return e;
}

/*
 * A nonblocking connect's state for select: -1 still pending, else the
 * connect's error, 0 when connected or none was pending.
 */
int
hs_connwait(h)
	struct hs *h;
{
	struct hsst *s;
	int e;

	if ((e = settle(h)) != 0)
		return e;
	if ((s = st(h->h_fp, 0)) == 0)
		return 0;
	return s->s_conn ? -1 : s->s_err;
}

/* a nonblocking connect is pending on fp */
int
hs_connecting(fp)
	file_t *fp;
{
	struct hsst *s = st(fp, 0);

	return s && s->s_conn;
}

static int
opendev(g, path, fdp)
	caddr_t g;
	char *path;
	int *fdp;
{
	long fd, r;
	int e;

	if ((e = put(g + G_CTL, path, 12)) != 0)
		return e;
	if ((e = hsys(5, (long)g + G_CTL, 2L, 0L, 0L, &fd)) != 0)
		return e == ENOENT || e == ENXIO || e == ENODEV ? ENETDOWN : e;
	if ((e = put(g + G_CTL, "sockmod", 8)) == 0)
		e = hsys(AMIX_IOCTL, fd, (long)I_PUSH, (long)g + G_CTL, 0L, &r);
	if (e == 0)
		(void)hsys(AMIX_IOCTL, fd, (long)I_SWROPT, (long)SNDZERO, 0L, &r);
	if (e) {
		(void)hsys(6, fd, 0L, 0L, 0L, (long *)0);
		return e;
	}
	*fdp = fd;
	return 0;
}

int
hs_socket(g, af, type, proto, fdp)
	caddr_t g;
	int af, type, proto, *fdp;
{
	struct hs h;
	char *dev;
	long v = proto;
	int e;

	if (af != AF_INET)
		return af == 1 ? EPROTONOSUPPORT : EAFNOSUPPORT;
	if (type == SOCK_STREAM && (proto == 0 || proto == 6))
		dev = "/dev/tcp";
	else if (type == SOCK_DGRAM && (proto == 0 || proto == 17))
		dev = "/dev/udp";
	else if (type == 4)
		dev = proto == 1 ? "/dev/icmp" : "/dev/rawip";
	else
		return type == SOCK_STREAM || type == SOCK_DGRAM ? EPROTONOSUPPORT :
		    ESOCKTNOSUPPORT;
	if ((e = opendev(g, dev, fdp)) != 0)
		return e;
	if ((e = hs_attach(&h, *fdp, g)) == 0 && type == 4 && proto != 1) {
		e = hs_setopt(&h, SOL_SOCKET, SO_PROTOTYPE, (caddr_t)&v, 4);
	}
	if (e) {
		(void)hsys(6, (long)*fdp, 0L, 0L, 0L, (long *)0);
		return e;
	}
	unst(h.h_fp);
	return 0;
}

int
hs_bind(h, addr, len)
	struct hs *h;
	caddr_t addr;
	int len;
{
	char ack[4];
	int e;

	if (h->h_state & SS_ISBOUND)
		return EINVAL;
	if (len >= 4 && addr[1] == AF_INET && (addr[2] || addr[3]) &&
	    ((u_char)addr[2] << 8 | (u_char)addr[3]) < IPPORT_RESERVED &&
	    drv_priv(u.u_cred))
		return EACCES;
	bzero(ack, 4);
	if ((e = bindreq(h, TI_BIND, addr, len, 0, ack)) != 0)
		return e;
	/* a port in use gets another one: undo that */
	if (len >= 4 && (addr[2] || addr[3]) && (ack[2] != addr[2] || ack[3] != addr[3])) {
		long u = T_UNBIND_REQ;

		(void)kstr(h, TI_UNBIND, (caddr_t)&u, 4, (int *)0);
		return EADDRINUSE;
	}
	return udata(h);
}

/*
 * The transport takes the backlog only with a bind: a bound socket is
 * unbound and bound again to its address with it.
 */
int
hs_listen(h, n)
	struct hs *h;
	int n;
{
	static char any[2] = { 0, AF_INET };
	char a[HS_ADDR], ack[4];
	long u = T_UNBIND_REQ;
	int e, len = 2;

	if (h->h_serv == T_CLTS)
		return EOPNOTSUPP;
	if (n < 1)
		n = 1;
	bcopy(any, a, 2);
	if (h->h_state & SS_ISBOUND) {
		len = HS_ADDR;
		if ((e = hs_name(h, 0, a, &len)) != 0 ||
		    (e = kstr(h, TI_UNBIND, (caddr_t)&u, 4, (int *)0)) != 0)
			return e;
	}
	bcopy(a, ack, 4);
	if ((e = bindreq(h, SI_LISTEN, a, len, n, ack)) != 0 &&
	    (e = bindreq(h, TI_BIND, a, len, n, ack)) != 0)
		return e;
	/* another socket took the port while it was unbound */
	if (len >= 4 && (a[2] || a[3]) && (ack[2] != a[2] || ack[3] != a[3])) {
		(void)kstr(h, TI_UNBIND, (caddr_t)&u, 4, (int *)0);
		(void)udata(h);
		return EADDRINUSE;
	}
	return udata(h);
}

int
hs_connect(h, addr, len)
	struct hs *h;
	caddr_t addr;
	int len;
{
	long b[(20 + HS_ADDR) / 4], r;
	struct hsst *s;
	int e, cl, dl;

	if (len < 16 || len > HS_ADDR)
		return EINVAL;
	bzero(addr + 8, 8);
	if (h->h_serv == T_CLTS) {
		if (!(h->h_state & SS_ISBOUND) &&
		    (e = bindreq(h, TI_BIND, (caddr_t)0, 0, 0, (caddr_t)0)) != 0)
			return e;
		return kstr(h, SI_SETPEERNAME, addr, len, (int *)0);
	}
	if ((e = settle(h)) != 0)
		return e;
	if ((s = st(h->h_fp, 0)) != 0 && s->s_conn)
		return EALREADY;
	if (s && s->s_err) {
		e = s->s_err;
		s->s_err = 0;
		idle(s);
		return e;
	}
	if (h->h_state & SS_ISCONNECTED)
		return EISCONN;
	if (!(h->h_state & SS_ISBOUND) &&
	    (e = bindreq(h, TI_BIND, (caddr_t)0, 0, 0, (caddr_t)0)) != 0)
		return e;
	s = 0;
	if (isnb(h) && (s = st(h->h_fp, 1)) == 0)
		return ENOBUFS;
	b[0] = T_CONN_REQ; b[1] = len; b[2] = 20; b[3] = 0; b[4] = 0;
	bcopy(addr, (caddr_t)&b[5], (u_int)len);
	/* sockmod reports a refusal as a stream error */
	if ((e = putmsg(h, (caddr_t)b, 20 + len, (caddr_t)0, -1)) == 0 &&
	    (e = okack(h)) != 0 && qdead(h) > 0)
		e = ECONNREFUSED;
	if (e) {
		idle(s);
		return e;
	}
	if (s) {
		s->s_conn = 1;
		return EINPROGRESS;
	}
	for (;;) {
		cl = sizeof b;
		dl = -1;
		if ((e = getmsg(h, (caddr_t)b, &cl, (caddr_t)0, &dl, 0L, &r)) != 0)
			return e != EINTR && qdead(h) > 0 ? ECONNREFUSED : e;
		if (cl >= 4 && b[0] == T_CONN_CON)
			return udata(h);
		if (cl >= 8 && b[0] == T_DISCON_IND)
			return b[1] <= 0 || b[1] == ENXIO ? ECONNREFUSED : (int)b[1];
	}
}

int
hs_accept(h, fdp, addr, lenp)
	struct hs *h;
	int *fdp, *lenp;
	caddr_t addr;
{
	long b[(24 + HS_ADDR) / 4], c[5], f[9], r, sig;
	struct hs n;
	int e, cl, dl, alen;

	if (h->h_serv == T_CLTS)
		return EOPNOTSUPP;
	for (;;) {
		cl = sizeof b;
		dl = -1;
		if ((e = getmsg(h, (caddr_t)b, &cl, (caddr_t)0, &dl, 0L, &r)) != 0)
			return e;
		if (cl >= 24 && b[0] == T_CONN_IND)
			break;
	}
	alen = b[1] > 0 && b[2] >= 24 && b[2] <= cl && b[1] <= cl - b[2] ? b[1] : 0;
	if ((e = opendev(h->h_g, "/dev/tcp", fdp)) != 0)
		return e;
	if ((e = hs_attach(&n, *fdp, h->h_g)) != 0 ||
	    (e = bindreq(&n, TI_BIND, (caddr_t)0, 0, 0, (caddr_t)0)) != 0)
		goto bad;
	c[0] = T_CONN_RES; c[1] = 0; c[2] = 0; c[3] = 0; c[4] = b[5];
	f[0] = 0; f[1] = sizeof c; f[2] = (long)(h->h_g + G_CTL);
	f[3] = 0; f[4] = -1; f[5] = 0;
	f[6] = 0; f[7] = *fdp; f[8] = 4;
	if ((e = put(h->h_g + G_CTL, (caddr_t)c, sizeof c)) != 0 ||
	    (e = put(h->h_g + G_SIO, (caddr_t)f, sizeof f)) != 0 ||
	    (e = hsys(AMIX_IOCTL, (long)h->h_fd, (long)I_FDINSERT, (long)h->h_g + G_SIO,
	    0L, &r)) != 0 || (e = okack(h)) != 0)
		goto bad;
	if (alen)
		(void)kstr(&n, SI_SETPEERNAME, (caddr_t)b + b[2], alen, (int *)0);
	/* inherits nonblocking and async notification, as BSD's sonewconn */
	n.h_fp->f_flag |= h->h_fp->f_flag & (FNONBLOCK | FNDELAY);
	if (hsys(AMIX_IOCTL, (long)h->h_fd, (long)I_GETSIG, (long)h->h_g + G_SIO, 0L, &r) == 0 &&
	    get(h->h_g + G_SIO, (caddr_t)&sig, 4) == 0)
		(void)hsys(AMIX_IOCTL, (long)*fdp, (long)I_SETSIG, sig, 0L, &r);
	unst(n.h_fp);
	if (alen > *lenp)
		alen = *lenp;
	bcopy((caddr_t)b + b[2], addr, (u_int)alen);
	*lenp = alen;
	return 0;
bad:
	(void)hsys(6, (long)*fdp, 0L, 0L, 0L, (long *)0);
	return e;
}

int
hs_send(h, ubuf, len, flags, to, tolen, np)
	struct hs *h;
	caddr_t ubuf, to;
	int len, flags, tolen, *np;
{
	long b[(20 + HS_ADDR) / 4], r;
	int e;

	if ((e = settle(h)) != 0 || (e = pending(h)) != 0)
		return e;
	*np = len;
	if (len < 0)
		return EINVAL;
	if (flags & MSG_OOB) {
		b[0] = T_EXDATA_REQ;
		b[1] = 0;
		e = putmsg(h, (caddr_t)b, 8, ubuf, len);
	} else if (h->h_serv == T_CLTS) {
		if (to == 0) {
			tolen = HS_ADDR;
			if (hs_name(h, 1, (caddr_t)&b[5], &tolen) != 0)
				return EDESTADDRREQ;
		} else if (tolen < 0 || tolen > HS_ADDR)
			return EINVAL;
		else
			bcopy(to, (caddr_t)&b[5], (u_int)tolen);
		if (h->h_tidu > 0 && len > h->h_tidu)
			return EMSGSIZE;
		b[0] = T_UNITDATA_REQ; b[1] = tolen; b[2] = 20; b[3] = 0; b[4] = 0;
		e = putmsg(h, (caddr_t)b, 20 + tolen, ubuf, len);
	} else if (len == 0)
		return 0;
	else if ((e = hsys(4, (long)h->h_fd, (long)ubuf, (long)len, 0L, &r)) == 0)
		*np = r;
	if (e == ENXIO || e == EPIPE) {
		psignal(u.u_procp, SIGPIPE);
		e = EPIPE;
	}
	return e;
}

int
hs_recv(h, ubuf, len, flags, from, fromlen, np)
	struct hs *h;
	caddr_t ubuf, from;
	int len, flags, *fromlen, *np;
{
	long b[(24 + HS_ADDR) / 4], r, pk[2], t;
	struct hsst *s;
	mblk_t *dp, *mp;
	int e, cl, dl, n, alen = 0, waited = 0;

	if ((e = settle(h)) != 0 || (e = pending(h)) != 0)
		return e;
	if (flags & MSG_OOB)
		return EINVAL;
	if ((s = st(h->h_fp, 0)) != 0 && s->s_eof) {
		*np = 0;
		return 0;
	}
	for (;;) {
		/* peer's release stays queued: EOF on every later call */
		t = qfirst(h, (mblk_t **)0);
		if (t == T_ORDREL_IND || (t == -3 && h->h_serv != T_CLTS)) {
			*np = 0;
			return 0;
		}
		if (t == -1 && ((flags & MSG_PEEK) ||
		    (!waited && !isnb(h) && h->h_serv != T_CLTS))) {
			if ((e = qdead(h)) != 0) {
				*np = 0;
				return e < 0 ? 0 : e;
			}
			if (isnb(h) || ((flags & MSG_PEEK) && waited))
				return EAGAIN;
			/* wait for a message, leaving it queued */
			cl = dl = -1;
			if ((e = getmsg(h, (caddr_t)pk, &cl, (caddr_t)0, &dl, 0L, &r)) != 0)
				return e;
			waited = 1;
			continue;
		}
		waited = 0;
		cl = sizeof b;
		dl = len;
		b[0] = -1;
		if (flags & MSG_PEEK) {
			/* from the queue: the stream may be shut for writing */
			dp = 0;
			if ((t = qfirst(h, &dp)) == -1)
				continue;
			if (dp == 0)
				return ENOSR;
			mp = dp;
			cl = -1;
			if (mp->b_datap->db_type != M_DATA) {
				cl = mp->b_wptr - mp->b_rptr;
				if (cl > sizeof b)
					cl = sizeof b;
				bcopy((caddr_t)mp->b_rptr, (caddr_t)b, (u_int)cl);
				mp = mp->b_cont;
			}
			for (dl = 0; mp && dl < len && e == 0; mp = mp->b_cont) {
				n = mp->b_wptr - mp->b_rptr;
				if (n > len - dl)
					n = len - dl;
				if (n > 0 && copyout((caddr_t)mp->b_rptr, ubuf + dl, (u_int)n))
					e = EFAULT;
				else if (n > 0)
					dl += n;
			}
			freemsg(dp);
			if (e)
				return e;
			if (cl > 0 && t != T_DATA_IND && t != T_EXDATA_IND &&
			    t != T_UNITDATA_IND && t != T_DISCON_IND) {
				/* nothing a reader wants: drop it, as a plain recv would */
				cl = sizeof b;
				dl = -1;
				if ((e = getmsg(h, (caddr_t)b, &cl, (caddr_t)0, &dl, 0L, &r)) != 0)
					return e;
				continue;
			}
		} else if ((e = getmsg(h, (caddr_t)b, &cl, ubuf, &dl, 0L, &r)) != 0)
			return e;
		else if (h->h_serv == T_CLTS)
			while (r & MOREDATA) {
				int c2 = -1, d2 = HS_CTL;

				if ((e = getmsg(h, (caddr_t)pk, &c2, h->h_g + G_DAT, &d2,
				    0L, &r)) != 0)
					return e;
			}
		if (cl <= 0)
			break;
		if (b[0] == T_DATA_IND || b[0] == T_EXDATA_IND)
			break;
		if (b[0] == T_UNITDATA_IND) {
			if (b[1] > 0 && b[2] >= 20 && b[2] <= cl && b[1] <= cl - b[2])
				alen = b[1];
			break;
		}
		if (b[0] == T_ORDREL_IND || b[0] == T_DISCON_IND) {
			if ((s = st(h->h_fp, 1)) != 0)
				s->s_eof = 1;
			if (b[0] == T_DISCON_IND)
				return ECONNRESET;
			dl = 0;
			break;
		}
	}
	*np = dl < 0 ? 0 : dl > len ? len : dl;
	if (fromlen) {
		if (alen > *fromlen)
			alen = *fromlen;
		if (alen)
			bcopy((caddr_t)b + b[2], from, (u_int)alen);
		*fromlen = alen;
	}
	return 0;
}

int
hs_name(h, peer, addr, lenp)
	struct hs *h;
	int peer, *lenp;
	caddr_t addr;
{
	long nb[3], r;
	int e;

	if (peer && (e = settle(h)) != 0)
		return e;
	nb[0] = HS_ADDR; nb[1] = 0; nb[2] = (long)(h->h_g + G_CTL);
	if ((e = put(h->h_g + G_SIO, (caddr_t)nb, sizeof nb)) != 0)
		return e;
	if ((e = hsys(AMIX_IOCTL, (long)h->h_fd, (long)(peer ? TI_GETPEERNAME : TI_GETMYNAME),
	    (long)h->h_g + G_SIO, 0L, &r)) != 0)
		return peer ? ENOTCONN : e;
	if ((e = get(h->h_g + G_SIO, (caddr_t)nb, sizeof nb)) != 0)
		return e;
	if (peer && nb[1] <= 0)
		return ENOTCONN;
	if (nb[1] < 0)
		nb[1] = 0;
	if (nb[1] < *lenp)
		*lenp = nb[1];
	return get(h->h_g + G_CTL, addr, *lenp);
}

static int
optmgmt(h, flag, level, name, val, lenp)
	struct hs *h;
	int flag, level, name, *lenp;
	caddr_t val;
{
	long b[(28 + HS_ADDR) / 4];
	int n = sizeof b, e;

	if (*lenp < 0 || *lenp > HS_ADDR)
		return EINVAL;
	b[0] = T_OPTMGMT_REQ; b[1] = 12 + *lenp; b[2] = 16; b[3] = flag;
	b[4] = level; b[5] = name; b[6] = *lenp;
	bcopy(val, (caddr_t)&b[7], (u_int)*lenp);
	if ((e = kstr(h, TI_OPTMGMT, (caddr_t)b, 28 + *lenp, &n)) != 0)
		return e == EINVAL || e == EPROTO ? ENOPROTOOPT : e;
	if (flag == T_CHECK) {
		if (n < 28 || b[2] < 16 || b[2] > n - 12 || (b[2] & 3))
			return ENOPROTOOPT;
		n = n - b[2] - 12;
		if (n > b[(b[2] + 8) / 4])
			n = b[(b[2] + 8) / 4];
		if (n < *lenp)
			*lenp = n < 0 ? 0 : n;
		bcopy((caddr_t)b + b[2] + 12, val, (u_int)*lenp);
	}
	return 0;
}

int
hs_getopt(h, level, name, val, lenp)
	struct hs *h;
	int level, name, *lenp;
	caddr_t val;
{
	struct hsst *s;
	long v;
	int e;

	if (level == SOL_SOCKET && (name == SO_TYPE || name == SO_ERROR)) {
		if (*lenp < 4)
			return EINVAL;
		v = h->h_serv == T_CLTS ? SOCK_DGRAM : SOCK_STREAM;
		if (name == SO_ERROR) {
			if ((e = settle(h)) != 0)
				return e;
			v = (s = st(h->h_fp, 0)) != 0 ? s->s_err : 0;
			if (s) {
				s->s_err = 0;
				idle(s);
			}
		}
		bcopy((caddr_t)&v, val, 4);
		*lenp = 4;
		return 0;
	}
	return optmgmt(h, T_CHECK, level, name, val, lenp);
}

int
hs_setopt(h, level, name, val, len)
	struct hs *h;
	int level, name, len;
	caddr_t val;
{
	return optmgmt(h, T_NEGOTIATE, level, name, val, &len);
}

int
hs_shutdown(h, how)
	struct hs *h;
	int how;
{
	long v = how;
	int e;

	if (how < 0 || how > 2)
		return EINVAL;
	if ((e = kstr(h, SI_SHUTDOWN, (caddr_t)&v, 4, (int *)0)) != 0)
		return e;
	(void)udata(h);
	return 0;
}

/* socket ioctls, SVR4 numbering; *rp the call's value */
int
hs_ioctl(h, cmd, uarg, rp)
	struct hs *h;
	int cmd;
	caddr_t uarg;
	long *rp;
{
	long v, r, ifc[2], s[4];
	int e, fd = h->h_fd;
	struct proc *p = u.u_procp;

	*rp = 0;
	switch (cmd) {
	case FIONBIO:
		if (copyin(uarg, (caddr_t)&v, 4))
			return EFAULT;
		if (v)
			h->h_fp->f_flag |= FNONBLOCK;
		else
			h->h_fp->f_flag &= ~(FNONBLOCK | FNDELAY);
		return 0;
	case FIOASYNC:
		if (copyin(uarg, (caddr_t)&v, 4))
			return EFAULT;
		e = hsys(AMIX_IOCTL, (long)fd, (long)I_SETSIG,
		    v ? (long)(S_RDNORM | S_WRNORM | S_RDBAND | S_HANGUP | S_ERROR) : 0L, 0L, &r);
		return v ? e : 0;
	case SIOCSPGRP:
		if (copyin(uarg, (caddr_t)&v, 4))
			return EFAULT;
		if (v != p->p_pid && v != -p->p_pgrp && v != p->p_pgrp)
			return EPERM;
		if (hsys(AMIX_IOCTL, (long)fd, (long)I_GETSIG, (long)h->h_g + G_SIO, 0L, &r) != 0 ||
		    get(h->h_g + G_SIO, (caddr_t)&r, 4) != 0)
			r = 0;
		return hsys(AMIX_IOCTL, (long)fd, (long)I_SETSIG, r | S_RDBAND | S_BANDURG, 0L, &r);
	case SIOCGPGRP:
		v = p->p_pid;
		return copyout((caddr_t)&v, uarg, 4) ? EFAULT : 0;
	case FIONREAD:
		return hsys(AMIX_IOCTL, (long)fd, (long)I_NREAD, (long)uarg, 0L, &r);
	}
	if (((cmd >> 8) & 0xff) != 'i')
		return EINVAL;
	/* interface queries: I_STR to IP below, else a transparent ioctl */
	if (cmd == SIOCGIFCONF) {
		if (copyin(uarg, (caddr_t)ifc, sizeof ifc))
			return EFAULT;
		s[0] = cmd; s[1] = -1; s[2] = ifc[0]; s[3] = ifc[1];
	} else {
		s[0] = cmd; s[1] = -1; s[2] = (cmd >> 16) & 0x7f; s[3] = (long)uarg;
	}
	if ((e = put(h->h_g + G_SIO, (caddr_t)s, sizeof s)) != 0)
		return e;
	e = hsys(AMIX_IOCTL, (long)fd, (long)I_STR, (long)h->h_g + G_SIO, 0L, &r);
	if (e == 0 && r > 0)
		e = tlierr((int)(r & 0xff), (int)((r >> 8) & 0xff));
	if (e == 0 && cmd == SIOCGIFCONF) {
		if ((e = get(h->h_g + G_SIO, (caddr_t)s, sizeof s)) != 0)
			return e;
		ifc[0] = s[2];
		return copyout((caddr_t)ifc, uarg, 4) ? EFAULT : 0;
	}
	if (e == EINVAL)
		e = hsys(AMIX_IOCTL, (long)fd, (long)cmd, (long)uarg, 0L, rp);
	return e;
}
