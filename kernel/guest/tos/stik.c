/*
 * stik.c -- a STiK/STinG transport layer over the host's sockets.
 * The `STiK' cookie's DRV_LIST hands out the TPL table; each call
 * becomes socket calls of the container's own (TOSIOC_SOCK), so the
 * host's network identity and the user's rights apply.  Descriptors
 * are nonblocking; clients poll, as with STiK.
 *
 * Arguments arrive packed as Pure C's cdecl pushes them (words for
 * 16-bit values, a char in the low byte of a word); the glue in tosml.s
 * passes their address.  K&R C.
 */

#include <tosio.h>
#include <pingio.h>

#define	G16(p)	(*(unsigned short *)(p))
#define	G32(p)	(*(unsigned long *)(p))
#define	S16(a, o)	((short)G16((a) + (o)))

#define	E_NORMAL	0
#define	E_OBUFFULL	-1
#define	E_NODATA	-2
#define	E_EOF		-3
#define	E_RRESET	-4
#define	E_NOMEM		-6
#define	E_REFUSE	-7
#define	E_BADHANDLE	-9
#define	E_LISTEN	-10
#define	E_NOCCB		-11
#define	E_NOCONNECTION	-12
#define	E_CONNECTFAIL	-13
#define	E_USERTIMEOUT	-15
#define	E_CNTIMEOUT	-16
#define	E_CANTRESOLVE	-17
#define	E_BADDNAME	-18
#define	E_NOHOSTNAME	-20
#define	E_NONAMESERVER	-22
#define	E_UNREACHABLE	-24
#define	E_DNSNOADDR	-25
#define	E_NOROUTINE	-26
#define	E_PARAMETER	-30
#define	E_BIGBUF	-31
#define	E_FNAVAIL	-32

#define	TCLOSED		0
#define	TLISTEN		1
#define	TSYN_SENT	2
#define	TESTABLISH	4
#define	TCLOSE_WAIT	7

/* SVR4 */
#define	SOCK_DGRAM	1
#define	SOCK_STREAM	2
#define	EINTR		4
#define	EAGAIN		11
#define	ENETUNREACH	128
#define	ETIMEDOUT	145
#define	ECONNREFUSED	146
#define	EHOSTUNREACH	148
#define	EINPROGRESS	150
#define	F_SETFL		4
#define	O_RDWR		2
#define	O_NONBLOCK	0x80
#define	I_NREAD		0x5301
#define	POLLIN		1
#define	POLLOUT		4

#define	NCN	16
#define	RBUF	2048
#define	POOL	(48 * 1024)

extern long sys_read(), sys_write(), sys_open(), sys_close(), sys_ioctl(), sys_fcntl(), sys_poll(),
    sys_time();
extern long stik_call();
extern long p_tfd;
extern char stik_drv[], stik_tpl[];

struct cn {
	int	fd;		/* -1: none */
	int	lfd;		/* listening until a peer comes */
	int	used, udp, state, err, eof;
	char	*rb;		/* received data: rb[rh] on, rn bytes */
	int	rh, rn;
	char	cib[16];
};

static struct cn cn[NCN];
static struct tossock so;
static char gap[576];
static char cfg[512];		/* STIK_CONFIG, zeroed: no serial line */
static char flags[64];
#define	NJAR	64
static long njar[NJAR * 2];

/* ---- KRmalloc: a first-fit free list over a fixed pool ---- */

struct hdr {
	struct hdr *next;
	unsigned long n;	/* in units */
};

static long pool[POOL / 4];
static struct hdr base, *freep;

static char *
kralloc(len)
	long len;
{
	struct hdr *p, *q;
	unsigned long n;

	if (len <= 0)
		return 0;
	n = (len + sizeof (struct hdr) - 1) / sizeof (struct hdr) + 1;
	if (freep == 0) {
		p = (struct hdr *)pool;
		p->n = POOL / sizeof (struct hdr);
		base.next = p;
		p->next = &base;
		freep = &base;
	}
	for (q = freep, p = q->next;; q = p, p = p->next) {
		if (p->n >= n) {
			if (p->n == n)
				q->next = p->next;
			else {
				p->n -= n;
				p += p->n;
				p->n = n;
			}
			freep = q;
			return (char *)(p + 1);
		}
		if (p == freep)
			return 0;
	}
}

/* ap could be a block kralloc returned */
static int
inpool(ap)
	char *ap;
{
	return ap && freep && ap > (char *)pool && ap < (char *)pool + POOL &&
	    (ap - (char *)pool) % sizeof (struct hdr) == 0;
}

