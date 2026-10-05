/*
 * stiktest.c -- the STiK transport from inside TOS, run from C:\AUTO on
 * the network root: finds the cookie, resolves, talks to the TCP echo
 * at 10.0.2.100:7, is refused by 10.0.2.2:1, and sends a datagram and
 * a stream over loopback.  Calls go through the TPL table with words
 * on the stack, as Pure C programs make them.  Each check becomes
 * "PASS name" or "FAIL name value" in U:\STIK.TXT; "DONE" ends it.
 */

extern long trap1(), tcall();

#define	ECHO	0x0a000264L
#define	GW	0x0a000202L
#define	LOOP	0x7f000001L
#define	TESTABLISH 4

static short w[16];
static int nw;
static char out[2048], buf[256];
static int on;
static char *tpl;

static void pw(v) { w[nw++] = v; }
static void pl(v) long v; { w[nw++] = v >> 16; w[nw++] = v; }

static long
go()
{
	int n = nw;

	nw = 0;
	return trap1(w, n);
}

/* TPL entry i with the words so far */
static long
T(i)
{
	int n = nw;

	nw = 0;
	return tcall(*(long *)(tpl + 12 + 4 * i), w, n);
}

static long Fcreate(n, a) char *n; { pw(0x3c); pl((long)n); pw(a); return go(); }
static long Fwrite(h, c, b) long c; char *b; { pw(0x40); pw(h); pl(c); pl((long)b); return go(); }
static long Fclose(h) { pw(0x3e); pw(h); return go(); }
static long Super(s) long s; { pw(0x20); pl(s); return go(); }

#define	KRmalloc(n)		(pl(n), T(0))
#define	KRfree(p)		(pl((long)(p)), T(1))
#define	get_err_text(e)		(pw(e), (char *)T(4))
#define	getvstr(s)		(pl((long)(s)), (char *)T(5))
#define	TCP_open(h, p)		(pl(h), pw(p), pw(0), pw(2000), (short)T(7))
#define	TCP_close(c)		(pw(c), pw(0), pl(0L), (short)T(8))
#define	TCP_send(c, b, n)	(pw(c), pl((long)(b)), pw(n), (short)T(9))
#define	TCP_wait_state(c, s, t)	(pw(c), pw(s), pw(t), (short)T(10))
#define	UDP_open(h, p)		(pl(h), pw(p), (short)T(12))
#define	UDP_close(c)		(pw(c), (short)T(13))
#define	UDP_send(c, b, n)	(pw(c), pl((long)(b)), pw(n), (short)T(14))
#define	CNbyte_count(c)		(pw(c), (short)T(16))
#define	CNget_char(c)		(pw(c), (short)T(17))
#define	CNget_NDB(c)		(pw(c), (char *)T(18))
#define	CNget_block(c, b, n)	(pw(c), pl((long)(b)), pw(n), (short)T(19))
#define	resolve(d, r, l, n)	(pl((long)(d)), pl((long)(r)), pl((long)(l)), pw(n), (short)T(21))
#define	CNgets(c, b, n, d)	(pw(c), pl((long)(b)), pw(n), pw(d), (short)T(31))

static int
same(a, b, n)
	char *a, *b;
{
	while (n-- > 0)
		if (*a++ != *b++)
			return 0;
	return 1;
}

static void
put(s)
	char *s;
{
	while (*s && on < sizeof out - 1)
		out[on++] = *s++;
}

static void
num(v)
	long v;
{
	char b[12];
	int i = 11;

	b[i] = 0;
	if (v < 0) {
		put("-");
		v = -v;
	}
	do
		b[--i] = '0' + v % 10;
	while ((v /= 10) && i);
	put(b + i);
}

static int
check(name, ok, v)
	char *name;
	int ok;
	long v;
{
	put(ok ? "PASS " : "FAIL ");
	put(name);
	if (!ok) {
		put(" ");
		num(v);
	}
	put("\r\n");
	return ok;
}

/* up to s seconds for n bytes on c, polling as clients do */
static int
waitn(c, n, s)
{
	int k;

	while ((k = CNbyte_count(c)) < n && k >= 0 && s-- > 0)
		TCP_wait_state(c, 99, 1);
	return k;
}

static int
wait1(c, n)
{
	int i, k = 0;

	for (i = 0; i < 200 && (k = CNbyte_count(c)) < n && k >= 0; i++)
		;
	return k;
}

/* the cookie's DRV_LIST */
static char *
cookie()
{
	long *j, s = Super(0L), d = 0;

	for (j = *(long **)0x5a0; j && j[0]; j += 2)
		if (j[0] == 0x5354694bL)
			d = j[1];
	Super(s);
	return (char *)d;
}

