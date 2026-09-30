/*
 * t_otb.c -- the otbridge module from the Open Transport side: this
 * program speaks OT-dialect records on host TCP/UDP streams with otxti
 * pushed, and keeps an event ring registered on a /dev/otbridge session.
 *
 * TCP connect and accept by cookie, data both ways, urgent data, address
 * queries, options (known, local, unknown), orderly release and abort;
 * UDP datagrams; privileged ports as root and as a user; ring events
 * and SIGPOLL; the ring across a fork and after its owner exits; flow
 * control with a slow reader; 1000 endpoints opened and closed; unload
 * refused while a session or a stream is open; loading on the first
 * open of /dev/otbridge.  Connections use loopback and, on the network
 * root, the SONIC echo host.  Skipped without the module or on a
 * kernel without its linkages.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/stropts.h>
#include <sys/mkdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include "sys/mod.h"
#include "otwire.h"
#include "t.h"

#define MOD	"/tests/otb/otbridge"
#define LOOP	0x7f000001
#define ECHOH	0x0a000264	/* 10.0.2.100, guestfwd to cat */
#define T_ERR	errstr()

static long sid;

/* strerror is 0 past the table (the module errnos 164-168) */
static char *
errstr(void)
{
	static char b[24];
	char *m = strerror(errno);

	if (m)
		return m;
	sprintf(b, "errno %d", errno);
	return b;
}
static int sfd, npoll, onnet;
static long ring[OTB_RINGSZ / 4], ring2[OTB_RINGSZ / 4 + 2];
static unsigned char rbuf[OTB_MAXREC], wbuf[OTB_RHDR + OTB_MAXCTL + OTB_MAXDATA];
static long evs[64];		/* events seen per cookie (cookie < 64) */

static void
onpoll(int sig)
{
	npoll++;
	signal(SIGPOLL, onpoll);
}

/* ---- ring ---- */

static unsigned short *
r16(int off)
{
	return (unsigned short *)((char *)ring + off);
}

static int
cas16(unsigned short *p, unsigned short old, unsigned short new)
{
	unsigned short o = old;

	__asm__ __volatile__("casw %0,%3,%1" : "=d" (o), "=m" (*p) : "0" (o), "d" (new), "m" (*p));
	return o == old;
}

/* take every entry; returns how many */
static int
drain(void)
{
	int n = 0, t, ent;
	unsigned short ev;
	unsigned long ck;

	while ((t = *r16(8)) != *r16(6)) {
		ent = OTB_RING_HDR + t * OTB_RING_ENT;
		do
			ev = *r16(ent + 4);
		while (!cas16(r16(ent + 4), ev, 0));
		ck = *(unsigned long *)((char *)ring + ent);
		*r16(8) = (t + 1) % OTB_RING_N;
		if (ck < 64)
			evs[ck] |= ev;
		n++;
	}
	return n;
}

/* ---- records ---- */

static int
putrec(int fd, int type, long *ctl, int clen, char *data, long dlen)
{
	wbuf[0] = type;
	wbuf[1] = 0;
	*(unsigned short *)(wbuf + 2) = clen;
	*(long *)(wbuf + 4) = dlen;
	memcpy(wbuf + OTB_RHDR, ctl, clen);
	if (dlen)
		memcpy(wbuf + OTB_RHDR + clen, data, dlen);
	return write(fd, wbuf, OTB_RHDR + clen + dlen);
}

/* next record within secs: its primitive (0 for data), -1 on timeout */
static int rtype, rflags, rclen;
static long rdlen, *rctl;
static char *rdata;

static long
getrec(int fd, int secs)
{
	struct pollfd p;
	int n;

	p.fd = fd;
	p.events = POLLIN;
	if (poll(&p, 1, secs * 1000) != 1)
		return -1;
	if ((n = read(fd, rbuf, sizeof rbuf)) < OTB_RHDR)
		return -1;
	rtype = rbuf[0];
	rflags = rbuf[1];
	rclen = *(unsigned short *)(rbuf + 2);
	rdlen = *(long *)(rbuf + 4);
	rctl = (long *)(rbuf + OTB_RHDR);
	rdata = (char *)rbuf + OTB_RHDR + rclen;
	if (OTB_RHDR + rclen + rdlen != n)
		return -2;
	return rtype == OTB_R_DATA ? 0 : rctl[0];
}