static void
krfree(ap)
	char *ap;
{
	struct hdr *b, *p;

	if (!inpool(ap))
		return;
	b = (struct hdr *)ap - 1;
	for (p = freep; !(b > p && b < p->next); p = p->next) {
		if (b == p->next)
			return;
		if (p >= p->next && (b > p || b < p->next))
			break;
	}
	/* already free, or overlapping a free block */
	if ((b > p && b < p + p->n) ||
	    (p->next != &base && b < p->next && b + b->n > p->next))
		return;
	if (b + b->n == p->next) {
		b->n += p->next->n;
		b->next = p->next->next;
	} else
		b->next = p->next;
	if (p + p->n == b) {
		p->n += b->n;
		p->next = b->next;
	} else
		p->next = b;
	freep = p;
}

static long
krgetfree(all)
	int all;
{
	struct hdr *p;
	unsigned long m = 0, t = 0;

	krfree(kralloc(1L));		/* the list exists */
	if ((p = freep) == 0)
		return 0;
	do {
		if (p != &base) {
			t += p->n;
			if (p->n > m)
				m = p->n;
		}
		p = p->next;
	} while (p != freep);
	return ((all ? t : m) - 1) * sizeof (struct hdr);
}

static void
cpy(d, s, n)
	char *d, *s;
	int n;
{
	while (n-- > 0)
		*d++ = *s++;
}

/* ---- host sockets ---- */

static int
sk(op, fd)
	int op, fd;
{
	so.so_op = op;
	so.so_fd = fd;
	so.so_gap = gap;
	return (int)-sys_ioctl(p_tfd, (long)TOSIOC_SOCK, (long)&so);
}

static void
sin(a, port)
	unsigned long a;
	int port;
{
	int i;

	for (i = 0; i < 16; i++)
		so.so_addr[i] = 0;
	so.so_addr[1] = 2;
	so.so_addr[2] = port >> 8;
	so.so_addr[3] = port;
	*(unsigned long *)(so.so_addr + 4) = a;
	so.so_alen = 16;
}

static int
newsock(type)
	int type;
{
	so.so_arg = type;
	so.so_alen = 0;
	if (sk(TSO_SOCKET, 0))
		return -1;
	sys_fcntl(so.so_rv, (long)F_SETFL, (long)O_NONBLOCK);
	return (int)so.so_rv;
}

static int
emap(e)
	int e;
{
	switch (e) {
	case ECONNREFUSED:
		return E_REFUSE;
	case ENETUNREACH:
	case EHOSTUNREACH:
		return E_UNREACHABLE;
	case ETIMEDOUT:
		return E_CNTIMEOUT;
	}
	return E_CONNECTFAIL;
}

/* poll fd for ev, up to ms; with no descriptor just sleep */
static void
nap(fd, ev, ms)
	int fd, ev, ms;
{
	long p[2];

	p[0] = fd;
	p[1] = ev << 16;
	sys_poll(fd < 0 ? 0L : (long)p, fd < 0 ? 0L : 1L, (long)ms);
}

static struct cn *
cnget(h)
	int h;
{
	return h >= 0 && h < NCN && cn[h].used ? &cn[h] : 0;
}

static struct cn *
cnnew()
{
	struct cn *c;

	for (c = cn; c < cn + NCN; c++)
		if (!c->used) {
			if ((c->rb = kralloc((long)RBUF)) == 0)
				return 0;
			c->used = 1;
			c->fd = c->lfd = -1;
			c->udp = c->state = c->err = c->eof = c->rh = c->rn = 0;
			return c;
		}
	return 0;
}

static void
cnfree(c)
	struct cn *c;
{
	if (c->fd >= 0)
		sys_close((long)c->fd);
	if (c->lfd >= 0)
		sys_close((long)c->lfd);
	krfree(c->rb);
	c->used = 0;
}

/* a connect or listen in progress moves on */
static void
upd(c)
	struct cn *c;
{
	int e;

	if (c->state == TSYN_SENT) {
		e = sk(TSO_CONNWAIT, c->fd);
		if (e == EINTR || (e == 0 && so.so_rv == -1))
			return;
		if (e == 0 && so.so_rv == 0)
			c->state = TESTABLISH;
		else {
			c->state = TCLOSED;
			c->err = emap(e ? e : (int)so.so_rv);
		}
	} else if (c->state == TLISTEN) {
		so.so_alen = 0;
		if (sk(TSO_ACCEPT, c->lfd) == 0) {
			c->fd = (int)so.so_rv;
			sys_fcntl((long)c->fd, (long)F_SETFL, (long)O_NONBLOCK);
			sys_close((long)c->lfd);
			c->lfd = -1;
			c->state = TESTABLISH;
		}
	}
}

/* what arrived since; one datagram at a time for UDP */
static void
fill(c)
	struct cn *c;
{
	int e;

