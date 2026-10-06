/*
 * amigasock.c -- AMIGAIOC_SOCK: bsdsocket.library's socket calls, made
 * by the Amiga environment's own process on its own descriptors, so the
 * user's rights apply.  Host sockets never block: every Amiga task
 * shares this process, so the library waits on Exec signals, and the
 * stream head's SIGPOLL becomes a PORTS interrupt (amiga_sendsig).
 *
 * K&R C.
 */

#include "amiga.h"
#include "sys/vnode.h"
#include "sys/file.h"
#include "sys/stropts.h"
#include "sys/poll.h"
#include "amigasock.h"
#include "../guestcore/hsock.h"

#define	FIONBIO		0x8004667e
#define	FIOASYNC	0x8004667d
#define	FIONREAD	0x4004667f
#define	AMIX_POLL	87
#define	AMIX_CLOSE	6

static int
sys(n, a0, a1, a2)
	int n;
	long a0, a1, a2;
{
	long a[6];
	rval_t rv;
	int e, sc = u.u_syscall;

	a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a[4] = a[5] = 0;
	rv.r_val1 = rv.r_val2 = 0;
	u.u_syscall = n;
	e = (*sysent[n].sy_call)(a, &rv);
	u.u_syscall = sc;
	return e;
}

/* nonblocking, with SIGPOLL on input, output space, errors and hangup */
static int
setup(fd, gap)
	int fd;
	caddr_t gap;
{
	struct hs h;
	long one = 1, r;
	int e;

	if ((e = hs_attach(&h, fd, gap)) != 0 ||
	    (e = copyout((caddr_t)&one, gap, 4) ? EFAULT : 0) != 0 ||
	    (e = hs_ioctl(&h, FIONBIO, gap, &r)) != 0)
		return e;
	return hs_ioctl(&h, FIOASYNC, gap, &r);
}

/*
 * poll without waiting.  A pending nonblocking connect is settled first
 * and counts as neither readable nor writable until it ends; one that
 * failed is writable with POLLERR, as select reports it.
 */
static int
spoll(b)
	struct amigasock *b;
{
	long *p;
	char *c;
	file_t *fp;
	struct hs h;
	int e, i, k, n = (int)b->bs_len, sz = n * 8;

	if (n < 0 || n > BS_NPOLL)
		return EINVAL;
	if (n == 0)
		return 0;
	p = (long *)kmem_alloc((u_int)sz, KM_SLEEP);
	c = (char *)kmem_zalloc((u_int)n, KM_SLEEP);
	if (copyin(b->bs_buf, (caddr_t)p, (u_int)sz)) {
		e = EFAULT;
		goto out;
	}
	for (i = 0; i < n; i++)
		if (getf((int)p[2 * i], &fp) == 0 && hs_issock(fp) && hs_connecting(fp) &&
		    hs_attach(&h, (int)p[2 * i], b->bs_gap) == 0 &&
		    (k = hs_connwait(&h)) != 0)
			c[i] = k < 0 ? 1 : 2;
	if ((e = sys(AMIX_POLL, (long)b->bs_buf, (long)n, 0L)) != 0 ||
	    copyin(b->bs_buf, (caddr_t)p, (u_int)sz)) {
		e = e ? e : EFAULT;
		goto out;
	}
	for (i = 0, k = 0; i < n; i++) {
		if (c[i] == 1)
			p[2 * i + 1] &= ~(long)(POLLIN | POLLRDNORM | POLLOUT);
		else if (c[i] == 2)
			p[2 * i + 1] |= POLLOUT | POLLERR;
		k += (p[2 * i + 1] & 0xffff) != 0;
	}
	b->bs_rv = k;
	e = copyout((caddr_t)p, b->bs_buf, (u_int)sz) ? EFAULT : 0;
out:
	kmem_free((caddr_t)p, (u_int)sz);
	kmem_free((caddr_t)c, (u_int)n);
	return e;
}