/* skip other records until prim; its ctl in rctl */
static int
expect(int fd, long prim, int secs)
{
	long p;
	int i;

	for (i = 0; i < 16; i++)
		if ((p = getrec(fd, secs)) == prim)
			return 1;
		else if (p < 0)
			return 0;
	return 0;
}

static void
inaddr(long *a, int port, unsigned long host)
{
	a[0] = (OT_AF_INET << 16) | port;
	a[1] = host;
	a[2] = a[3] = 0;
}

static int
ep_open(char *dev, unsigned long cookie, long proto)
{
	struct otx_attach at;
	struct strioctl si;
	int fd;

	if ((fd = open(dev, O_RDWR | O_NDELAY)) < 0)
		return -1;
	if (ioctl(fd, I_PUSH, "otxti") < 0) {
		close(fd);
		return -2;
	}
	at.at_session = sid;
	at.at_cookie = cookie;
	at.at_proto = proto;
	si.ic_cmd = OTX_ATTACH;
	si.ic_timout = 5;
	si.ic_len = sizeof at;
	si.ic_dp = (char *)&at;
	if (sid && ioctl(fd, I_STR, &si) < 0) {
		close(fd);
		return -3;
	}
	return fd;
}

/* bind; the bound port, or -(TLI error), or -100 */
static int
bind_ep(int fd, int port, unsigned long host, int qlen)
{
	long c[8];

	c[0] = OT_BIND_REQ;
	c[1] = 16;
	c[2] = 16;
	c[3] = qlen;
	inaddr(c + 4, port, host);
	if (putrec(fd, OTB_R_PROTO, c, 32, 0, 0L) < 0)
		return -100;
	if (getrec(fd, 10) == OT_ERROR_ACK)
		return -rctl[2];
	if (rctl[0] != OT_BIND_ACK || rctl[1] != 16)
		return -100;
	return ((long *)((char *)rctl + rctl[2]))[0] & 0xffff;
}

static long
send_prim(int fd, long *c, int n)
{
	return putrec(fd, OTB_R_PROTO, c, 4 * n, 0, 0L);
}

static int
send_data(int fd, long prim, char *d, long n)
{
	long c[2];

	c[0] = prim;
	c[1] = 0;
	return putrec(fd, OTB_R_PROTO, c, 8, d, n);
}

/* collect n bytes of user data; returns bytes that matched seed */
static long
recv_data(int fd, long n, int seed, int secs)
{
	long got = 0, i, p;

	while (got < n && (p = getrec(fd, secs)) >= 0) {
		if (p != 0 && p != OT_DATA_IND)
			break;
		for (i = 0; i < rdlen; i++)
			if ((unsigned char)rdata[i] != (unsigned char)t_pattern(got + i, seed))
				return got + i;
		got += rdlen;
	}
	return got;
}

/* pattern bytes [off, off + n) */
static char *
fillo(long off, long n, int seed)
{
	static char b[OTB_MAXDATA];
	long i;

	for (i = 0; i < n; i++)
		b[i] = t_pattern(off + i, seed);
	return b;
}
#define fill(n, seed)	fillo(0L, n, seed)

static void
stats(struct otb_stats *st)
{
	memset(st, 0, sizeof *st);
	ioctl(sfd, OTB_STATS, st);
}

/* ---- connections ---- */