	upd(c);
	if (c->fd < 0 || c->eof || c->state == TSYN_SENT || c->state == TCLOSED)
		return;
	if (c->rn == 0)
		c->rh = 0;
	if (c->udp && c->rn)
		return;
	if (c->rh && c->rh + c->rn == RBUF) {
		cpy(c->rb, c->rb + c->rh, c->rn);
		c->rh = 0;
	}
	if (c->rh + c->rn == RBUF)
		return;
	so.so_buf = c->rb + c->rh + c->rn;
	so.so_len = RBUF - c->rh - c->rn;
	so.so_arg = 0;
	so.so_alen = 0;
	if ((e = sk(TSO_RECV, c->fd)) == 0 && (so.so_rv > 0 || c->udp))
		c->rn += so.so_rv;
	else if (e == 0) {
		c->eof = 1;
		if (c->state == TESTABLISH)
			c->state = TCLOSE_WAIT;
	} else if (e != EAGAIN && e != EINTR) {
		c->eof = 1;
		c->err = e == ECONNREFUSED ? E_REFUSE : E_RRESET;
		c->state = TCLOSED;
	}
}

/* ---- the TPL calls: a points at the arguments ---- */

long
st_KRmalloc(a)
	char *a;
{
	return (long)kralloc((long)G32(a));
}

long
st_KRfree(a)
	char *a;
{
	krfree((char *)G32(a));
	return 0;
}

long
st_KRgetfree(a)
	char *a;
{
	return krgetfree(S16(a, 0));
}

long
st_KRrealloc(a)
	char *a;
{
	char *o = (char *)G32(a), *n;
	long len = G32(a + 4), have;

	if (o == 0)
		return (long)kralloc(len);
	if (len == 0) {
		krfree(o);
		return 0;
	}
	if (!inpool(o))
		return 0;
	have = (((struct hdr *)o - 1)->n - 1) * sizeof (struct hdr);
	if ((n = kralloc(len)) != 0) {
		cpy(n, o, (int)(have < len ? have : len));
		krfree(o);
	}
	return (long)n;
}

static char *errs[] = {
	"No error", "Output buffer is full", "No data available", "EOF from remote",
	"Reset received from remote", "Unacceptable packet received, reset",
	"Something failed due to lack of memory", "Connection refused by remote",
	"A SYN was received in the window", "Bad connection handle used",
	"The connection is in LISTEN state", "No free CCB's available",
	"No connection matches this packet (TCP)", "Failure to connect to remote port (TCP)",
	"Invalid TCP_close() requested", "A user function timed out",
	"A connection timed out", "Can't resolve the hostname",
	"Domain name or dotted dec. bad format", "The modem disconnected",
	"Hostname does not exist", "Resolver Work limit reached",
	"No nameservers could be found for query", "Bad format of DS query",
	"Destination unreachable", "No address records exist for host",
	"Routine unavailable", "Locked by another application",
	"Error during fragmentation", "Time To Live of an IP packet exceeded",
	"Problem with a parameter", "Input buffer is too small for data",
	"Function not available"
};

long
st_get_err_text(a)
	char *a;
{
	int e = -S16(a, 0);

	return (long)(e >= 0 && e < sizeof errs / sizeof errs[0] ? errs[e] : "Unknown error");
}

/* configuration variables: defaults, then setvstr's */
#define	NV	24
static char vn[NV][24], vv[NV][64];

static int
streqi(x, y)
	char *x, *y;
{
	int c, d;

	do {
		c = *x++;
		d = *y++;
		if (c >= 'a' && c <= 'z')
			c -= 32;
		if (d >= 'a' && d <= 'z')
			d -= 32;
		if (c != d)
			return 0;
	} while (c);
	return 1;
}

static int
setv(n, v)
	char *n, *v;
{
	int i, k;

	for (i = 0; i < NV && vn[i][0] && !streqi(vn[i], n); i++)
		;
	if (i == NV)
		return 0;
	for (k = 0; k < 23 && n[k]; k++)
		vn[i][k] = n[k];
	vn[i][k] = 0;
	for (k = 0; k < 63 && v[k]; k++)
		vv[i][k] = v[k];
	vv[i][k] = 0;
	return 1;
}

long
st_getvstr(a)
	char *a;
{
	char *n = (char *)G32(a);
	int i;

	for (i = 0; n && i < NV && vn[i][0]; i++)
		if (streqi(vn[i], n))
			return (long)vv[i];
	return (long)"0";
}

long
st_setvstr(a)
	char *a;
{
	char *n = (char *)G32(a), *v = (char *)G32(a + 4);

	return n && v && *n ? setv(n, v) : 0;
}