static void
tcp()
{
	char *ndb;
	int c, k;

	c = TCP_open(ECHO, 7);
	if (!check("tcp_open", c >= 0, (long)c))
		return;
	k = TCP_wait_state(c, TESTABLISH, 10);
	check("tcp_established", k == 0, (long)k);
	k = TCP_send(c, "hello stik\r\n", 12);
	check("tcp_send", k == 0, (long)k);
	k = waitn(c, 12, 10);
	check("tcp_count", k >= 12, (long)k);
	k = CNgets(c, buf, sizeof buf, '\n');
	check("tcp_gets", k == 11 && same(buf, "hello stik\r", 12), (long)k);
	TCP_send(c, "xy", 2);
	waitn(c, 2, 10);
	k = CNget_char(c);
	check("tcp_char", k == 'x', (long)k);
	k = CNget_char(c);
	check("tcp_char2", k == 'y', (long)k);
	k = CNget_char(c);
	check("tcp_nodata", k == -2, (long)k);
	TCP_send(c, "block", 5);
	waitn(c, 5, 10);
	k = CNget_block(c, buf, 5);
	check("tcp_block", k == 5 && same(buf, "block", 5), (long)k);
	TCP_send(c, "ndb", 3);
	waitn(c, 3, 10);
	ndb = CNget_NDB(c);
	check("tcp_ndb", ndb && *(unsigned short *)(ndb + 8) == 3 && same(*(char **)(ndb + 4), "ndb", 3),
	    ndb ? (long)*(unsigned short *)(ndb + 8) : -1L);
	if (ndb) {
		KRfree(*(char **)ndb);
		KRfree(ndb);
	}
	k = TCP_close(c);
	check("tcp_close", k == 0, (long)k);
	k = CNbyte_count(c);
	check("tcp_closed_handle", k == -9, (long)k);
}

static void
refused()
{
	int c, k;

	c = TCP_open(GW, 1);
	if (!check("refused_open", c >= 0, (long)c))
		return;
	k = TCP_wait_state(c, TESTABLISH, 10);
	check("refused", k == -7, (long)k);
	TCP_close(c);
}

/* a datagram and a stream to ourselves: UDP_EXTEND and TCP_PASSIVE */
static void
loop()
{
	short cab[6];
	int s, c, k;

	cab[0] = 7007; cab[1] = 0;
	*(long *)(cab + 2) = 0;
	*(long *)(cab + 4) = 0;
	s = UDP_open((long)cab, 0);
	c = UDP_open(LOOP, 7007);
	if (check("udp_open", s >= 0 && c >= 0, (long)(s < 0 ? s : c))) {
		k = UDP_send(c, "dgram", 5);
		check("udp_send", k == 0, (long)k);
		k = waitn(s, 5, 5);
		check("udp_count", k == 5, (long)k);
		k = CNget_block(s, buf, 5);
		check("udp_recv", k == 5 && same(buf, "dgram", 5), (long)k);
	}
	UDP_close(s);
	UDP_close(c);
	s = TCP_open(0L, 7008);
	k = CNbyte_count(s);
	check("listen_count", k == -10, (long)k);
	c = TCP_open(LOOP, 7008);
	if (!check("listen_open", s >= 0 && c >= 0, (long)(s < 0 ? s : c)))
		return;
	k = TCP_wait_state(c, TESTABLISH, 10);
	check("listen_connect", k == 0, (long)k);
	k = TCP_wait_state(s, TESTABLISH, 10);
	check("listen_accept", k == 0, (long)k);
	TCP_send(c, "to server", 9);
	k = waitn(s, 9, 10);
	check("listen_data", k == 9, (long)k);
	TCP_close(c);
	waitn(s, 10, 3);
	k = CNget_block(s, buf, 9);
	check("listen_recv", k == 9 && same(buf, "to server", 9), (long)k);
	k = wait1(s, 1);
	check("listen_eof", k == -3, (long)k);
	/* the peer is gone: the second send sees its reset, and we live on */
	TCP_send(s, "x", 1);
	TCP_wait_state(s, TESTABLISH, 1);
	k = TCP_send(s, "y", 1);
	check("send_reset", k == -4, (long)k);
	TCP_close(s);
}

/* a name only DNS knows; skipped without a name server or an answer */
static void
dns()
{
	unsigned long a[4];
	char *real = 0;
	int k;

	a[0] = 0;
	k = resolve("example.com", &real, a, 4);
	if (k == -22 || k == -17) {
		put(k == -22 ? "SKIP resolve_dns no name server\r\n" :
		    "SKIP resolve_dns no answer\r\n");
		return;
	}
	check("resolve_dns", k >= 1 && a[0] != 0 && real && same(real, "example.com", 12),
	    (long)k);
	KRfree(real);
	k = resolve("nosuchhost.invalid", (char **)0, a, 4);
	check("resolve_dns_none", k == -20 || k == -25, (long)k);
}

int
main()
{
	char *d, *real = 0, *p;
	unsigned long a[4];
	int h, k;

	d = cookie();
	if (check("cookie", d != 0, 0L) && check("magic", same(d, "STiKmagic", 10), 0L)) {
		pl((long)"TRANSPORT_TCPIP");
		tpl = (char *)tcall(*(long *)(d + 10), w, nw);
		nw = 0;
	}
	if (check("dftab", tpl && same(*(char **)tpl, "TRANSPORT_TCPIP", 16), 0L)) {
		p = get_err_text(-7);
		check("err_text", p && same(p, "Connection refused", 18), 0L);
		check("getvstr", same(getvstr("NOSUCHVAR"), "0", 2), 0L);
		p = (char *)KRmalloc(1000L);
		check("krmalloc", p != 0, 0L);
		KRfree(p);
		k = resolve("10.0.2.100", &real, a, 4);
		check("resolve_quad", k == 1 && a[0] == ECHO && real && same(real, "10.0.2.100", 11),
		    (long)k);
		KRfree(real);
		k = resolve("localhost", (char **)0, a, 4);
		check("resolve_hosts", k == 1 && a[0] == LOOP, (long)k);
		dns();
		tcp();
		refused();
		loop();
	}
	put("DONE\r\n");
	if ((h = Fcreate("U:\\STIK.TXT", 0)) >= 0) {
		Fwrite(h, (long)on, out);
		Fclose(h);
	}
	return 0;
}