/* connect c to lport; accept on a by cookie; 1 when both ends see it */
static int
connect_accept(int l, int c, int a, unsigned long acookie, int lport, unsigned long host, long *seqp)
{
	long r[9], seq;

	r[0] = OT_CONN_REQ;
	r[1] = 16;
	r[2] = 20;
	r[3] = 0;
	r[4] = 0;
	inaddr(r + 5, lport, host);
	if (send_prim(c, r, 9) < 0)
		return 0;
	if (!expect(l, OT_CONN_IND, 10))
		return 0;
	seq = rctl[5];
	*seqp = seq;
	r[0] = OT_CONN_RES;
	r[1] = acookie;
	r[2] = 0;
	r[3] = 0;
	r[4] = seq;
	send_prim(l, r, 5);
	if (!expect(l, OT_OK_ACK, 10) || rctl[1] != OT_CONN_RES)
		return 0;
	return expect(c, OT_CONN_CON, 10);
}

static int
addr_query(int fd, int *lport, int *rport, unsigned long *rhost)
{
	long c[1];
	long *la, *ra;

	c[0] = OT_ADDR_REQ;
	send_prim(fd, c, 1);
	if (!expect(fd, OT_ADDR_ACK, 5))
		return 0;
	la = (long *)((char *)rctl + rctl[2]);
	ra = (long *)((char *)rctl + rctl[4]);
	*lport = rctl[1] == 16 ? la[0] & 0xffff : -1;
	*rport = rctl[3] == 16 ? ra[0] & 0xffff : -1;
	*rhost = rctl[3] == 16 ? ra[1] : 0;
	return 1;
}

/* option request: pairs of (level, name, value) of one long each */
static int
optreq(int fd, long flags, long *spec, int n)
{
	long c[4 + 5 * 8], p = -1;
	int i;

	c[0] = OT_OPTMGMT_REQ;
	c[1] = 20 * n;
	c[2] = 16;
	c[3] = flags;
	for (i = 0; i < n; i++) {
		c[4 + 5 * i] = 20;
		c[5 + 5 * i] = spec[3 * i];
		c[6 + 5 * i] = spec[3 * i + 1];
		c[7 + 5 * i] = 0;
		c[8 + 5 * i] = spec[3 * i + 2];
	}
	send_prim(fd, c, 4 + 5 * n);
	for (i = 0; i < 8; i++)
		if ((p = getrec(fd, 10)) == OT_OPTMGMT_ACK || p < 0 ||
		    (p == OT_ERROR_ACK && rctl[1] == OT_OPTMGMT_REQ))
			break;
	return p == OT_OPTMGMT_ACK;
}

/* status and value of the i-th option in the last OPTMGMT_ACK */
static long
optst(int i, long *val)
{
	char *o = (char *)rctl + rctl[2];
	int k;

	*val = 0;
	if (rctl[0] != OT_OPTMGMT_ACK || rctl[1] < 20 * (i + 1))
		return -1;
	for (k = 0; k < i; k++)
		o += (*(long *)o + 3) & ~3;
	*val = ((long *)o)[4];
	return ((long *)o)[3];
}