long
st_carrier_detect(a)
	char *a;
{
	return 1;
}

/* a CAB: lport, rport, rhost, lhost */
static int
open1(udp, rhost, rport, lport, passive)
	unsigned long rhost;
	int udp, rport, lport, passive;
{
	struct cn *c;
	int fd, e;

	if ((c = cnnew()) == 0)
		return E_NOCCB;
	if ((fd = newsock(udp ? SOCK_DGRAM : SOCK_STREAM)) < 0) {
		cnfree(c);
		return E_NOMEM;
	}
	c->udp = udp;
	c->fd = fd;
	c->cib[1] = udp ? 17 : 6;
	if (lport) {
		sin(0L, lport);
		if (sk(TSO_BIND, fd)) {
			cnfree(c);
			return E_PARAMETER;
		}
	}
	if (passive) {
		so.so_arg = 1;
		if (sk(TSO_LISTEN, fd)) {
			cnfree(c);
			return E_PARAMETER;
		}
		c->lfd = fd;
		c->fd = -1;
		c->state = TLISTEN;
	} else if (rhost) {
		sin(rhost, rport);
		e = sk(TSO_CONNECT, fd);
		if (e && (udp || e != EINPROGRESS)) {
			cnfree(c);
			return udp ? E_UNREACHABLE : emap(e);
		}
		c->state = e ? TSYN_SENT : TESTABLISH;
	} else
		c->state = TESTABLISH;
	return c - cn;
}

long
st_TCP_open(a)
	char *a;
{
	unsigned long rh = G32(a);
	int rp = G16(a + 4);
	char *cab = (char *)rh;

	if ((rp == 0 || rp == 0xffff) && rh)
		return open1(0, G32(cab + 4), G16(cab + 2), G16(cab), rp == 0xffff);
	if (rh == 0)
		return open1(0, 0L, 0, rp, 1);
	return open1(0, rh, rp, 0, 0);
}

long
st_TCP_close(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));
	short *r = (short *)G32(a + 4);

	if (c == 0 || c->udp)
		return E_BADHANDLE;
	cnfree(c);
	if (r)
		*r = E_NORMAL;
	return E_NORMAL;
}

/* all of buf, or E_OBUFFULL when none of it fits */
static long
send1(c, buf, len)
	struct cn *c;
	char *buf;
	int len;
{
	int e, n = 0, tries = 0;

	upd(c);
	if (c->state == TSYN_SENT || c->state == TLISTEN)
		return E_OBUFFULL;
	if (c->fd < 0 || (c->state != TESTABLISH && c->state != TCLOSE_WAIT))
		return c->err ? c->err : E_NOCONNECTION;
	while (n < len) {
		so.so_buf = buf + n;
		so.so_len = len - n;
		so.so_arg = 0;
		so.so_alen = 0;
		if ((e = sk(TSO_SEND, c->fd)) == 0)
			n += so.so_rv;
		else if (e == EAGAIN || e == EINTR) {
			if (n == 0 && !c->udp)
				return E_OBUFFULL;
			if (++tries > 200)
				return E_CNTIMEOUT;
			nap(c->fd, POLLOUT, 50);
		} else {
			if (c->udp)
				return E_UNREACHABLE;
			c->state = TCLOSED;
			c->err = E_RRESET;
			return E_RRESET;
		}
	}
	return E_NORMAL;
}

long
st_TCP_send(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));

	if (c == 0 || c->udp)
		return E_BADHANDLE;
	return send1(c, (char *)G32(a + 2), S16(a, 6));
}

long
st_TCP_wait_state(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));
	int want = S16(a, 2);
	long t0 = sys_time();

	if (c == 0 || c->udp)
		return E_BADHANDLE;
	for (;;) {
		fill(c);
		if (c->state == want)
			return E_NORMAL;
		if (c->state == TCLOSED)
			return c->err ? c->err : E_NOCONNECTION;
		if (sys_time() - t0 >= S16(a, 4))
			return E_USERTIMEOUT;
		nap(c->fd >= 0 ? c->fd : c->lfd, POLLIN | POLLOUT, 50);
	}
}

long
st_TCP_ack_wait(a)
	char *a;
{
	return cnget(S16(a, 0)) ? E_NORMAL : E_BADHANDLE;
}

long
st_UDP_open(a)
	char *a;
{
	unsigned long rh = G32(a);
	int rp = G16(a + 4);
	char *cab = (char *)rh;

	if (rp == 0 && rh)
		return open1(1, G32(cab + 4), G16(cab + 2), G16(cab), 0);
	return open1(1, rh, rp, 0, 0);
}

long
st_UDP_close(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));

	if (c == 0 || !c->udp)
		return E_BADHANDLE;
	cnfree(c);
	return E_NORMAL;
}

