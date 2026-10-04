/*
 * auxsock.c -- A/UX socket calls 70-93 and socket ioctls on the host
 * socket layer: A/UX types and ioctl numbers, EWOULDBLOCK for EAGAIN.
 *
 * K&R C.
 */

#include "auxcore.h"
#include "hsock.h"

extern int aux_socktype_in(), aux_socktype_out();
extern unsigned long aux_sockioc();

#define	SOL_SOCKET	0xffff
#define	SO_TYPE		0x1008
#define	SIOCGIFCONF	0xc0086914
#define	AF_INET		2

static int
ret(e)
	int e;
{
	return e == EAGAIN ? AUXE_WOULDBLOCK : e;
}

static int
sock(h, a, r)
	struct hs *h;
	long *a;
	char *r;
{
	return hs_attach(h, (int)a[0], aux_gap(r, HS_GAP));
}

/* an address argument into buf */
static int
addrin(ua, len, buf)
	long ua, len;
	char *buf;
{
	if (len < 0 || len > HS_ADDR)
		return EINVAL;
	return copyin((caddr_t)ua, buf, (u_int)len) ? EFAULT : 0;
}

/* an address result to ua, its length to *ulen (value-result) */
static int
addrout(buf, n, ua, ulen)
	char *buf;
	int n;
	long ua, ulen;
{
	if (ua == 0 || ulen == 0)
		return 0;
	if (copyout(buf, (caddr_t)ua, (u_int)n) || suword((int *)ulen, n))
		return EFAULT;
	return 0;
}

static int
ulen(p, n)
	long p;
	int *n;
{
	*n = 0;
	if (p == 0)
		return 0;
	*n = fuword((int *)p);
	if (*n < 0)
		return EINVAL;
	if (*n > HS_ADDR)
		*n = HS_ADDR;
	return 0;
}

int
aux_socket(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	int t = aux_socktype_in(a[1]), fd;
	int e;

	if (t < 0)
		return ESOCKTNOSUPPORT;
	if ((e = hs_socket(aux_gap(r, HS_GAP), (int)a[0], t, (int)a[2], &fd)) != 0)
		return e;
	rv->r_val1 = fd;
	return 0;
}

int
aux_bind(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	char b[HS_ADDR];
	struct hs h;
	int e;

	if ((e = sock(&h, a, r)) != 0 || (e = addrin(a[1], a[2], b)) != 0)
		return e;
	return hs_bind(&h, b, (int)a[2]);
}

int
aux_connect(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	char b[HS_ADDR];
	struct hs h;
	int e;

	if ((e = sock(&h, a, r)) != 0 || (e = addrin(a[1], a[2], b)) != 0)
		return e;
	return ret(hs_connect(&h, b, (int)a[2]));
}

int
aux_listen(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	struct hs h;
	int e;

	if ((e = sock(&h, a, r)) != 0)
		return e;
	return hs_listen(&h, (int)a[1]);
}

int
aux_accept(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	char b[HS_ADDR];
	struct hs h;
	int e, n, fd;

	if ((e = sock(&h, a, r)) != 0 || (e = ulen(a[1] ? a[2] : 0L, &n)) != 0)
		return e;
	if ((e = hs_accept(&h, &fd, b, &n)) != 0)
		return ret(e);
	rv->r_val1 = fd;
	return addrout(b, n, a[1], a[2]);
}

static int
sendto(a, r, rv, flags, to, tolen)
	long *a;
	char *r;
	rval_t *rv;
	long flags, to, tolen;
{
	char b[HS_ADDR];
	struct hs h;
	int e, n;

	if ((e = sock(&h, a, r)) != 0 || (to && (e = addrin(to, tolen, b)) != 0))
		return e;
	if ((e = hs_send(&h, (caddr_t)a[1], (int)a[2], (int)flags, to ? b : (char *)0,
	    (int)tolen, &n)) != 0)
		return ret(e);
	rv->r_val1 = n;
	return 0;
}

static int
recvfrom(a, r, rv, flags, from, fromlen)
	long *a;
	char *r;
	rval_t *rv;
	long flags, from, fromlen;
{
	char b[HS_ADDR];
	struct hs h;
	int e, n, al;

	if ((e = sock(&h, a, r)) != 0 || (e = ulen(from ? fromlen : 0L, &al)) != 0)
		return e;
	if ((e = hs_recv(&h, (caddr_t)a[1], (int)a[2], (int)flags, b,
	    from ? &al : (int *)0, &n)) != 0)
		return ret(e);
	rv->r_val1 = n;
	return from ? addrout(b, al, from, fromlen) : 0;
}

int
aux_send(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return sendto(a, r, rv, a[3], 0L, 0L);
}

int
aux_sendto(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return sendto(a, r, rv, a[3], a[4], a[5]);
}

int
aux_recv(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return recvfrom(a, r, rv, a[3], 0L, 0L);
}

int
aux_recvfrom(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return recvfrom(a, r, rv, a[3], a[4], a[5]);
}