static void
tcp_tests(int net)
{
	int l, c, a, c2, a2, lport, cport, p1, p2, n, k;
	long seq, v, total;
	unsigned long h, host = LOOP;
	struct otb_stats st;

	l = ep_open("/dev/tcp", 1, OTX_TCP);
	c = ep_open("/dev/tcp", 2, OTX_TCP);
	a = ep_open("/dev/tcp", 3, OTX_TCP);
	if (!t_check("tcp.open", l >= 0 && c >= 0 && a >= 0, "open/push/attach %d %d %d: %s",
	    l, c, a, T_ERR))
		return;
	lport = bind_ep(l, 0, 0L, 5);
	cport = bind_ep(c, 0, 0L, 0);
	t_check("tcp.bind", lport > 0 && cport > 0 && bind_ep(a, 0, 0L, 0) > 0,
	    "ports %d %d", lport, cport);
	if (!connect_accept(l, c, a, 3, lport, host, &seq)) {
		if (!net) {
			t_skip("tcp.connect_accept", "no loopback route on this root");
			goto out;
		}
		t_fail("tcp.connect_accept", "last primitive %ld", rctl ? rctl[0] : -1L);
		goto out;
	}
	t_pass("tcp.connect_accept");

	/* data both ways, one 20 KB record split upstream */
	t_check("tcp.data_c_to_a", send_data(c, OT_DATA_REQ, fill(3000L, 1), 3000L) > 0 &&
	    (v = recv_data(a, 3000L, 1, 10)) == 3000, "%ld of 3000 bytes", v);
	t_check("tcp.data_a_to_c", send_data(a, OT_DATA_REQ, fill(20000L, 2), 20000L) > 0 &&
	    (v = recv_data(c, 20000L, 2, 10)) == 20000, "%ld of 20000 bytes", v);

	/* urgent data */
	send_data(c, OT_EXDATA_REQ, "U", 1L);
	for (k = 0, v = 0; k < 8 && (v = getrec(a, 5)) >= 0 && v != OT_EXDATA_IND; k++)
		;
	t_check("tcp.urgent", v == OT_EXDATA_IND && rdlen >= 1 && rdata[rdlen - 1] == 'U',
	    "got %ld, %ld bytes", v, rdlen);

	/* addresses */
	t_check("tcp.addr_connector", addr_query(c, &p1, &p2, &h) && p1 == cport &&
	    p2 == lport && h == host, "local %d remote %d host %lx", p1, p2, h);
	t_check("tcp.addr_acceptor", addr_query(a, &p1, &p2, &h) && p1 == lport &&
	    p2 == cport && h == host, "local %d remote %d host %lx", p1, p2, h);

	/* options */
	{
		long spec[] = { OT_INET_TCP, OT_TCP_NODELAY, 1, OT_INET_TCP, 0x99, 1,
			OT_INET_TCP, OT_TCP_NOTIFY_THRESHOLD, 5000,
			OT_XTI_GENERIC, OT_XTI_SNDBUF, 16384 };
		long s0, s1, s2, s3, v0, v2, v3;
		if (t_check("opt.negotiate", optreq(c, OT_NEGOTIATE, spec, 4), "no ack")) {
			s0 = optst(0, &v0);
			s1 = optst(1, &v);
			s2 = optst(2, &v2);
			s3 = optst(3, &v3);
			t_check("opt.negotiate_status", s0 == OT_SUCCESS && s1 == OT_NOTSUPPORT &&
			    s2 == OT_SUCCESS && v2 == 5000 && rctl[3] == OT_NOTSUPPORT,
			    "nodelay %lx=%ld unknown %lx threshold %lx=%ld overall %lx",
			    s0, v0, s1, s2, v2, rctl[3]);
			t_info("opt.sndbuf_negotiated", "status %lx value %ld", s3, v3);
		}
		spec[2] = 0;
		if (optreq(c, OT_CURRENT, spec, 1)) {
			s0 = optst(0, &v0);
			t_check("opt.current_host", s0 == OT_SUCCESS && v0 == 1,
			    "nodelay status %lx value %ld", s0, v0);
		} else
			t_fail("opt.current_host", "no ack");
		spec[8] = 0;
		if (optreq(c, OT_DEFAULT, spec + 6, 1)) {
			s0 = optst(0, &v0);
			t_check("opt.default_local", s0 == OT_SUCCESS && v0 == 10000,
			    "status %lx value %ld", s0, v0);
		} else
			t_fail("opt.default_local", "no ack");
		spec[2] = 1;
		t_check("opt.check", optreq(c, OT_CHECK, spec, 1) && optst(0, &v0) == OT_SUCCESS,
		    "status %lx", optst(0, &v0));
		t_check("opt.bad_flags", optreq(c, OT_CHECK | OT_NEGOTIATE, spec, 1) == 0 &&
		    rctl[0] == OT_ERROR_ACK && rctl[2] == OT_TBADFLAG, "prim %ld err %ld",
		    rctl[0], rctl[2]);
	}

	/* ring and SIGPOLL: data to a posts READ for cookie 3 */
	drain();
	memset(evs, 0, sizeof evs);
	n = npoll;
	send_data(c, OT_DATA_REQ, fill(100L, 3), 100L);
	for (k = 0; k < 50 && npoll == n; k++)
		poll(0, 0, 100);
	drain();
	t_check("ring.read_event", (evs[3] & OTB_EV_READ) && npoll > n,
	    "events %lx, signals %d -> %d", evs[3], n, npoll);
	recv_data(a, 100L, 3, 5);

	/* slow reader: c fills up, then drains back to writable */
	total = 0;
	for (k = 0; k < 256; k++) {
		v = send_data(c, OT_DATA_REQ, fillo(total, 4096L, 4), 4096L);
		if (v <= 0)
			break;
		total += 4096;
	}
	t_check("flow.would_block", (v == 0 || errno == EAGAIN) && total > 0,
	    "write %ld (%s) after %ld bytes", v, T_ERR, total);
	drain();
	memset(evs, 0, sizeof evs);
	v = recv_data(a, total, 4, 10);
	for (k = 0; k < 50 && !(evs[2] & OTB_EV_WRITE); k++) {
		poll(0, 0, 100);
		drain();
	}
	t_check("flow.writable_event", v == total && (evs[2] & OTB_EV_WRITE),
	    "read %ld of %ld, events %lx", v, total, evs[2]);
	t_check("flow.write_again", send_data(c, OT_DATA_REQ, fill(100L, 5), 100L) > 0 &&
	    recv_data(a, 100L, 5, 5) == 100, "%s", T_ERR);

	/* orderly release */
	{
		long r[1];
		r[0] = OT_ORDREL_REQ;
		send_prim(c, r, 1);
		t_check("tcp.ordrel_a", expect(a, OT_ORDREL_IND, 10), "last %ld", rctl[0]);
		send_prim(a, r, 1);
		t_check("tcp.ordrel_c", expect(c, OT_ORDREL_IND, 10), "last %ld", rctl[0]);
	}

	/* abort */
	c2 = ep_open("/dev/tcp", 4, OTX_TCP);
	a2 = ep_open("/dev/tcp", 5, OTX_TCP);
	if (c2 >= 0 && a2 >= 0 && bind_ep(c2, 0, 0L, 0) > 0 && bind_ep(a2, 0, 0L, 0) > 0 &&
	    connect_accept(l, c2, a2, 5, lport, host, &seq)) {
		long r[2];
		r[0] = OT_DISCON_REQ;
		r[1] = -1;
		send_prim(c2, r, 2);
		t_check("tcp.abort_ack", expect(c2, OT_OK_ACK, 10) && rctl[1] == OT_DISCON_REQ,
		    "last %ld", rctl[0]);
		t_check("tcp.abort_discon_ind", expect(a2, OT_DISCON_IND, 10) &&
		    rctl[1] == OT_ECONNRESET, "last %ld reason %ld", rctl[0], rctl[1]);
	} else
		t_fail("tcp.abort", "second connection failed");

	/* unload refused while endpoints exist */
	stats(&st);
	t_info("stats", "sessions %ld endpoints %ld kmem %ld posts %ld merged %ld signals %ld",
	    st.st_sessions, st.st_endpoints, st.st_kmem, st.st_posts, st.st_merged, st.st_signals);
	if (c2 >= 0)
		close(c2);
	if (a2 >= 0)
		close(a2);
out:
	close(l);
	close(c);
	close(a);
}