long
st_UDP_send(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));

	if (c == 0 || !c->udp)
		return E_BADHANDLE;
	return send1(c, (char *)G32(a + 2), S16(a, 6));
}

long
st_CNkick(a)
	char *a;
{
	return cnget(S16(a, 0)) ? E_NORMAL : E_BADHANDLE;
}

/* bytes waiting, else 0 or why there will be none */
static int
avail(c)
	struct cn *c;
{
	fill(c);
	if (c->state == TLISTEN)
		return E_LISTEN;
	if (c->rn)
		return c->rn > 32767 ? 32767 : c->rn;
	if (c->eof || c->state == TCLOSED)
		return c->err ? c->err : E_EOF;
	return 0;
}

long
st_CNbyte_count(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));

	return c ? avail(c) : E_BADHANDLE;
}

static void
take(c, n)
	struct cn *c;
	int n;
{
	c->rh += n;
	c->rn -= n;
}

long
st_CNget_char(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));
	int n;

	if (c == 0)
		return E_BADHANDLE;
	if ((n = avail(c)) <= 0)
		return n ? n : E_NODATA;
	n = c->rb[c->rh] & 0xff;
	take(c, 1);
	return n;
}

long
st_CNget_NDB(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));
	char *d, *ndb;

	if (c == 0 || avail(c) <= 0)
		return 0;
	if ((ndb = kralloc(14L)) == 0 || (d = kralloc((long)c->rn)) == 0) {
		krfree(ndb);
		return 0;
	}
	cpy(d, c->rb + c->rh, c->rn);
	G32(ndb) = G32(ndb + 4) = (unsigned long)d;
	G16(ndb + 8) = c->rn;
	G32(ndb + 10) = 0;
	take(c, c->rn);
	return (long)ndb;
}

long
st_CNget_block(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));
	int n, len = S16(a, 6);

	if (c == 0)
		return E_BADHANDLE;
	if (len <= 0)
		return E_PARAMETER;
	if ((n = avail(c)) < 0)
		return n;
	if (n < len)
		return c->eof && c->udp == 0 && n == 0 ? E_EOF : E_NODATA;
	cpy((char *)G32(a + 2), c->rb + c->rh, len);
	take(c, c->udp ? c->rn : len);
	return len;
}

long
st_CNgets(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));
	char *buf = (char *)G32(a + 2), dl = a[9];
	int i, n, len = S16(a, 6);

	if (c == 0)
		return E_BADHANDLE;
	if ((n = avail(c)) <= 0)
		return n ? n : E_NODATA;
	for (i = 0; i < c->rn && c->rb[c->rh + i] != dl; i++)
		;
	if (i == c->rn)
		return c->rn >= len ? E_BIGBUF : c->eof ? E_EOF : E_NODATA;
	if (i >= len)
		return E_BIGBUF;
	cpy(buf, c->rb + c->rh, i);
	buf[i] = 0;
	take(c, i + 1);
	return i;
}

long
st_housekeep(a)
	char *a;
{
	return 0;
}

/* ---- resolve: dotted quads, the hosts file, then DNS ---- */

static int
quad(s, ap)
	char *s;
	unsigned long *ap;
{
	unsigned long a = 0, v;
	int i;

	for (i = 0; i < 4; i++) {
		if (*s < '0' || *s > '9')
			return 0;
		for (v = 0; *s >= '0' && *s <= '9'; s++)
			v = v * 10 + *s - '0';
		if (v > 255 || (i < 3 ? *s++ != '.' : *s != 0))
			return 0;
		a = a << 8 | v;
	}
	*ap = a;
	return 1;
}

static char fb[1024];
static int ffd = -1, fpos, flen;

/* the next line of an open file, comments and line end cut off */
static char *
line(l, n)
	char *l;
	int n;
{
	int k = 0, c;

	for (;;) {
		if (fpos == flen) {
			fpos = 0;
			if ((flen = (int)sys_read((long)ffd, (long)fb, (long)sizeof fb)) <= 0) {
				flen = 0;
				if (k == 0)
					return 0;
				break;
			}
		}
		if ((c = fb[fpos++]) == '\n')
			break;
		if (k < n - 1)
			l[k++] = c;
	}
	l[k] = 0;
	for (k = 0; l[k] && l[k] != '#'; k++)
		if (l[k] == '\t' || l[k] == '\r')
			l[k] = ' ';
	l[k] = 0;
	return l;
}

static int
fopen1(name)
	char *name;
{
	fpos = flen = 0;
	return (ffd = (int)sys_open((long)name, 0L, 0L)) >= 0;
}

