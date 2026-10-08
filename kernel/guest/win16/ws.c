/*
 * ws.c -- WINSOCK.DLL, Windows Sockets 1.1, ours (as Wabi 2's was) over
 * the host's BSD sockets: the host's network, the user's rights.
 *
 * A Windows socket is a small number naming a host descriptor, always
 * nonblocking underneath.  A blocking call waits as Windows' stacks did:
 * the blocking hook runs meanwhile (the program's, or the default that
 * dispatches its messages), until the descriptor is ready or the call is
 * cancelled.  WSAAsyncSelect's events are found by ws_poll() from the
 * message loop and posted, each once until the call that re-enables it.
 * The asynchronous database calls are done at once and their answer
 * posted.  The program's structures (sockaddr_in, hostent, fd_set ...)
 * are 16-bit and little-endian; they are converted field by field.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include "w16.h"
#include "win.h"

#ifndef O_NONBLOCK
#define	O_NONBLOCK	O_NDELAY
#endif
#ifdef __linux__
#define	SOCKLEN	socklen_t
#else
#define	SOCKLEN	int		/* SVR4 */
#endif

#define	NSOCK		64
#define	SBASE		1		/* socket numbers: 1 up (0 and 0xffff are not sockets) */
#define	INVALID		0xffff
#define	SOCKET_ERROR	0xffff

#define	WSABASEERR	10000
#define	WSAEINTR	10004
#define	WSAEBADF	10009
#define	WSAEACCES	10013
#define	WSAEFAULT	10014
#define	WSAEINVAL	10022
#define	WSAEMFILE	10024
#define	WSAEWOULDBLOCK	10035
#define	WSAEINPROGRESS	10036
#define	WSAEALREADY	10037
#define	WSAENOTSOCK	10038
#define	WSAEDESTADDRREQ	10039
#define	WSAEMSGSIZE	10040
#define	WSAEPROTOTYPE	10041
#define	WSAENOPROTOOPT	10042
#define	WSAEPROTONOSUPPORT 10043
#define	WSAESOCKTNOSUPPORT 10044
#define	WSAEOPNOTSUPP	10045
#define	WSAEAFNOSUPPORT	10047
#define	WSAEADDRINUSE	10048
#define	WSAEADDRNOTAVAIL 10049
#define	WSAENETDOWN	10050
#define	WSAENETUNREACH	10051
#define	WSAENETRESET	10052
#define	WSAECONNABORTED	10053
#define	WSAECONNRESET	10054
#define	WSAENOBUFS	10055
#define	WSAEISCONN	10056
#define	WSAENOTCONN	10057
#define	WSAESHUTDOWN	10058
#define	WSAETIMEDOUT	10060
#define	WSAECONNREFUSED	10061
#define	WSAEHOSTUNREACH	10065
#define	WSANOTINITIALISED 10093
#define	WSAHOST_NOT_FOUND 11001
#define	WSANO_DATA	11004

#define	FD_READ		0x01
#define	FD_WRITE	0x02
#define	FD_OOB		0x04
#define	FD_ACCEPT	0x08
#define	FD_CONNECT	0x10
#define	FD_CLOSE	0x20

struct wsock {
	int	used, fd;
	int	nbio;			/* FIONBIO: the program asked for nonblocking */
	int	listening, connecting, connected, closed;
	u16	hwnd, msg;		/* WSAAsyncSelect's */
	int	events, armed;		/* asked for; may be posted now */
	struct task *task;
};

static struct wsock socks[NSOCK];
static int started;			/* WSAStartup calls less WSACleanup calls */
static int blocking, cancelled;
static u32 hook;			/* WSASetBlockingHook's, 0 the default */
static u16 dbsel;			/* the database answers and inet_ntoa's string */

/* ---- errors ---- */

static int
wserr(e)
	int e;
{
	switch (e) {
	case EINTR: return WSAEINTR;
	case EBADF: return WSAEBADF;
	case EACCES: return WSAEACCES;
	case EFAULT: return WSAEFAULT;
	case EINVAL: return WSAEINVAL;
	case EMFILE: return WSAEMFILE;
	case EAGAIN: return WSAEWOULDBLOCK;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK: return WSAEWOULDBLOCK;
#endif
	case EINPROGRESS: return WSAEINPROGRESS;
	case EALREADY: return WSAEALREADY;
	case ENOTSOCK: return WSAENOTSOCK;
	case EDESTADDRREQ: return WSAEDESTADDRREQ;
	case EMSGSIZE: return WSAEMSGSIZE;
	case EPROTOTYPE: return WSAEPROTOTYPE;
	case ENOPROTOOPT: return WSAENOPROTOOPT;
	case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
	case EOPNOTSUPP: return WSAEOPNOTSUPP;
	case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
	case EADDRINUSE: return WSAEADDRINUSE;
	case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
	case ENETDOWN: return WSAENETDOWN;
	case ENETUNREACH: return WSAENETUNREACH;
	case ENETRESET: return WSAENETRESET;
	case ECONNABORTED: return WSAECONNABORTED;
	case ECONNRESET: return WSAECONNRESET;
	case ENOBUFS: return WSAENOBUFS;
	case EISCONN: return WSAEISCONN;
	case ENOTCONN: return WSAENOTCONN;
	case ESHUTDOWN: return WSAESHUTDOWN;
	case ETIMEDOUT: return WSAETIMEDOUT;
	case ECONNREFUSED: return WSAECONNREFUSED;
	case EHOSTUNREACH: return WSAEHOSTUNREACH;
	case EPIPE: return WSAESHUTDOWN;
	}
	return WSAEINVAL;
}

static void
seterr(e)
	int e;
{
	if (curtask)
		curtask->t_wserr = e;
}