/* over the SONIC to the echo host */
static void
echo_test(void)
{
	int c = ep_open("/dev/tcp", 6, OTX_TCP);
	long r[9], v;

	if (c < 0 || bind_ep(c, 0, 0L, 0) <= 0) {
		t_fail("echo.open", "%s", T_ERR);
		return;
	}
	r[0] = OT_CONN_REQ;
	r[1] = 16;
	r[2] = 20;
	r[3] = r[4] = 0;
	inaddr(r + 5, 7, ECHOH);
	send_prim(c, r, 9);
	if (t_check("echo.connect", expect(c, OT_CONN_CON, 15), "last %ld", rctl[0]))
		t_check("echo.data", send_data(c, OT_DATA_REQ, fill(6000L, 6), 6000L) > 0 &&
		    (v = recv_data(c, 6000L, 6, 15)) == 6000, "%ld of 6000", v);
	close(c);
}

static void
udp_tests(void)
{
	int u1 = ep_open("/dev/udp", 10, OTX_UDP), u2 = ep_open("/dev/udp", 11, OTX_UDP);
	int p1, p2;
	long r[9], *src;

	if (!t_check("udp.open", u1 >= 0 && u2 >= 0, "%s", T_ERR))
		return;
	p1 = bind_ep(u1, 0, 0L, 0);
	p2 = bind_ep(u2, 0, 0L, 0);
	t_check("udp.bind", p1 > 0 && p2 > 0, "ports %d %d", p1, p2);
	r[0] = OT_UNITDATA_REQ;
	r[1] = 16;
	r[2] = 20;
	r[3] = r[4] = 0;
	inaddr(r + 5, p2, LOOP);
	putrec(u1, OTB_R_PROTO, r, 36, fill(200L, 7), 200L);
	if (getrec(u2, 5) == OT_UNITDATA_IND) {
		src = (long *)((char *)rctl + rctl[2]);
		t_check("udp.unitdata", rctl[1] == 16 && (src[0] & 0xffff) == p1 &&
		    rdlen == 200 && rdata[199] == (char)t_pattern(199L, 7),
		    "src %lx %lx, %ld bytes", src[0], src[1], rdlen);
	} else if (onnet)
		t_fail("udp.unitdata", "no datagram over loopback (last %ld)", rctl ? rctl[0] : -1L);
	else
		t_skip("udp.unitdata", "no loopback on this root");
	{
		long spec[] = { OT_INET_UDP, OT_UDP_CHECKSUM, 1, OT_INET_TCP, OT_TCP_NODELAY, 1 };
		long v;
		t_check("udp.options", optreq(u1, OT_NEGOTIATE, spec, 2) &&
		    optst(0, &v) == OT_SUCCESS && optst(1, &v) == OT_NOTSUPPORT,
		    "status %lx %lx", optst(0, &v), optst(1, &v));
	}
	close(u1);
	close(u2);
}

