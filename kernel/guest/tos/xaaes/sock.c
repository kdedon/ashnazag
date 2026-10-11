/*
 * sock.c -- MiNTNet's socket calls on the environment's host sockets:
 * AF_INET only, MiNT's numbering turned into the host's.
 */
#include "xamint.h"

#define	ME	KP(curproc)

/* the host's TSO_* operations */
enum { S_SOCKET = 1, S_BIND, S_CONNECT, S_LISTEN, S_ACCEPT, S_SEND, S_RECV,
	S_CONNWAIT, S_NAME, S_GETOPT, S_SETOPT, S_SHUTDOWN };

struct iov {
	char	*base;
	long	len;
};

struct mmsg {
	char	*name;
	long	namelen;
	struct iov *iov;
	long	iovlen;
	void	*acc;
	long	acclen;
};

/* MiNT's SOL_SOCKET option names as the host's */
static const short soname[] = {
	0, 1, 4, 0x1008, 0x1007, 0x10, 0x20, 0x1001, 0x1002, 8, 0x100, 0x80
};

static long
sopt(long level, long name)
{
	if (level != 0xffff)
		return level << 16 | (name & 0xffff);
	if (name <= 0 || name >= (long)(sizeof soname / sizeof soname[0]))
		return ENOPROTOOPT;
	return 0xffffL << 16 | soname[name];
}

/* fd's host socket, with its nonblocking flag */
static long
sfd(short fd, int *ndelay)
{
	struct file *f = fd_get(ME, fd);
	long h = sock_hfd(f);

	if (!f)
		return EBADF;
	if (h < 0)
		return ENOTSOCK;
	if (ndelay)
		*ndelay = f->flags & O_NDELAY;
	return h;
}

static long
newfd(long hfd, int flags)
{
	struct file *f;
	long r;

	if (hfd < 0)
		return hfd;
	if ((r = file_sock(hfd, flags, &f)) != 0)
		return r;
	r = fd_alloc(ME, f, 6);
	if (r < 0)
		file_close(f);
	return r;
}

static long _cdecl
sys_f_socket(long domain, long type, long proto)
{
	(void)proto;
	if (domain != 2)
		return EAFNOSUPPORT;
	if ((type & 0xff) != 1 && (type & 0xff) != 2)
		return ESOCKTNOSUPPORT;
	return newfd(sock_op(S_SOCKET, 0, (type & 0xff) == 1 ? 2 : 1, 0, 0, 0, 0, 1), 0);
}

static long _cdecl
sys_f_accept(short fd, char *addr, long *addrlen)
{
	int nd;
	long h = sfd(fd, &nd), n = 16;

	if (h < 0)
		return h;
	h = sock_op(S_ACCEPT, h, 0, 0, 0, addr ? addr : 0, addr && addrlen ? addrlen : &n, nd);
	return newfd(h, 0);
}

static long _cdecl
sys_f_connect(short fd, char *addr, long addrlen)
{
	int nd;
	long h = sfd(fd, &nd), r;

	if (h < 0)
		return h;
	r = sock_op(S_CONNECT, h, 0, 0, 0, addr, &addrlen, nd);
	if (r != EINPROGRESS || nd)
		return r < 0 ? r : 0;
	while ((r = sock_op(S_CONNWAIT, h, 0, 0, 0, 0, 0, 1)) == -1) {
		if (curproc->sigpending & ~curproc->p_sigmask)
			return EINTR;
		nap(20);
	}
	return r < 0 ? r : merr(-r);
}

static long _cdecl
sys_f_bind(short fd, char *addr, long addrlen)
{
	long h = sfd(fd, 0), r;

	if (h < 0)
		return h;
	r = sock_op(S_BIND, h, 0, 0, 0, addr, &addrlen, 1);
	return r < 0 ? r : 0;
}

static long _cdecl
sys_f_listen(short fd, long backlog)
{
	long h = sfd(fd, 0), r;

	if (h < 0)
		return h;
	r = sock_op(S_LISTEN, h, backlog, 0, 0, 0, 0, 1);
	return r < 0 ? r : 0;
}

static long _cdecl
sys_f_recvfrom(short fd, char *buf, long len, long flags, char *addr, long *addrlen)
{
	int nd;
	long h = sfd(fd, &nd);

	if (h < 0)
		return h;
	return sock_op(S_RECV, h, flags, buf, len, addrlen ? addr : 0, addrlen, nd);
}