/* the next blank-separated word of *sp */
static char *
word(sp)
	char **sp;
{
	char *s = *sp, *w;

	while (*s == ' ')
		s++;
	if (*s == 0)
		return 0;
	for (w = s; *s && *s != ' '; s++)
		;
	if (*s)
		*s++ = 0;
	*sp = s;
	return w;
}

static char rname[256];

static int
hosts(dn, ap)
	char *dn;
	unsigned long *ap;
{
	char l[256], *s, *w, *cn;
	unsigned long a;
	int found = 0, k;

	if (!fopen1("/etc/hosts"))
		return 0;
	while (!found && line(l, sizeof l)) {
		s = l;
		if ((w = word(&s)) == 0 || !quad(w, &a))
			continue;
		for (cn = 0; (w = word(&s)) != 0; ) {
			if (cn == 0)
				cn = w;
			if (streqi(w, dn)) {
				*ap = a;
				for (k = 0; cn[k] && k < 255; k++)
					rname[k] = cn[k];
				rname[k] = 0;
				found = 1;
				break;
			}
		}
	}
	sys_close((long)ffd);
	return found;
}

static unsigned char q[512];

/* A records for dn from the first name server that answers */
static int
dns(dn, list, max)
	char *dn;
	unsigned long *list;
	int max;
{
	char l[256], *s, *w;
	unsigned long ns[3];
	int nns = 0, fd, i, k, n, an, try, t, len, got = 0;
	unsigned char *p, *e;

	if (fopen1("/etc/resolv.conf")) {
		while (nns < 3 && line(l, sizeof l)) {
			s = l;
			if ((w = word(&s)) != 0 && streqi(w, "nameserver") && (w = word(&s)) &&
			    quad(w, &ns[nns]))
				nns++;
		}
		sys_close((long)ffd);
	}
	if (nns == 0)
		return E_NONAMESERVER;
	/* header: id, recursion desired, one question */
	for (i = 0; i < 12; i++)
		q[i] = 0;
	q[0] = 0x5a;
	q[1] = (unsigned char)sys_time();
	q[2] = 1;
	q[5] = 1;
	p = q + 12;
	for (s = dn; *s; ) {
		for (k = 0; s[k] && s[k] != '.'; k++)
			;
		if (k == 0 || k > 63 || p + k + 6 > q + 256)
			return E_BADDNAME;
		*p++ = k;
		cpy((char *)p, s, k);
		p += k;
		s += k;
		if (*s)
			s++;
	}
	*p++ = 0;
	*p++ = 0; *p++ = 1;		/* A */
	*p++ = 0; *p++ = 1;		/* IN */
	len = p - q;
	for (try = 0; try < 2 * nns && !got; try++) {
		if ((fd = newsock(SOCK_DGRAM)) < 0)
			return E_CANTRESOLVE;
		sin(ns[try % nns], 53);
		so.so_buf = (char *)q;
		so.so_len = len;
		so.so_arg = 0;
		if (sk(TSO_SEND, fd) == 0)
			for (t = 0; t < 40 && !got; t++) {
				nap(fd, POLLIN, 50);
				so.so_buf = (char *)q + 256;
				so.so_len = 256;
				so.so_alen = 0;
				/* the answer to this query, from the server asked */
				if (sk(TSO_RECV, fd) || so.so_rv < len || q[256] != q[0] ||
				    q[257] != q[1] || !(q[258] & 0x80) || q[260] || q[261] != 1 ||
				    so.so_addr[2] || so.so_addr[3] != 53 ||
				    G32(so.so_addr + 4) != ns[try % nns])
					continue;
				got = 1;
				p = q + 256;
				e = p + so.so_rv;
				if ((p[3] & 15) == 3)
					got = E_NOHOSTNAME;
				an = p[6] << 8 | p[7];
				p += len;	/* the question, echoed */
				for (n = 0; got == 1 && an-- > 0 && p + 12 <= e && n < max; ) {
					while (p < e && *p && *p < 0xc0)
						p += *p + 1;
					p += p < e && *p ? 2 : 1;
					if (p + 10 > e)
						break;
					k = p[8] << 8 | p[9];
					if (p[0] == 0 && p[1] == 1 && p[2] == 0 && p[3] == 1 &&
					    k == 4 && p + 14 <= e)
						cpy((char *)&list[n++], (char *)p + 10, 4);
					p += 10 + k;
				}
				if (got == 1)
					got = n ? n : E_DNSNOADDR;
			}
		sys_close((long)fd);
	}
	return got ? got : E_CANTRESOLVE;
}