static u32
fail(e)
	int e;
{
	seterr(e);
	return SOCKET_ERROR;
}

static struct wsock *
sget(s)
	u32 s;
{
	s &= 0xffff;
	if (s < SBASE || s >= SBASE + NSOCK || !socks[s - SBASE].used)
		return 0;
	return &socks[s - SBASE];
}

#define	SOCK(s)	struct wsock *w = sget(s); if (!started) return fail(WSANOTINITIALISED); \
		if (!w) return fail(WSAENOTSOCK)

/* ---- byte order: the program's is little-endian whatever the host's ---- */

static u32 swap16(v) u32 v; { return (v >> 8 & 0xff) | (v & 0xff) << 8; }
static u32 swap32(v) u32 v; { return swap16(v >> 16) | swap16(v) << 16; }

/* a sockaddr_in of the program's to the host's: family little-endian, port and address as bytes */
static int
getaddr(p, len, sin)
	u32 p, len;
	struct sockaddr_in *sin;
{
	if (!p || len < 8 || GW(p) != 2)	/* AF_INET */
		return -1;
	memset((char *)sin, 0, sizeof *sin);
	sin->sin_family = AF_INET;
	memcpy((char *)&sin->sin_port, M + p + 2, 2);
	memcpy((char *)&sin->sin_addr, M + p + 4, 4);
	return 0;
}

static void
putaddr(sin, p, lenp)
	struct sockaddr_in *sin;
	u32 p, lenp;
{
	if (!p)
		return;
	if (lenp && (short)GW(lenp) < 16) {
		PW(lenp, 16);
		return;
	}
	memset(M + p, 0, 16);
	PW(p, 2);
	memcpy(M + p + 2, (char *)&sin->sin_port, 2);
	memcpy(M + p + 4, (char *)&sin->sin_addr, 4);
	if (lenp)
		PW(lenp, 16);
}

/* ---- waiting as a blocking call does ---- */

/* the default blocking hook: the program's messages dispatched, while any */
static void
defhook()
{
	extern int user_getmessage();
	extern u32 user_dispatch();
	extern int user_translate();
	u32 p = ualloc(MSG_SIZE), a = ulin(p);

	if (user_getmessage(a, (u32)0, 0, 0, 1, 0) > 0) {
		user_translate(a);
		user_dispatch(a);
	}
	ufree(p);
}

/*
 * Until fd is ready for what (POLLIN, POLLOUT): 0, or a Windows error
 * (WSAEINTR when cancelled).  The blocking hook runs between looks.
 */
static int
waitfd(fd, what)
	int fd, what;
{
	extern void user_idle();
	struct pollfd pf;
	int r = 0;

	if (blocking)
		return WSAEINPROGRESS;
	blocking = 1;
	cancelled = 0;
	for (;;) {
		pf.fd = fd;
		pf.events = what;
		pf.revents = 0;
		if (poll(&pf, 1, 10) > 0)
			break;
		if (cancelled) {
			r = WSAEINTR;
			break;
		}
		if (hook) {
			cb_begin();
			if (!(cb_call(hook, 0) & 0xffff))
				user_idle();
		} else {
			defhook();
			user_idle();
		}
		if (cancelled) {
			r = WSAEINTR;
			break;
		}
	}
	blocking = 0;
	return r;
}

/* ---- WSAAsyncSelect's events ---- */

static void
post(w, ev, err)
	struct wsock *w;
	int ev, err;
{
	w->armed &= ~ev;
	wnd_post(w->hwnd, w->msg, (u32)(w - socks + SBASE), FP(err, ev));
}

/* from the message loop: what the async sockets have to say */
void
ws_poll()
{
	struct pollfd pf;
	struct wsock *w;
	int i, n, e;
	SOCKLEN len;

	for (i = 0; i < NSOCK; i++) {
		w = &socks[i];
		if (!w->used || !w->hwnd || !(w->events & w->armed) || !wnd_get(w->hwnd))
			continue;
		pf.fd = w->fd;
		pf.events = POLLIN | POLLOUT;
		pf.revents = 0;
		if (poll(&pf, 1, 0) <= 0)
			continue;
		if (w->connecting && (pf.revents & (POLLOUT | POLLERR | POLLHUP))) {
			e = 0;
			len = sizeof e;
			getsockopt(w->fd, SOL_SOCKET, SO_ERROR, (char *)&e, &len);
			w->connecting = 0;
			w->connected = !e;
			if (w->events & FD_CONNECT)
				post(w, FD_CONNECT, e ? wserr(e) : 0);
			if (!e && (w->events & FD_WRITE) && (w->armed & FD_WRITE))
				post(w, FD_WRITE, 0);
			continue;
		}
		if ((pf.revents & (POLLIN | POLLHUP | POLLERR)) && w->listening) {
			if ((w->events & FD_ACCEPT) && (w->armed & FD_ACCEPT))
				post(w, FD_ACCEPT, 0);
			continue;
		}
		if (pf.revents & (POLLIN | POLLHUP | POLLERR)) {
			char c;

			/* data, or the end of it */
			n = recv(w->fd, &c, 1, MSG_PEEK);
			if (n > 0) {
				if ((w->events & FD_READ) && (w->armed & FD_READ))
					post(w, FD_READ, 0);
			} else if (!w->closed && (n == 0 || (errno != EAGAIN && errno != EINTR))) {
				w->closed = 1;
				if (w->events & FD_CLOSE)
					post(w, FD_CLOSE, n == 0 ? 0 : wserr(errno));
			}
		}
		if ((pf.revents & POLLOUT) && w->connected && (w->events & FD_WRITE) && (w->armed & FD_WRITE))
			post(w, FD_WRITE, 0);
	}
}