/* privileged ports: root binds 999, a user gets TACCES; a user may not attach to our session */
static void
priv_tests(void)
{
	int fd, st;
	pid_t pid;

	fd = ep_open("/dev/tcp", 20, OTX_TCP);
	t_check("priv.root_bind_999", fd >= 0 && bind_ep(fd, 999, 0L, 0) == 999, "%s", T_ERR);
	if (fd >= 0)
		close(fd);
	if ((pid = fork()) == 0) {
		setgid(1);
		if (setuid(100) < 0)
			_exit(10);
		if ((fd = ep_open("/dev/tcp", 21, OTX_TCP)) != -3 || errno != EPERM)
			_exit(11);
		sid = 0;
		if ((fd = ep_open("/dev/tcp", 21, OTX_TCP)) < 0)
			_exit(12);
		_exit(bind_ep(fd, 998, 0L, 0) == -OT_TACCES ? 0 : 13);
	}
	t_waitchild(pid, &st, 20);
	t_check("priv.user_bind_998", WIFEXITED(st) && WEXITSTATUS(st) == 0,
	    "child status %x (11: attach to another process's session, 13: bind)", st);
}

/*
 * A fork after OTB_SETRING, the parent writing the ring: the ring either
 * keeps working or is reported lost and registers again.
 */