int
amiga_sock(arg)
	caddr_t arg;
{
	struct amigasock b;
	struct hs h;
	char a[HS_ADDR], o[64];
	int e, v = 0, n = HS_ADDR, sc = u.u_syscall;

	if (copyin(arg, (caddr_t)&b, sizeof b))
		return EFAULT;
	if (b.bs_alen < 0 || b.bs_alen > sizeof b.bs_addr)
		return EINVAL;
	bzero(a, sizeof a);
	bzero(o, sizeof o);
	bcopy(b.bs_addr, a, sizeof b.bs_addr);
	if (b.bs_op == BSO_SOCKET) {
		if ((e = hs_socket(b.bs_gap, 2, (int)b.bs_arg, (int)b.bs_len, &v)) == 0 &&
		    (e = setup(v, b.bs_gap)) != 0)
			(void)sys(AMIX_CLOSE, (long)v, 0L, 0L);
	} else if (b.bs_op == BSO_POLL)
		e = spoll(&b);
	else if ((e = hs_attach(&h, (int)b.bs_fd, b.bs_gap)) == 0) {
		switch (b.bs_op) {
		case BSO_BIND:
			e = hs_bind(&h, a, (int)b.bs_alen);
			break;
		case BSO_CONNECT:
			e = hs_connect(&h, a, (int)b.bs_alen);
			break;
		case BSO_LISTEN:
			e = hs_listen(&h, (int)b.bs_arg);
			break;
		case BSO_ACCEPT:
			if ((e = hs_accept(&h, &v, a, &n)) == 0 &&
			    (e = setup(v, b.bs_gap)) != 0)
				(void)sys(AMIX_CLOSE, (long)v, 0L, 0L);
			break;
		case BSO_SEND:
			e = hs_send(&h, b.bs_buf, (int)b.bs_len, (int)b.bs_arg,
			    b.bs_alen ? a : (char *)0, (int)b.bs_alen, &v);
			break;
		case BSO_RECV:
			if (b.bs_len < 0) {
				e = EINVAL;
				break;
			}
			e = hs_recv(&h, b.bs_buf, (int)b.bs_len, (int)b.bs_arg, a, &n, &v);
			break;
		case BSO_CONNWAIT:
			v = hs_connwait(&h);
			break;
		case BSO_NAME:
			e = hs_name(&h, (int)b.bs_arg, a, &n);
			break;
		case BSO_GETOPT:
			if ((v = (int)b.bs_len) < 0)
				e = EINVAL;
			else if (v > sizeof o)
				v = sizeof o;
			if (e == 0 && (e = hs_getopt(&h, (int)(b.bs_arg >> 16) & 0xffff,
			    (int)b.bs_arg & 0xffff, o, &v)) == 0 && copyout(o, b.bs_buf, v))
				e = EFAULT;
			break;
		case BSO_SETOPT:
			if (b.bs_len < 0 || b.bs_len > sizeof o)
				e = EINVAL;
			else if (copyin(b.bs_buf, o, (int)b.bs_len))
				e = EFAULT;
			else
				e = hs_setopt(&h, (int)(b.bs_arg >> 16) & 0xffff,
				    (int)b.bs_arg & 0xffff, o, (int)b.bs_len);
			break;
		case BSO_SHUTDOWN:
			e = hs_shutdown(&h, (int)b.bs_arg);
			break;
		case BSO_NREAD:
			{
				long r, k = 0;

				if ((e = hs_ioctl(&h, FIONREAD, b.bs_gap, &r)) == 0 && r > 0 &&
				    copyin(b.bs_gap, (caddr_t)&k, 4))
					e = EFAULT;
				v = r > 0 ? k : 0;
			}
			break;
		default:
			e = EINVAL;
		}
	}
	u.u_syscall = sc;
	if (e)
		return e;
	if (b.bs_op != BSO_POLL)
		b.bs_rv = v;
	if (n > sizeof b.bs_addr)
		n = sizeof b.bs_addr;
	bcopy(a, b.bs_addr, sizeof b.bs_addr);
	b.bs_alen = n;
	return copyout((caddr_t)&b, arg, sizeof b) ? EFAULT : 0;
}