/* milliseconds until ws_poll should look again, -1 no async sockets */
int
ws_next()
{
	int i;

	for (i = 0; i < NSOCK; i++)
		if (socks[i].used && socks[i].hwnd && (socks[i].events & socks[i].armed))
			return 20;
	return -1;
}

/* ---- startup ---- */

static u32
w_WSAStartup(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	int maj = a[0] & 0xff, min = (a[0] >> 8) & 0xff;

	if (maj < 1 || (maj == 1 && min < 1))
		return 10092;		/* WSAVERNOTSUPPORTED */
	if (p) {
		/* WSADATA: version, highest, description[257], status[129], sockets, datagram, vendor */
		memset(M + p, 0, 398);
		PW(p, 0x0101);
		PW(p + 2, 0x0101);
		strcpy((char *)M + p + 4, "Ash Nazag Windows Sockets 1.1 on the host's sockets");
		strcpy((char *)M + p + 4 + 257, "Running");
		PW(p + 4 + 257 + 129, NSOCK);
		PW(p + 4 + 257 + 129 + 2, 8192);
	}
	if (!dbsel)
		dbsel = g_alloc(GMEM_ZEROINIT | GMEM_DDESHARE, (u32)2048, 0);
	started++;
	return 0;
}

static u32
w_WSACleanup(a)
	u32 *a;
{
	int i;

	if (!started)
		return fail(WSANOTINITIALISED);
	if (--started == 0)
		for (i = 0; i < NSOCK; i++)
			if (socks[i].used) {
				close(socks[i].fd);
				socks[i].used = 0;
			}
	return 0;
}

static u32 w_WSAGetLastError(a) u32 *a; { return curtask ? curtask->t_wserr : 0; }
static u32 w_WSASetLastError(a) u32 *a; { seterr((int)(a[0] & 0xffff)); return 0; }
static u32 w_WSAIsBlocking(a) u32 *a; { return blocking; }
static u32 w_WSACancelBlockingCall(a) u32 *a; { if (!blocking) return fail(WSAEINVAL); cancelled = 1; return 0; }

static u32
w_WSASetBlockingHook(a)
	u32 *a;
{
	u32 old = hook;

	hook = a[0];
	return old;
}

static u32 w_WSAUnhookBlockingHook(a) u32 *a; { hook = 0; return 0; }

/* ---- sockets ---- */

static u32
w_socket(a)
	u32 *a;
{
	int af = a[0] & 0xffff, type = a[1] & 0xffff, proto = a[2] & 0xffff, i, fd;

	if (!started)
		return fail(WSANOTINITIALISED);
	if (af != 2)
		return fail(WSAEAFNOSUPPORT);
	if (type != 1 && type != 2)
		return fail(WSAESOCKTNOSUPPORT);
	for (i = 0; i < NSOCK && socks[i].used; i++)
		;
	if (i == NSOCK)
		return fail(WSAEMFILE);
	fd = socket(AF_INET, type == 1 ? SOCK_STREAM : SOCK_DGRAM, proto == 6 ? IPPROTO_TCP : proto == 17 ? IPPROTO_UDP : 0);
	if (fd < 0)
		return fail(wserr(errno));
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	fcntl(fd, F_SETFD, 1);
	memset((char *)&socks[i], 0, sizeof socks[i]);
	socks[i].used = 1;
	socks[i].fd = fd;
	socks[i].task = curtask;
	socks[i].connected = type == 2;		/* a datagram socket can send at once */
	return SBASE + i;
}

static u32
w_closesocket(a)
	u32 *a;
{
	SOCK(a[0]);

	close(w->fd);
	w->used = 0;
	return 0;
}

static u32
w_bind(a)
	u32 *a;
{
	struct sockaddr_in sin;
	SOCK(a[0]);

	if (getaddr(lin(FPSEL(a[1]), FPOFF(a[1])), a[2] & 0xffff, &sin) < 0)
		return fail(WSAEFAULT);
	if (bind(w->fd, (struct sockaddr *)&sin, sizeof sin) < 0)
		return fail(wserr(errno));
	return 0;
}

static u32
w_listen(a)
	u32 *a;
{
	SOCK(a[0]);

	if (listen(w->fd, (int)(a[1] & 0xffff)) < 0)
		return fail(wserr(errno));
	w->listening = 1;
	w->armed |= FD_ACCEPT;
	return 0;
}