long
st_resolve(a)
	char *a;
{
	char *dn = (char *)G32(a), **real = (char **)G32(a + 4), *r;
	unsigned long *list = (unsigned long *)G32(a + 8), ad;
	int max = S16(a, 12), n = 1, k;

	if (dn == 0 || *dn == 0)
		return E_BADDNAME;
	if (list == 0)
		max = 0;
	for (k = 0; dn[k] && k < 255; k++)
		rname[k] = dn[k];
	rname[k] = 0;
	if (!quad(dn, &ad) && !hosts(dn, &ad)) {
		if ((n = dns(dn, list, max)) <= 0)
			return n;
	} else if (max > 0)
		list[0] = ad;
	if (real) {
		for (k = 0; rname[k]; k++)
			;
		if ((r = kralloc((long)k + 1)) != 0)
			cpy(r, rname, k + 1);
		*real = r;
	}
	return n;
}

long
st_ser_disable(a)
	char *a;
{
	return 0;
}

long
st_set_flag(a)
	char *a;
{
	int f = S16(a, 0), o;

	if (f < 0 || f >= 64)
		return E_PARAMETER;
	o = flags[f];
	flags[f] = 1;
	return o;
}

long
st_clear_flag(a)
	char *a;
{
	int f = S16(a, 0);

	if (f >= 0 && f < 64)
		flags[f] = 0;
	return 0;
}

long
st_CNgetinfo(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));
	int fd;

	if (c == 0)
		return 0;
	upd(c);
	fd = c->fd >= 0 ? c->fd : c->lfd;
	so.so_alen = 0;
	so.so_arg = 0;
	if (sk(TSO_NAME, fd) == 0) {
		cpy(c->cib + 2, so.so_addr + 2, 2);
		cpy(c->cib + 10, so.so_addr + 4, 4);
	}
	so.so_arg = 1;
	if (c->state != TLISTEN && sk(TSO_NAME, fd) == 0) {
		cpy(c->cib + 4, so.so_addr + 2, 2);
		cpy(c->cib + 6, so.so_addr + 4, 4);
	}
	G16(c->cib + 14) = c->err ? -c->err : 0;
	return (long)c->cib;
}

/* one port, always up: the host's network */
long
st_on_port(a)
	char *a;
{
	return 1;
}

long
st_cntrl_port(a)
	char *a;
{
	return E_FNAVAIL;
}

long
st_TCP_info(a)
	char *a;
{
	struct cn *c = cnget(S16(a, 0));
	char *b = (char *)G32(a + 2);

	if (c == 0)
		return E_BADHANDLE;
	upd(c);
	if (b == 0)
		return E_PARAMETER;
	G16(b + 4) = c->udp ? 4 : c->state;	/* UESTABLISH */
	G32(b + 6) = 0;
	G32(b + 10) = 0;
	return 4;
}

long
st_CNfree_NDB(a)
	char *a;
{
	char *ndb = (char *)G32(a + 2);

	if (ndb) {
		krfree((char *)G32(ndb));
		krfree(ndb);
	}
	return 0;
}

/* ---- ICMP echo through the host's echo service ---- */

#define	NIH	8
static long ih[NIH];		/* the clients' ICMP handlers, latest first */
static int nih, pfd = -1;
static struct pingreq pq;
static struct pingrep pr;

/* the service's stream, opened on first use; a plain file there is refused */
static int
pingfd()
{
	long n;

	if (pfd < 0 && (pfd = (int)sys_open((long)PINGPATH, (long)(O_RDWR | O_NONBLOCK), 0L)) >= 0 &&
	    sys_ioctl((long)pfd, (long)I_NREAD, (long)&n) < 0) {
		sys_close((long)pfd);
		pfd = -1;
	}
	if (pfd < 0)
		pfd = -1;
	return pfd;
}

/* data follows the type, code and checksum: identifier, sequence, payload */
long
st_ICMP_send(a)
	char *a;
{
	unsigned long dst = G32(a);
	unsigned char *d = (unsigned char *)G32(a + 8);
	int len = G16(a + 12);

	if (a[5] != 8 || a[7] != 0)
		return E_FNAVAIL;
	if (dst == 0 || dst >> 24 == 0xe0)
		return E_BADDNAME;
	if (len < 4 || len - 4 > PING_DATA || d == 0)
		return E_PARAMETER;
	if (pingfd() < 0)
		return E_FNAVAIL;
	pq.pq_dst = dst;
	pq.pq_id = d[0] << 8 | d[1];
	pq.pq_seq = d[2] << 8 | d[3];
	pq.pq_len = len - 4;
	cpy(pq.pq_data, (char *)d + 4, len - 4);
	return sys_write((long)pfd, (long)&pq, (long)sizeof pq) == sizeof pq ? E_NORMAL : E_NOMEM;
}