static long _cdecl
sys_f_sendto(short fd, char *buf, long len, long flags, char *addr, long addrlen)
{
	int nd;
	long h = sfd(fd, &nd);

	if (h < 0)
		return h;
	return sock_op(S_SEND, h, flags, buf, len, addr, addr ? &addrlen : 0, nd);
}

/* the message's pieces through one buffer */
static long
msg(short fd, struct mmsg *m, long flags, int out)
{
	char *b;
	long n = 0, r, i, o;

	for (i = 0; i < m->iovlen; i++)
		n += m->iov[i].len;
	if ((b = kmalloc_o(n ? n : 1, 0)) == 0)
		return ENOMEM;
	if (out) {
		for (i = o = 0; i < m->iovlen; o += m->iov[i++].len)
			memcpy(b + o, m->iov[i].base, m->iov[i].len);
		r = sys_f_sendto(fd, b, n, flags, m->name, m->namelen);
	} else {
		r = sys_f_recvfrom(fd, b, n, flags, m->name, m->name ? &m->namelen : 0);
		for (i = o = 0; r > 0 && i < m->iovlen && o < r; o += m->iov[i++].len)
			memcpy(m->iov[i].base, b + o, m->iov[i].len < r - o ? m->iov[i].len : r - o);
	}
	kfree_any(b);
	return r;
}

static long _cdecl
sys_f_recvmsg(short fd, struct mmsg *m, long flags)
{
	return msg(fd, m, flags, 0);
}

static long _cdecl
sys_f_sendmsg(short fd, struct mmsg *m, long flags)
{
	return msg(fd, m, flags, 1);
}

static long _cdecl
sys_f_setsockopt(short fd, long level, long name, void *val, long len)
{
	long h = sfd(fd, 0), o = sopt(level, name), r;

	if (h < 0)
		return h;
	if (o < 0)
		return o;
	r = sock_op(S_SETOPT, h, o, val, len, 0, 0, 1);
	return r < 0 ? r : 0;
}

static long _cdecl
sys_f_getsockopt(short fd, long level, long name, void *val, long *len)
{
	long h = sfd(fd, 0), o = sopt(level, name), r;

	if (h < 0)
		return h;
	if (o < 0)
		return o;
	if (!len)
		return EINVAL;
	if ((r = sock_op(S_GETOPT, h, o, val, *len, 0, 0, 1)) < 0)
		return r;
	*len = r;
	if (o == (0xffffL << 16 | 0x1008))
		*(long *)val = *(long *)val == 2 ? 1 : 2;
	else if (o == (0xffffL << 16 | 0x1007) && *(long *)val)
		*(long *)val = -merr(-*(long *)val);
	return 0;
}

static long
names(short fd, char *addr, long *len, int peer)
{
	long h = sfd(fd, 0), r;

	if (h < 0)
		return h;
	r = sock_op(S_NAME, h, peer, 0, 0, addr, len, 1);
	return r < 0 ? r : 0;
}

static long _cdecl
sys_f_getpeername(short fd, char *addr, long *len)
{
	return names(fd, addr, len, 1);
}

static long _cdecl
sys_f_getsockname(short fd, char *addr, long *len)
{
	return names(fd, addr, len, 0);
}

static long _cdecl
sys_f_shutdown(short fd, long how)
{
	long h = sfd(fd, 0), r;

	if (h < 0)
		return h;
	r = sock_op(S_SHUTDOWN, h, how, 0, 0, 0, 0, 1);
	return r < 0 ? r : 0;
}

void
sock_init(dos_vecs *t)
{
	t->p_f_socket = sys_f_socket;
	t->p_f_accept = (void *)sys_f_accept;
	t->p_f_connect = (void *)sys_f_connect;
	t->p_f_bind = (void *)sys_f_bind;
	t->p_f_listen = sys_f_listen;
	t->p_f_recvmsg = (void *)sys_f_recvmsg;
	t->p_f_sendmsg = (void *)sys_f_sendmsg;
	t->p_f_recvfrom = (void *)sys_f_recvfrom;
	t->p_f_sendto = (void *)sys_f_sendto;
	t->p_f_setsockopt = sys_f_setsockopt;
	t->p_f_getsockopt = sys_f_getsockopt;
	t->p_f_getpeername = (void *)sys_f_getpeername;
	t->p_f_getsockname = (void *)sys_f_getsockname;
	t->p_f_shutdown = sys_f_shutdown;
}