static u32
w_accept(a)
	u32 *a;
{
	struct sockaddr_in sin;
	SOCKLEN len = sizeof sin;
	int fd, i, e;
	SOCK(a[0]);

	for (;;) {
		fd = accept(w->fd, (struct sockaddr *)&sin, &len);
		if (fd >= 0 || (errno != EAGAIN && errno != EINTR) || w->nbio || w->hwnd)
			break;
		if ((e = waitfd(w->fd, POLLIN)) != 0)
			return fail(e);
	}
	w->armed |= FD_ACCEPT;
	if (fd < 0)
		return fail(wserr(errno));
	for (i = 0; i < NSOCK && socks[i].used; i++)
		;
	if (i == NSOCK) {
		close(fd);
		return fail(WSAEMFILE);
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	fcntl(fd, F_SETFD, 1);
	memset((char *)&socks[i], 0, sizeof socks[i]);
	socks[i].used = 1;
	socks[i].fd = fd;
	socks[i].task = curtask;
	socks[i].connected = 1;
	/* the new socket keeps the listener's async selection */
	socks[i].hwnd = w->hwnd;
	socks[i].msg = w->msg;
	socks[i].events = w->events;
	socks[i].armed = FD_READ | FD_WRITE | FD_OOB | FD_CLOSE;
	putaddr(&sin, lin(FPSEL(a[1]), FPOFF(a[1])), lin(FPSEL(a[2]), FPOFF(a[2])));
	return SBASE + i;
}

static u32
w_connect(a)
	u32 *a;
{
	struct sockaddr_in sin;
	SOCKLEN len;
	int e;
	SOCK(a[0]);

	if (getaddr(lin(FPSEL(a[1]), FPOFF(a[1])), a[2] & 0xffff, &sin) < 0)
		return fail(WSAEFAULT);
	if (connect(w->fd, (struct sockaddr *)&sin, sizeof sin) == 0) {
		w->connected = 1;
		w->armed |= FD_CONNECT | FD_WRITE;
		return 0;
	}
	if (errno != EINPROGRESS && errno != EAGAIN)
		return fail(wserr(errno));
	w->connecting = 1;
	w->armed |= FD_CONNECT | FD_WRITE;
	if (w->nbio || w->hwnd)
		return fail(WSAEWOULDBLOCK);
	if ((e = waitfd(w->fd, POLLOUT)) != 0)
		return fail(e);
	e = 0;
	len = sizeof e;
	getsockopt(w->fd, SOL_SOCKET, SO_ERROR, (char *)&e, &len);
	w->connecting = 0;
	if (e)
		return fail(wserr(e));
	w->connected = 1;
	return 0;
}

static u32
w_shutdown(a)
	u32 *a;
{
	SOCK(a[0]);

	if (shutdown(w->fd, (int)(a[1] & 0xffff)) < 0)
		return fail(wserr(errno));
	return 0;
}

/* send, sendto: the bytes, or SOCKET_ERROR */
static u32
dosend(w, p, n, flags, to)
	struct wsock *w;
	u32 p, n;
	int flags;
	struct sockaddr_in *to;
{
	int r, e, f = (flags & 1 ? MSG_OOB : 0) | (flags & 4 ? MSG_DONTROUTE : 0);

	if (n > 0 && !p)
		return fail(WSAEFAULT);
	for (;;) {
		r = to ? sendto(w->fd, (char *)M + p, (int)n, f, (struct sockaddr *)to, sizeof *to) :
		    send(w->fd, (char *)M + p, (int)n, f);
		if (r >= 0)
			return r;
		if ((errno != EAGAIN && errno != EINTR) || w->nbio || w->hwnd) {
			if (errno == EAGAIN)
				w->armed |= FD_WRITE;
			return fail(wserr(errno));
		}
		if ((e = waitfd(w->fd, POLLOUT)) != 0)
			return fail(e);
	}
}

static u32
w_send(a)
	u32 *a;
{
	SOCK(a[0]);

	return dosend(w, lin(FPSEL(a[1]), FPOFF(a[1])), a[2] & 0xffff, (int)a[3], (struct sockaddr_in *)0);
}

static u32
w_sendto(a)
	u32 *a;
{
	struct sockaddr_in sin;
	SOCK(a[0]);

	if (getaddr(lin(FPSEL(a[4]), FPOFF(a[4])), a[5] & 0xffff, &sin) < 0)
		return fail(WSAEFAULT);
	return dosend(w, lin(FPSEL(a[1]), FPOFF(a[1])), a[2] & 0xffff, (int)a[3], &sin);
}

static u32
dorecv(w, p, n, flags, from, fromlen)
	struct wsock *w;
	u32 p, n, from, fromlen;
	int flags;
{
	struct sockaddr_in sin;
	SOCKLEN len = sizeof sin;
	int r, e, f = (flags & 1 ? MSG_OOB : 0) | (flags & 2 ? MSG_PEEK : 0);

	if (n > 0 && !p)
		return fail(WSAEFAULT);
	for (;;) {
		r = recvfrom(w->fd, (char *)M + p, (int)n, f, (struct sockaddr *)&sin, &len);
		if (r >= 0)
			break;
		if ((errno != EAGAIN && errno != EINTR) || w->nbio || w->hwnd) {
			w->armed |= FD_READ;
			return fail(wserr(errno));
		}
		if ((e = waitfd(w->fd, POLLIN)) != 0)
			return fail(e);
	}
	w->armed |= FD_READ | FD_OOB;
	if (from)
		putaddr(&sin, from, fromlen);
	return r;
}

static u32
w_recv(a)
	u32 *a;
{
	SOCK(a[0]);

	return dorecv(w, lin(FPSEL(a[1]), FPOFF(a[1])), a[2] & 0xffff, (int)a[3], (u32)0, (u32)0);
}

static u32
w_recvfrom(a)
	u32 *a;
{
	SOCK(a[0]);

	return dorecv(w, lin(FPSEL(a[1]), FPOFF(a[1])), a[2] & 0xffff, (int)a[3], lin(FPSEL(a[4]), FPOFF(a[4])),
	    lin(FPSEL(a[5]), FPOFF(a[5])));
}

static u32
w_getsockname(a)
	u32 *a;
{
	struct sockaddr_in sin;
	SOCKLEN len = sizeof sin;
	SOCK(a[0]);

	if (getsockname(w->fd, (struct sockaddr *)&sin, &len) < 0)
		return fail(wserr(errno));
	putaddr(&sin, lin(FPSEL(a[1]), FPOFF(a[1])), lin(FPSEL(a[2]), FPOFF(a[2])));
	return 0;
}

static u32
w_getpeername(a)
	u32 *a;
{
	struct sockaddr_in sin;
	SOCKLEN len = sizeof sin;
	SOCK(a[0]);

	if (getpeername(w->fd, (struct sockaddr *)&sin, &len) < 0)
		return fail(wserr(errno));
	putaddr(&sin, lin(FPSEL(a[1]), FPOFF(a[1])), lin(FPSEL(a[2]), FPOFF(a[2])));
	return 0;
}

/* ioctlsocket: FIONBIO, FIONREAD, SIOCATMARK */
static u32
w_ioctlsocket(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[2]), FPOFF(a[2]));
	char buf[4096];
	int n;
	SOCK(a[0]);

	if (!p)
		return fail(WSAEFAULT);
	switch (a[1]) {
	case 0x8004667e:		/* FIONBIO */
		w->nbio = GL(p) != 0;
		return 0;
	case 0x4004667f:		/* FIONREAD: what one recv would get */
		n = recv(w->fd, buf, sizeof buf, MSG_PEEK);
		PL(p, n > 0 ? n : 0);
		return 0;
	case 0x40047307:		/* SIOCATMARK: no out-of-band data waits */
		PL(p, 1);
		return 0;
	}
	return fail(WSAEINVAL);
}