long
st_ICMP_handler(a)
	char *a;
{
	long h = G32(a);
	int i, k;

	for (i = 0; i < nih && ih[i] != h; i++)
		;
	switch (S16(a, 4)) {
	case 0:		/* HNDLR_SET */
	case 1:		/* HNDLR_FORCE */
		if (h == 0 || i < nih || nih == NIH)
			return 0;
		for (k = nih++; k > 0; k--)
			ih[k] = ih[k - 1];
		ih[0] = h;
		pingfd();
		return 1;
	case 2:		/* HNDLR_REMOVE */
		if (i == nih)
			return 0;
		for (nih--; i < nih; i++)
			ih[i] = ih[i + 1];
		return 1;
	case 3:		/* HNDLR_QUERY */
		return i < nih;
	}
	return 0;
}

/* an IP_DGRAM: its data, its options, itself */
long
st_ICMP_discard(a)
	char *a;
{
	char *dg = (char *)G32(a);

	if (inpool(dg)) {
		krfree((char *)G32(dg + 26));
		krfree((char *)G32(dg + 20));
		krfree(dg);
	}
	return 0;
}

static unsigned short
cksum(p, n)
	unsigned char *p;
	int n;
{
	unsigned long s = 0;

	for (; n > 1; p += 2, n -= 2)
		s += p[0] << 8 | p[1];
	if (n)
		s += p[0] << 8;
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return ~s & 0xffff;
}

/*
 * Each VBL, outside any TPL call: echo replies become IP_DGRAMs for the
 * handlers, latest first, until one takes it.  Without handlers they
 * are dropped.
 */
void
st_icmppoll()
{
	unsigned char *pk;
	char *dg;
	int i, n, k;

	for (k = 0; pfd >= 0 && k < 4 && sys_read((long)pfd, (long)&pr, (long)sizeof pr) == sizeof pr; k++) {
		n = pr.pr_len > PING_DATA ? PING_DATA : pr.pr_len;
		if (nih == 0 || (dg = kralloc(48L)) == 0)
			continue;
		if ((pk = (unsigned char *)kralloc(8L + n)) == 0) {
			krfree(dg);
			continue;
		}
		for (i = 0; i < 48; i++)
			dg[i] = 0;
		dg[0] = 0x45;
		G16(dg + 2) = 28 + n;
		dg[8] = pr.pr_ttl;
		dg[9] = 1;
		G32(dg + 12) = pr.pr_src;
		G32(dg + 26) = (unsigned long)pk;
		G16(dg + 30) = 8 + n;
		pk[0] = pk[1] = pk[2] = pk[3] = 0;
		pk[4] = pr.pr_id >> 8;
		pk[5] = pr.pr_id;
		pk[6] = pr.pr_seq >> 8;
		pk[7] = pr.pr_seq;
		cpy((char *)pk + 8, pr.pr_data, n);
		G16(pk + 2) = cksum(pk, 8 + n);
		for (i = 0; i < nih; i++)
			if ((short)stik_call(ih[i], dg))
				break;
		if (i == nih)
			st_ICMP_discard((char *)&dg);
	}
}

long
st_RAW_open(a)
	char *a;
{
	return E_NOROUTINE;
}

long
st_RAW_close(a)
	char *a;
{
	return E_BADHANDLE;
}

long
st_get_dftab(a)
	char *a;
{
	char *n = (char *)G32(a);

	return n && streqi(n, "TRANSPORT_TCPIP") ? (long)stik_tpl : 0;
}

/* the `STiK' cookie, when the jar has room */
void
stikinit()
{
	long *jar = *(long **)0x5a0, *j;
	char l[64];
	int k;

	setv("HOSTNAME", "localhost");
	if (fopen1("/etc/nodename")) {
		if (line(l, sizeof l)) {
			for (k = 0; l[k] && l[k] != ' '; k++)
				;
			l[k] = 0;
			if (k)
				setv("HOSTNAME", l);
		}
		sys_close((long)ffd);
	}
	setv("ACTIVE", "1");
	G16(cfg + 8) = 64;		/* ttl */
	G32(stik_drv + 18) = (unsigned long)cfg;
	if (jar == 0)
		return;
	for (j = jar; j[0]; j += 2)
		if (j[0] == 0x5354694bL)
			return;
	if ((j - jar) / 2 + 1 >= j[1]) {
		/* full: move the cookies to a jar of our own */
		if (j - jar + 4 > NJAR * 2)
			return;
		for (k = 0; k < j - jar; k++)
			njar[k] = jar[k];
		j = njar + k;
		j[1] = NJAR;
		*(long **)0x5a0 = njar;
	}
	j[2] = 0;
	j[3] = j[1];
	j[0] = 0x5354694bL;
	j[1] = (long)stik_drv;
}