/* sendmsg, recvmsg: one iovec, no access rights */
static int
msg(a, r, rv, out)
	long *a;
	char *r;
	rval_t *rv;
	int out;
{
	long m[6], iov[2], b[6];
	int e;

	if (copyin((caddr_t)a[1], (caddr_t)m, sizeof m))
		return EFAULT;
	if (m[3] != 1 || m[5] != 0)
		return EOPNOTSUPP;
	if (copyin((caddr_t)m[2], (caddr_t)iov, sizeof iov))
		return EFAULT;
	b[0] = a[0]; b[1] = iov[0]; b[2] = iov[1]; b[3] = a[2];
	if (out)
		return sendto(b, r, rv, a[2], m[0], m[1]);
	if ((e = recvfrom(b, r, rv, a[2], m[0], m[0] ? a[1] + 4 : 0L)) != 0)
		return e;
	return 0;
}

int
aux_sendmsg(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return msg(a, r, rv, 1);
}

int
aux_recvmsg(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return msg(a, r, rv, 0);
}

static int
name(a, r, peer)
	long *a;
	char *r;
	int peer;
{
	char b[HS_ADDR];
	struct hs h;
	int e, n;

	if ((e = sock(&h, a, r)) != 0 || (e = ulen(a[2], &n)) != 0)
		return e;
	if (a[2] == 0)
		return EFAULT;
	if ((e = hs_name(&h, peer, b, &n)) != 0)
		return e;
	return addrout(b, n, a[1], a[2]);
}

int
aux_getsockname(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return name(a, r, 0);
}

int
aux_getpeername(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return name(a, r, 1);
}

int
aux_getsockopt(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	char b[HS_ADDR];
	struct hs h;
	long v;
	int e, n;

	if ((e = sock(&h, a, r)) != 0 || (e = ulen(a[4], &n)) != 0)
		return e;
	if (a[3] == 0 || a[4] == 0)
		return EFAULT;
	if (copyin((caddr_t)a[3], b, (u_int)n))
		return EFAULT;
	if ((e = hs_getopt(&h, (int)a[1], (int)a[2], b, &n)) != 0)
		return e;
	if (a[1] == SOL_SOCKET && a[2] == SO_TYPE) {
		bcopy(b, (caddr_t)&v, 4);
		v = aux_socktype_out(v);
		bcopy((caddr_t)&v, b, 4);
	}
	return addrout(b, n, a[3], a[4]);
}

int
aux_setsockopt(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	char b[HS_ADDR];
	struct hs h;
	int e;

	if ((e = sock(&h, a, r)) != 0 || (e = addrin(a[3], a[4], b)) != 0)
		return e;
	return hs_setopt(&h, (int)a[1], (int)a[2], b, (int)a[4]);
}

int
aux_shutdown(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	struct hs h;
	int e;

	if ((e = sock(&h, a, r)) != 0)
		return e;
	return hs_shutdown(&h, (int)a[1]);
}

int
aux_socketpair(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	return EOPNOTSUPP;
}

/* select: a nonblocking connect is pending on fd */
int
aux_connecting(fd)
	int fd;
{
	file_t *fp;

	return getf(fd, &fp) == 0 && hs_issock(fp) && hs_connecting(fp);
}

/* select: -1 while fd's connect is pending, else its error */
int
aux_connwait(fd, r)
	int fd;
	char *r;
{
	struct hs h;
	long a = fd;

	return sock(&h, &a, r) ? 0 : hs_connwait(&h);
}

/*
 * Interfaces with loopback addresses last, as BSD lists them: programs
 * take the first interface that is up as their own address.
 */
static void
lolast(uarg)
	caddr_t uarg;
{
	long ifc[2];
	char *b, *t;
	int i, n, k, pass;

	if (copyin(uarg, (caddr_t)ifc, sizeof ifc) || ifc[0] <= 32 || ifc[0] > 0x10000)
		return;
	n = ifc[0] & ~31;
	b = (char *)kmem_alloc((u_int)(2 * n), KM_SLEEP);
	t = b + n;
	if (copyin((caddr_t)ifc[1], b, (u_int)n) == 0) {
		for (k = 0, pass = 0; pass < 2; pass++)
			for (i = 0; i < n; i += 32)
				if ((b[i + 17] == AF_INET && (u_char)b[i + 20] == 127) == pass) {
					bcopy(b + i, t + k, 32);
					k += 32;
				}
		(void)copyout(t, (caddr_t)ifc[1], (u_int)n);
	}
	kmem_free((_VOID *)b, (u_int)(2 * n));
}

/* ioctl on a socket: 1 with *ep set if handled */
int
aux_sockctl(a, rv, r, ep)
	long *a;
	rval_t *rv;
	char *r;
	int *ep;
{
	unsigned long c = aux_sockioc((unsigned long)a[1]);
	struct hs h;
	file_t *fp;
	long v;

	if (c == 0 || getf((int)a[0], &fp) || !hs_issock(fp))
		return 0;
	if ((*ep = sock(&h, a, r)) == 0 &&
	    (*ep = hs_ioctl(&h, (int)c, (caddr_t)a[2], &v)) == 0) {
		rv->r_val1 = v;
		if (c == SIOCGIFCONF)
			lolast((caddr_t)a[2]);
	}
	*ep = ret(*ep);
	if (mac_socktrace)
		aux_thex("sockioc", a, 3, a[2], 16, *ep ? (long)-*ep : rv->r_val1);
	return 1;
}