/* socket options: the program's numbers, ints of 16 bits */
static int
optmap(level, opt, hl, ho)
	int level, opt, *hl, *ho;
{
	if (level == 0xffff) {			/* SOL_SOCKET */
		*hl = SOL_SOCKET;
		switch (opt) {
		case 0x0001: *ho = SO_DEBUG; return 0;
		case 0x0004: *ho = SO_REUSEADDR; return 0;
		case 0x0008: *ho = SO_KEEPALIVE; return 0;
		case 0x0010: *ho = SO_DONTROUTE; return 0;
		case 0x0020: *ho = SO_BROADCAST; return 0;
		case 0x0080: *ho = SO_LINGER; return 1;
		case 0x0100: *ho = SO_OOBINLINE; return 0;
		case 0x1001: *ho = SO_SNDBUF; return 0;
		case 0x1002: *ho = SO_RCVBUF; return 0;
		case 0x1007: *ho = SO_ERROR; return 0;
		case 0x1008: *ho = SO_TYPE; return 0;
		}
		return -1;
	}
	if (level == 6 && opt == 1) {		/* IPPROTO_TCP, TCP_NODELAY */
		*hl = IPPROTO_TCP;
		*ho = TCP_NODELAY;
		return 0;
	}
	return -1;
}

static u32
w_setsockopt(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[3]), FPOFF(a[3]));
	int hl, ho, k, v;
	struct linger lg;
	SOCK(a[0]);

	if ((k = optmap((int)(a[1] & 0xffff), (int)(a[2] & 0xffff), &hl, &ho)) < 0)
		return fail(WSAENOPROTOOPT);
	if (!p)
		return fail(WSAEFAULT);
	if (k == 1) {
		lg.l_onoff = GW(p);
		lg.l_linger = GW(p + 2);
		if (setsockopt(w->fd, hl, ho, (char *)&lg, sizeof lg) < 0)
			return fail(wserr(errno));
		return 0;
	}
	v = (a[4] & 0xffff) >= 4 ? (int)GL(p) : (int)(short)GW(p);
	if (setsockopt(w->fd, hl, ho, (char *)&v, sizeof v) < 0)
		return fail(wserr(errno));
	return 0;
}

static u32
w_getsockopt(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[3]), FPOFF(a[3])), lp = lin(FPSEL(a[4]), FPOFF(a[4]));
	int hl, ho, k, v = 0;
	struct linger lg;
	SOCKLEN len;
	SOCK(a[0]);

	if ((k = optmap((int)(a[1] & 0xffff), (int)(a[2] & 0xffff), &hl, &ho)) < 0)
		return fail(WSAENOPROTOOPT);
	if (!p || !lp)
		return fail(WSAEFAULT);
	if (k == 1) {
		len = sizeof lg;
		if (getsockopt(w->fd, hl, ho, (char *)&lg, &len) < 0)
			return fail(wserr(errno));
		PW(p, lg.l_onoff);
		PW(p + 2, lg.l_linger);
		PW(lp, 4);
		return 0;
	}
	len = sizeof v;
	if (getsockopt(w->fd, hl, ho, (char *)&v, &len) < 0)
		return fail(wserr(errno));
	if (ho == SO_ERROR && hl == SOL_SOCKET && v)
		v = wserr(v);
	if (ho == SO_TYPE && hl == SOL_SOCKET)
		v = v == SOCK_STREAM ? 1 : 2;
	if ((short)GW(lp) >= 4)
		PL(p, v), PW(lp, 4);
	else
		PW(p, v), PW(lp, 2);
	return 0;
}

/*
 * select: fd_sets of 16-bit counts and sockets; a timeout of longs, or
 * none (wait, the blocking hook running).
 */
static u32
w_select(a)
	u32 *a;
{
	struct pollfd pf[3 * 64];
	u32 set[3], t = lin(FPSEL(a[4]), FPOFF(a[4]));
	int n = 0, i, k, cnt, r, total = 0, ms = -1, start = w16_ticks();
	struct wsock *w;

	if (!started)
		return fail(WSANOTINITIALISED);
	for (k = 0; k < 3; k++)
		set[k] = lin(FPSEL(a[k + 1]), FPOFF(a[k + 1]));
	if (t)
		ms = (int)(GL(t) * 1000 + GL(t + 4) / 1000);
	for (;;) {
		n = 0;
		for (k = 0; k < 3; k++) {
			if (!set[k])
				continue;
			cnt = GW(set[k]);
			for (i = 0; i < cnt && i < 64 && n < 3 * 64; i++) {
				if (!(w = sget(GW(set[k] + 2 + 2 * i))))
					return fail(WSAENOTSOCK);
				pf[n].fd = w->fd;
				pf[n].events = k == 0 ? POLLIN : k == 1 ? POLLOUT : POLLPRI;
				pf[n].revents = 0;
				n++;
			}
		}
		r = poll(pf, n, 0);
		if (r < 0)
			return fail(wserr(errno));
		if (r > 0 || ms == 0 || (ms > 0 && (int)(w16_ticks() - start) >= ms))
			break;
		if (blocking)
			return fail(WSAEINPROGRESS);
		/* wait a little, the hook running */
		{
			int e;

			if ((e = waitfd(pf[0].fd, (int)pf[0].events)) != 0)
				return fail(e);
		}
	}
	/* the sets keep the ready sockets */
	n = 0;
	for (k = 0; k < 3; k++) {
		int keep = 0;

		if (!set[k])
			continue;
		cnt = GW(set[k]);
		for (i = 0; i < cnt && i < 64; i++, n++)
			if (pf[n].revents & (pf[n].events | POLLHUP | POLLERR)) {
				PW(set[k] + 2 + 2 * keep, GW(set[k] + 2 + 2 * i));
				keep++;
			}
		PW(set[k], keep);
		total += keep;
	}
	return total;
}