static void
ring_fork(void)
{
	struct otb_setring sr;
	long c[1];
	int fd, st, live, got;
	pid_t pid;

	drain();
	if ((pid = fork()) == 0) {
		sleep(5);
		_exit(0);
	}
	*(volatile unsigned short *)r16(8) = *r16(8);
	fd = ep_open("/dev/udp", 40, OTX_UDP);
	c[0] = 9999;			/* unknown: an error ack comes up */
	got = fd >= 0 && putrec(fd, OTB_R_PROTO, c, 4, 0, 0L) > 0 &&
	    expect(fd, OT_ERROR_ACK, 5);
	live = ioctl(sfd, OTB_GETRING, 0);
	evs[40] = 0;
	drain();
	t_info("ring.fork_mode", "%s", live == 1 ? "ring kept" : "ring lost");
	t_check("ring.fork", got && (live == 1 ? (evs[40] & OTB_EV_READ) != 0 : live == 0),
	    "error ack %d, getring %d, events %lx", got, live, evs[40]);
	if (live == 0) {
		sr.sr_addr = (unsigned long)ring;
		sr.sr_sig = 0;
		t_check("ring.reregister", ioctl(sfd, OTB_SETRING, &sr) == 0 &&
		    ioctl(sfd, OTB_GETRING, 0) == 1, "%s", T_ERR);
	}
	if (fd >= 0)
		close(fd);
	kill(pid, SIGKILL);
	t_waitchild(pid, &st, 10);
}

/*
 * A session inherited by a grandchild that outlives the owner: its
 * last close frees the session; the dead owner's pages stay locked.
 */
static void
ring_inherit(void)
{
	struct otb_stats a, b;
	struct otb_setring sr;
	int p[2], fd, st, n = -1;
	pid_t pid;
	char ch;

	stats(&a);
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		close(p[0]);
		if ((fd = open("/dev/otbridge", O_RDWR)) < 0)
			_exit(1);
		sr.sr_addr = ((unsigned long)ring2 + 7) & ~7UL;
		sr.sr_sig = 0;
		if (ioctl(fd, OTB_SETRING, &sr) < 0)
			_exit(2);
		if (fork() == 0) {
			sleep(2);
			close(fd);
			write(p[1], "x", 1);
			_exit(0);
		}
		_exit(0);
	}
	close(p[1]);
	t_waitchild(pid, &st, 10);
	n = read(p[0], &ch, 1);
	close(p[0]);
	stats(&b);
	t_check("ring.inherited", WIFEXITED(st) && WEXITSTATUS(st) == 0 && n == 1 &&
	    b.st_sessions == a.st_sessions && b.st_ringleak == a.st_ringleak + 1,
	    "child status %x, pipe %d, sessions %ld -> %ld, leaked rings %ld -> %ld",
	    st, n, a.st_sessions, b.st_sessions, a.st_ringleak, b.st_ringleak);
}

/* id of the loaded module named name, or -1 */
static int
byname(char *name)
{
	struct modstatus ms;
	int id = 1;

	while (modstat(id, &ms, 1) == 0) {
		if (strcmp(ms.ms_name, name) == 0)
			return ms.ms_id;
		id = ms.ms_id + 1;
	}
	return -1;
}

/* the module still answers: a stream takes otxti */
static int
modstat_ok(int id)
{
	int fd = ep_open("/dev/udp", 31, OTX_UDP);

	if (fd < 0)
		return 0;
	close(fd);
	return 1;
}

static void
many(void)
{
	struct otb_stats a, b;
	int i, fd, bad = 0;
	long t0 = t_now_ms();

	stats(&a);
	for (i = 0; i < 1000; i++) {
		fd = ep_open(i & 1 ? "/dev/udp" : "/dev/tcp", 100 + i, i & 1 ? OTX_UDP : OTX_TCP);
		if (fd < 0)
			bad++;
		else
			close(fd);
		if (i % 200 == 199)
			t_rearm(120);
	}
	stats(&b);
	t_check("many.1000", bad == 0 && b.st_endpoints == a.st_endpoints &&
	    b.st_kmem == a.st_kmem, "%d failed; endpoints %ld -> %ld, kmem %ld -> %ld",
	    bad, a.st_endpoints, b.st_endpoints, a.st_kmem, b.st_kmem);
	t_info("many.time", "%ld ms", t_now_ms() - t0);
}