static u32
w_WSAFDIsSet(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));
	int i;

	for (i = 0; p && i < GW(p) && i < 64; i++)
		if (GW(p + 2 + 2 * i) == (a[0] & 0xffff))
			return 1;
	return 0;
}

static u32
w_WSAAsyncSelect(a)
	u32 *a;
{
	SOCK(a[0]);

	w->hwnd = a[1];
	w->msg = a[2];
	w->events = a[3] & 0x3f;
	/* what is true now is said once: writable when connected, data waiting */
	w->armed = FD_READ | FD_OOB | FD_CLOSE | FD_ACCEPT | (w->connected ? FD_WRITE : 0) |
	    (w->connecting ? FD_CONNECT | FD_WRITE : 0);
	if (!w->events)
		w->hwnd = 0;
	return 0;
}

/* ---- byte order and addresses ---- */

static u32 w_htons(a) u32 *a; { return swap16(a[0]); }
static u32 w_htonl(a) u32 *a; { return swap32(a[0]); }

/* inet_addr: the address as its bytes lie in memory */
static u32
w_inet_addr(a)
	u32 *a;
{
	char *s = gptr(a[0]);
	unsigned long b[4];
	int n;

	if (!s)
		return 0xffffffff;
	n = sscanf(s, "%lu.%lu.%lu.%lu", &b[0], &b[1], &b[2], &b[3]);
	if (n != 4 || b[0] > 255 || b[1] > 255 || b[2] > 255 || b[3] > 255)
		return 0xffffffff;	/* INADDR_NONE */
	return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
}

static u32
w_inet_ntoa(a)
	u32 *a;
{
	u32 v = a[0];

	if (!dbsel)
		return 0;
	sprintf((char *)M + sel_base(dbsel) + 2000, "%u.%u.%u.%u", (unsigned)(v & 0xff), (unsigned)(v >> 8 & 0xff),
	    (unsigned)(v >> 16 & 0xff), (unsigned)(v >> 24 & 0xff));
	return FP(dbsel, 2000);
}

/* ---- the databases: 16-bit hostent, servent, protoent in a buffer ---- */

/*
 * A host's entry at p (n bytes; fp its far address): the hostent, its
 * aliases' and addresses' arrays, the addresses, the strings.  Its size,
 * 0 if it does not fit.
 */
static u32
puthost(h, p, n, fp)
	struct hostent *h;
	u32 p, n, fp;
{
	u32 na = 0, nl = 0, i, s, oal, oad, o;

	for (i = 0; h->h_aliases && h->h_aliases[i] && i < 8; i++)
		na++;
	for (i = 0; h->h_addr_list && h->h_addr_list[i] && i < 8; i++)
		nl++;
	oal = 16 + 4 * (na + 1);	/* h_addr_list's array */
	oad = oal + 4 * (nl + 1);	/* the addresses */
	s = oad + 4 * nl + strlen(h->h_name) + 1;
	for (i = 0; i < na; i++)
		s += strlen(h->h_aliases[i]) + 1;
	if (s > n)
		return 0;
	PL(p + 4, fp + 16);
	PW(p + 8, 2);			/* AF_INET */
	PW(p + 10, 4);
	PL(p + 12, fp + oal);
	for (i = 0; i < nl; i++) {
		PL(p + oal + 4 * i, fp + oad + 4 * i);
		memcpy(M + p + oad + 4 * i, h->h_addr_list[i], 4);
	}
	PL(p + oal + 4 * nl, 0);
	o = oad + 4 * nl;
	PL(p, fp + o);
	strcpy((char *)M + p + o, h->h_name);
	o += strlen(h->h_name) + 1;
	for (i = 0; i < na; i++) {
		PL(p + 16 + 4 * i, fp + o);
		strcpy((char *)M + p + o, h->h_aliases[i]);
		o += strlen(h->h_aliases[i]) + 1;
	}
	PL(p + 16 + 4 * na, 0);
	return o;
}

static u32
putserv(sv, p, n, fp)
	struct servent *sv;
	u32 p, n, fp;
{
	u32 o = 14;

	if (14 + 4 + strlen(sv->s_name) + 1 + strlen(sv->s_proto) + 1 > n)
		return 0;
	/* s_name, s_aliases (none), s_port (as it lies), s_proto */
	PL(p + 4, fp + o);
	PL(p + o, 0);
	o += 4;
	PB(p + 8, ntohs((unsigned short)sv->s_port) >> 8);	/* network order, as it lies */
	PB(p + 9, ntohs((unsigned short)sv->s_port) & 0xff);
	PL(p, fp + o);
	strcpy((char *)M + p + o, sv->s_name);
	o += strlen(sv->s_name) + 1;
	PL(p + 10, fp + o);
	strcpy((char *)M + p + o, sv->s_proto);
	return o + strlen(sv->s_proto) + 1;
}

static u32
putproto(pr, p, n, fp)
	struct protoent *pr;
	u32 p, n, fp;
{
	u32 o = 10;

	if (10 + 4 + strlen(pr->p_name) + 1 > n)
		return 0;
	PL(p + 4, fp + o);
	PL(p + o, 0);
	o += 4;
	PW(p + 8, pr->p_proto);
	PL(p, fp + o);
	strcpy((char *)M + p + o, pr->p_name);
	return o + strlen(pr->p_name) + 1;
}

static u32
dbret(n)
	u32 n;
{
	return n ? FP(dbsel, 0) : 0;
}

static u32
w_gethostbyname(a)
	u32 *a;
{
	struct hostent *h;
	char *s = gptr(a[0]);

	if (!started)
		return seterr(WSANOTINITIALISED), 0;
	if (!s || (h = gethostbyname(s)) == 0 || h->h_addrtype != AF_INET)
		return seterr(WSAHOST_NOT_FOUND), 0;
	return dbret(puthost(h, sel_base(dbsel), (u32)1900, FP(dbsel, 0)));
}

static u32
w_gethostbyaddr(a)
	u32 *a;
{
	struct hostent *h;
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0]));

	if (!started)
		return seterr(WSANOTINITIALISED), 0;
	if (!p || (a[1] & 0xffff) < 4 || (h = gethostbyaddr((char *)M + p, 4, AF_INET)) == 0)
		return seterr(WSAHOST_NOT_FOUND), 0;
	return dbret(puthost(h, sel_base(dbsel), (u32)1900, FP(dbsel, 0)));
}

static u32
w_getservbyname(a)
	u32 *a;
{
	struct servent *sv;

	if (!gptr(a[0]) || (sv = getservbyname(gptr(a[0]), gptr(a[1]) ? (char *)gptr(a[1]) : (char *)0)) == 0)
		return seterr(WSANO_DATA), 0;
	return dbret(putserv(sv, sel_base(dbsel), (u32)1900, FP(dbsel, 0)));
}

static u32
w_getservbyport(a)
	u32 *a;
{
	struct servent *sv;
	unsigned short port;

	port = htons((unsigned short)swap16(a[0]));	/* the program's network order to the host's */
	if ((sv = getservbyport((int)port, gptr(a[1]) ? (char *)gptr(a[1]) : (char *)0)) == 0)
		return seterr(WSANO_DATA), 0;
	return dbret(putserv(sv, sel_base(dbsel), (u32)1900, FP(dbsel, 0)));
}

static u32
w_getprotobyname(a)
	u32 *a;
{
	struct protoent *pr;

	if (!gptr(a[0]) || (pr = getprotobyname(gptr(a[0]))) == 0)
		return seterr(WSANO_DATA), 0;
	return dbret(putproto(pr, sel_base(dbsel), (u32)1900, FP(dbsel, 0)));
}

static u32
w_getprotobynumber(a)
	u32 *a;
{
	struct protoent *pr;

	if ((pr = getprotobynumber((int)(a[0] & 0xffff))) == 0)
		return seterr(WSANO_DATA), 0;
	return dbret(putproto(pr, sel_base(dbsel), (u32)1900, FP(dbsel, 0)));
}

static u32
w_gethostname(a)
	u32 *a;
{
	char buf[256];
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0]));

	if (!p)
		return fail(WSAEFAULT);
	if (gethostname(buf, sizeof buf) < 0)
		return fail(wserr(errno));
	buf[sizeof buf - 1] = 0;
	if (strlen(buf) + 1 > (a[1] & 0xffff))
		return fail(WSAEFAULT);
	strcpy((char *)M + p, buf);
	return 0;
}

/*
 * The asynchronous database calls, done at once: the answer in the
 * program's buffer and posted (wParam the handle, lParam its size and
 * the error).
 */
static u16 nasync;

static u32
asyncdone(hwnd, msg, n, err)
	u32 hwnd, msg, n;
	int err;
{
	if (++nasync == 0)
		nasync = 1;
	wnd_post(hwnd, msg, nasync, FP(err, err ? 0 : n));
	return nasync;
}

static u32
w_WSAAsyncGetHostByName(a)
	u32 *a;
{
	struct hostent *h;
	char *s = gptr(a[2]);
	u32 p = lin(FPSEL(a[3]), FPOFF(a[3])), n = 0;

	if (!started)
		return seterr(WSANOTINITIALISED), 0;
	if (s && p && (h = gethostbyname(s)) != 0 && h->h_addrtype == AF_INET)
		n = puthost(h, p, a[4] & 0xffff, a[3]);
	return asyncdone(a[0], a[1], n, n ? 0 : WSAHOST_NOT_FOUND);
}

static u32
w_WSAAsyncGetHostByAddr(a)
	u32 *a;
{
	struct hostent *h;
	u32 q = lin(FPSEL(a[2]), FPOFF(a[2])), p = lin(FPSEL(a[5]), FPOFF(a[5])), n = 0;

	if (!started)
		return seterr(WSANOTINITIALISED), 0;
	if (q && p && (a[3] & 0xffff) >= 4 && (h = gethostbyaddr((char *)M + q, 4, AF_INET)) != 0)
		n = puthost(h, p, a[6] & 0xffff, a[5]);
	return asyncdone(a[0], a[1], n, n ? 0 : WSAHOST_NOT_FOUND);
}

static u32
w_WSAAsyncGetServByName(a)
	u32 *a;
{
	struct servent *sv;
	u32 p = lin(FPSEL(a[4]), FPOFF(a[4])), n = 0;

	if (gptr(a[2]) && p && (sv = getservbyname(gptr(a[2]), gptr(a[3]) ? (char *)gptr(a[3]) : (char *)0)) != 0)
		n = putserv(sv, p, a[5] & 0xffff, a[4]);
	return asyncdone(a[0], a[1], n, n ? 0 : WSANO_DATA);
}