int
main(void)
{
	struct otb_setring sr;
	struct otb_stats st;
	struct modstatus ms;
	struct mod_mreg reg;
	int id, busy, mj = 55, net = access("/usr/sbin/ifconfig", 0) == 0;

	t_init("otb", 280);
	onnet = net;
	if (net && fork() == 0) {
		close(1);
		close(2);
		execl("/usr/sbin/ifconfig", "ifconfig", "lo0", "127.0.0.1", "up", (char *)0);
		_exit(1);
	}
	wait(0);
	if (access(MOD, 0) < 0) {
		t_skip("all", "no otbridge module for this kernel");
		return t_done();
	}
	/* major 55 loads otbridge: its driver linkage needs the slot */
	strcpy(reg.md_modname, "otbridge");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0, "%s", T_ERR))
		return t_done();
	if ((id = modload(MOD)) < 0) {
		t_fail("load", "%s", T_ERR);
		return t_done();
	}
	t_pass("load");
	t_check("linkages", modstat(id, &ms, 0) == 0 &&
	    ms.ms_msinfo[0].mss_type == MOD_TY_STR && ms.ms_msinfo[1].mss_type == MOD_TY_CDEV &&
	    ms.ms_msinfo[1].mss_p1[0] == 55, "types %d %d, major %d",
	    ms.ms_msinfo[0].mss_type, ms.ms_msinfo[1].mss_type, ms.ms_msinfo[1].mss_p1[0]);
	if (access("/dev/otbridge", 0) < 0)
		mknod("/dev/otbridge", S_IFCHR | 0666, makedev(55, 0));
	sfd = open("/dev/otbridge", O_RDWR);
	sid = sfd < 0 ? -1 : ioctl(sfd, OTB_GETID, 0);
	if (!t_check("session", sfd >= 0 && sid >= 2, "fd %d id %ld: %s", sfd, sid, T_ERR))
		goto unload;
	priv_tests();
	signal(SIGPOLL, onpoll);
	sr.sr_addr = (unsigned long)ring;
	sr.sr_sig = 0;
	t_check("ring.register", ioctl(sfd, OTB_SETRING, &sr) == 0 &&
	    (unsigned long)ring[0] == OTB_RMAGIC && *r16(4) == OTB_RING_N,
	    "magic %lx n %d: %s", ring[0], *r16(4), T_ERR);

	tcp_tests(net);
	t_rearm(200);
	if (net)
		echo_test();
	udp_tests();
	{
		int e = ep_open("/dev/tcp", 30, OTX_TCP), r, err;

		errno = 0;
		r = moduload(id);
		err = errno;
		busy = r < 0 && modstat_ok(id);
		t_check("unload_busy", e >= 0 && busy && err == EBUSY,
		    "endpoint %d, moduload %d errno %d", e, r, err);
		if (e >= 0)
			close(e);
	}
	t_rearm(200);
	many();
	t_rearm(200);
	ring_fork();
	ring_inherit();

	stats(&st);
	t_check("stats.clean", st.st_endpoints == 0 && st.st_kmem == 0 && st.st_sessions == 1,
	    "endpoints %ld kmem %ld sessions %ld", st.st_endpoints, st.st_kmem, st.st_sessions);
	sr.sr_addr = 0;
	t_check("ring.unregister", ioctl(sfd, OTB_SETRING, &sr) == 0, "%s", T_ERR);
	close(sfd);
	sid = 0;
	{
		int e = ep_open("/dev/udp", 50, OTX_UDP), r, err;

		errno = 0;
		r = moduload(id);
		err = errno;
		t_check("unload_busy_stream", e >= 0 && r < 0 && err == EBUSY,
		    "stream %d, moduload %d errno %d", e, r, err);
		if (e >= 0)
			close(e);
	}
unload:
	t_check("unload", moduload(id) == 0, "%s", T_ERR);
	{
		int fd, aid = -1;

		errno = 0;
		fd = modpath("/tests/otb") < 0 ? -2 : open("/dev/otbridge", O_RDWR);
		t_check("autoload", fd >= 0 && ioctl(fd, OTB_GETID, 0) >= 2 &&
		    (aid = byname("otbridge")) > 0, "fd %d id %d: %s", fd, aid, T_ERR);
		if (fd >= 0)
			close(fd);
		t_check("autoload.unload", aid > 0 && moduload(aid) == 0, "%s", T_ERR);
	}
	return t_done();
}