static u32
w_WSAAsyncGetServByPort(a)
	u32 *a;
{
	struct servent *sv;
	u32 p = lin(FPSEL(a[4]), FPOFF(a[4])), n = 0;

	if (p && (sv = getservbyport((int)htons((unsigned short)swap16(a[2])), gptr(a[3]) ? (char *)gptr(a[3]) :
	    (char *)0)) != 0)
		n = putserv(sv, p, a[5] & 0xffff, a[4]);
	return asyncdone(a[0], a[1], n, n ? 0 : WSANO_DATA);
}

static u32
w_WSAAsyncGetProtoByName(a)
	u32 *a;
{
	struct protoent *pr;
	u32 p = lin(FPSEL(a[3]), FPOFF(a[3])), n = 0;

	if (gptr(a[2]) && p && (pr = getprotobyname(gptr(a[2]))) != 0)
		n = putproto(pr, p, a[4] & 0xffff, a[3]);
	return asyncdone(a[0], a[1], n, n ? 0 : WSANO_DATA);
}

static u32
w_WSAAsyncGetProtoByNumber(a)
	u32 *a;
{
	struct protoent *pr;
	u32 p = lin(FPSEL(a[3]), FPOFF(a[3])), n = 0;

	if (p && (pr = getprotobynumber((int)(a[2] & 0xffff))) != 0)
		n = putproto(pr, p, a[4] & 0xffff, a[3]);
	return asyncdone(a[0], a[1], n, n ? 0 : WSANO_DATA);
}

static u32 w_WSACancelAsyncRequest(a) u32 *a; { return fail(WSAEALREADY); }

/* a task that ends: its sockets close */
void
ws_taskended(t)
	struct task *t;
{
	int i;

	for (i = 0; i < NSOCK; i++)
		if (socks[i].used && socks[i].task == t) {
			close(socks[i].fd);
			socks[i].used = 0;
		}
}

struct impl ws_impl[] = {
	{ "WINSOCK", "accept", w_accept },
	{ "WINSOCK", "bind", w_bind },
	{ "WINSOCK", "closesocket", w_closesocket },
	{ "WINSOCK", "connect", w_connect },
	{ "WINSOCK", "getpeername", w_getpeername },
	{ "WINSOCK", "getsockname", w_getsockname },
	{ "WINSOCK", "getsockopt", w_getsockopt },
	{ "WINSOCK", "htonl", w_htonl },
	{ "WINSOCK", "htons", w_htons },
	{ "WINSOCK", "inet_addr", w_inet_addr },
	{ "WINSOCK", "inet_ntoa", w_inet_ntoa },
	{ "WINSOCK", "ioctlsocket", w_ioctlsocket },
	{ "WINSOCK", "listen", w_listen },
	{ "WINSOCK", "ntohl", w_htonl },
	{ "WINSOCK", "ntohs", w_htons },
	{ "WINSOCK", "recv", w_recv },
	{ "WINSOCK", "recvfrom", w_recvfrom },
	{ "WINSOCK", "select", w_select },
	{ "WINSOCK", "send", w_send },
	{ "WINSOCK", "sendto", w_sendto },
	{ "WINSOCK", "setsockopt", w_setsockopt },
	{ "WINSOCK", "shutdown", w_shutdown },
	{ "WINSOCK", "socket", w_socket },
	{ "WINSOCK", "gethostbyaddr", w_gethostbyaddr },
	{ "WINSOCK", "gethostbyname", w_gethostbyname },
	{ "WINSOCK", "getprotobyname", w_getprotobyname },
	{ "WINSOCK", "getprotobynumber", w_getprotobynumber },
	{ "WINSOCK", "getservbyname", w_getservbyname },
	{ "WINSOCK", "getservbyport", w_getservbyport },
	{ "WINSOCK", "gethostname", w_gethostname },
	{ "WINSOCK", "WSAAsyncSelect", w_WSAAsyncSelect },
	{ "WINSOCK", "WSAAsyncGetHostByAddr", w_WSAAsyncGetHostByAddr },
	{ "WINSOCK", "WSAAsyncGetHostByName", w_WSAAsyncGetHostByName },
	{ "WINSOCK", "WSAAsyncGetProtoByNumber", w_WSAAsyncGetProtoByNumber },
	{ "WINSOCK", "WSAAsyncGetProtoByName", w_WSAAsyncGetProtoByName },
	{ "WINSOCK", "WSAAsyncGetServByPort", w_WSAAsyncGetServByPort },
	{ "WINSOCK", "WSAAsyncGetServByName", w_WSAAsyncGetServByName },
	{ "WINSOCK", "WSACancelAsyncRequest", w_WSACancelAsyncRequest },
	{ "WINSOCK", "WSASetBlockingHook", w_WSASetBlockingHook },
	{ "WINSOCK", "WSAUnhookBlockingHook", w_WSAUnhookBlockingHook },
	{ "WINSOCK", "WSAGetLastError", w_WSAGetLastError },
	{ "WINSOCK", "WSASetLastError", w_WSASetLastError },
	{ "WINSOCK", "WSACancelBlockingCall", w_WSACancelBlockingCall },
	{ "WINSOCK", "WSAIsBlocking", w_WSAIsBlocking },
	{ "WINSOCK", "WSAStartup", w_WSAStartup },
	{ "WINSOCK", "WSACleanup", w_WSACleanup },
	{ "WINSOCK", "__WSAFDIsSet", w_WSAFDIsSet },
	{ 0 }
};
