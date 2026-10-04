/*
 * otxtest.c -- host tests of the translation tables and the option
 * engine against hand-built records in both dialects.
 */

#include <stdio.h>
#include <string.h>
#include "otbridge.h"

static int fails, passes;

static void
check(name, ok)
	char *name;
	int ok;
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	if (ok)
		passes++;
	else
		fails++;
}

/* longs, big-endian, into b; returns bytes */
static int
L(b, n, v)
	unsigned char *b;
	int n;
	long *v;
{
	int i;

	for (i = 0; i < n; i++)
		OB_P32(b + 4 * i, v[i]);
	return 4 * n;
}

static int
eq(b, n, v)
	unsigned char *b;
	int n;
	long *v;
{
	int i;

	for (i = 0; i < n; i++)
		if (OB_G32(b + 4 * i) != ((unsigned long)v[i] & 0xffffffffUL))
			return 0;
	return 1;
}

/* InetAddress / sockaddr_in: AF_INET, port, host */
static void
inaddr(b, port, host)
	unsigned char *b;
	int port;
	unsigned long host;
{
	memset(b, 0, 16);
	OB_P16(b, 2);
	OB_P16(b + 2, port);
	OB_P32(b + 4, host);
}

static unsigned char in[512], out[512], ref[16];

static void
records()
{
	int t, f, c;
	long d;

	otx_mkrhdr(out, OTB_R_PROTO, OTB_RF_MORE, 20, 300L);
	check("record.hdr_bytes", memcmp(out, "\1\1\0\24\0\0\1\54", 8) == 0);
	check("record.hdr_parse", otx_rhdr(out, &t, &f, &c, &d) == 0 &&
	    t == OTB_R_PROTO && f == OTB_RF_MORE && c == 20 && d == 300);
	otx_mkrhdr(out, 7, 0, 0, 0L);
	check("record.bad_type", otx_rhdr(out, &t, &f, &c, &d) < 0);
	otx_mkrhdr(out, OTB_R_DATA, 0, 4, 0L);
	check("record.data_with_ctl", otx_rhdr(out, &t, &f, &c, &d) < 0);
	otx_mkrhdr(out, OTB_R_PROTO, 0, OTB_MAXCTL + 1, 0L);
	check("record.ctl_too_long", otx_rhdr(out, &t, &f, &c, &d) < 0);
}

static void
down()
{
	struct otx_res r;
	int n;

	{	/* bind 127.0.0.1:4660, backlog 5 */
		long v[] = { OT_BIND_REQ, 16, 16, 5 }, h[] = { H_BIND_REQ, 16, 16, 5 };
		L(in, 4, v);
		inaddr(in + 16, 0x1234, 0x7f000001L);
		n = otx_xdown(in, 32, out, sizeof out, &r);
		check("down.bind", n == 32 && eq(out, 4, h) &&
		    memcmp(out + 16, in + 16, 16) == 0 && r.r_how == OX_PASS);
	}
	{	/* connect with 20 bytes of options: stripped, reported */
		long v[] = { OT_CONN_REQ, 16, 20, 20, 36 }, h[] = { H_CONN_REQ, 16, 20, 0, 0 };
		L(in, 5, v);
		inaddr(in + 20, 7, 0x0a000264L);
		memset(in + 36, 0, 20);
		n = otx_xdown(in, 56, out, sizeof out, &r);
		check("down.conn_req_opts_stripped", n == 36 && eq(out, 5, h) &&
		    r.r_optoff == 36 && r.r_optlen == 20);
	}
	{	/* a name address goes to the Mac's resolver, not here */
		long v[] = { OT_CONN_REQ, 16, 20, 0, 0 };
		L(in, 5, v);
		inaddr(in + 20, 7, 0L);
		OB_P16(in + 20, OT_AF_DNS);
		check("down.dns_address", otx_xdown(in, 36, out, sizeof out, &r) == -OT_TBADADDR);
		OB_P32(in + 8, 30);
		OB_P16(in + 20, OT_AF_INET);
		check("down.address_out_of_ctl",
		    otx_xdown(in, 36, out, sizeof out, &r) == -OT_TBADADDR);
		OB_P32(in + 4, 1);
		OB_P32(in + 8, 20);
		check("down.one_byte_address",
		    otx_xdown(in, 21, out, sizeof out, &r) == -OT_TBADADDR);
	}
	{
		long v[] = { OT_CONN_RES, 0xc0ffee, 0, 0, 9 }, h[] = { H_CONN_RES, 0, 0, 0, 9 };
		L(in, 5, v);
		n = otx_xdown(in, 20, out, sizeof out, &r);
		check("down.conn_res_cookie", n == 20 && eq(out, 5, h) &&
		    r.r_how == OX_CONNRES && r.r_cookie == 0xc0ffee && r.r_seq == 9);
	}
	{
		long v[] = { OT_ADDR_REQ };
		L(in, 1, v);
		check("down.addr_req_local", otx_xdown(in, 4, out, sizeof out, &r) == 0 &&
		    r.r_how == OX_ADDR);
		OB_P32(in, OT_RESOLVEADDR_REQ);
		check("down.unknown_notsupport",
		    otx_xdown(in, 4, out, sizeof out, &r) == -OT_TNOTSUPPORT &&
		    r.r_how == OX_NOTSUP);
		OB_P32(in, OT_BIND_REQ);
		check("down.short_ctl", otx_xdown(in, 8, out, sizeof out, &r) == -OT_TSYSERR);
	}
	{
		long v[] = { OT_DATA_REQ, 0 }, h[] = { H_DATA_REQ, 0 };
		L(in, 2, v);
		check("down.data_req", otx_xdown(in, 8, out, sizeof out, &r) == 8 &&
		    eq(out, 2, h) && r.r_data == 1);
	}
}

static void
up()
{
	struct otx_res r;
	int n;

	{	/* privileged bind refused */
		long v[] = { H_ERROR_ACK, H_BIND_REQ, 3, 13 }, o[] = { OT_ERROR_ACK, OT_BIND_REQ, OT_TACCES, OT_EACCES };
		L(in, 4, v);
		n = otx_xup(in, 16, out, sizeof out, &r);
		check("up.error_ack_tacces", n == 16 && eq(out, 4, o) && r.r_eprim == OT_BIND_REQ);
	}
	{
		long v[] = { H_ERROR_ACK, H_CONN_REQ, 8, 146 }, o[] = { OT_ERROR_ACK, OT_CONN_REQ, OT_TSYSERR, OT_ECONNREFUSED };
		L(in, 4, v);
		check("up.error_ack_errno", otx_xup(in, 16, out, sizeof out, &r) == 16 && eq(out, 4, o));
	}
	{
		long v[] = { H_DISCON_IND, 131, -1 }, o[] = { OT_DISCON_IND, OT_ECONNRESET, -1 };
		L(in, 3, v);
		check("up.discon_reason", otx_xup(in, 12, out, sizeof out, &r) == 12 && eq(out, 3, o));
	}
	{
		long v[] = { H_OK_ACK, H_UNBIND_REQ }, o[] = { OT_OK_ACK, OT_UNBIND_REQ };
		L(in, 2, v);
		check("up.ok_ack", otx_xup(in, 8, out, sizeof out, &r) == 8 && eq(out, 2, o) &&
		    r.r_eprim == OT_UNBIND_REQ);
	}
	{	/* an 8-byte source address widens to 16; options dropped */
		long v[] = { H_CONN_IND, 8, 24, 4, 32, 77 }, o[] = { OT_CONN_IND, 16, 24, 0, 0, 77 };
		L(in, 6, v);
		inaddr(in + 24, 1025, 0x0a000202L);
		memset(in + 32, 0xee, 4);
		inaddr(ref, 1025, 0x0a000202L);
		n = otx_xup(in, 36, out, sizeof out, &r);
		check("up.conn_ind_widened", n == 40 && eq(out, 6, o) &&
		    memcmp(out + 24, ref, 16) == 0 && r.r_seq == 77 &&
		    r.r_addroff == 24 && r.r_addrlen == 16);
	}
	{
		long v[] = { H_BIND_ACK, 16, 16, 0 }, o[] = { OT_BIND_ACK, 16, 16, 0 };
		L(in, 4, v);
		inaddr(in + 16, 1024, 0L);
		check("up.bind_ack", otx_xup(in, 32, out, sizeof out, &r) == 32 &&
		    eq(out, 4, o) && memcmp(out + 16, in + 16, 16) == 0);
	}
	{
		long v[] = { 99 };
		L(in, 1, v);
		check("up.unknown", otx_xup(in, 4, out, sizeof out, &r) == -1);
	}
	{
		long o[] = { OT_ADDR_ACK, 16, 20, 16, 36 };
		unsigned char la[16], ra[16];
		inaddr(la, 1, 1L);
		inaddr(ra, 2, 2L);
		check("addr_ack", otx_mkaddrack(out, la, 16, ra, 16) == 52 && eq(out, 5, o) &&
		    memcmp(out + 20, la, 16) == 0 && memcmp(out + 36, ra, 16) == 0);
		check("addr_ack_unbound", otx_mkaddrack(out, la, 0, ra, 0) == 20 &&
		    OB_G32(out + 8) == 0 && OB_G32(out + 16) == 0);
	}
	check("errno.map", otx_errno(125L) == OT_EADDRINUSE && otx_errno(145L) == OT_ETIMEDOUT &&
	    otx_errno(13L) == OT_EACCES);
	check("tlierr.map", otx_tlierr(3L) == OT_TACCES && otx_tlierr(25L) == OT_TSYSERR);
}

/* one XTI option: header, value longs */
static int
xopt(b, level, name, n, v)
	unsigned char *b;
	long level, name;
	int n;
	long *v;
{
	long h[4];

	h[0] = OT_OPTHDR + 4 * n;
	h[1] = level;
	h[2] = name;
	h[3] = 0;
	L(b, 4, h);
	return OT_OPTHDR + L(b + OT_OPTHDR, n, v);
}

static void
options()
{
	struct otx_oeng e;
	struct otx_lopt loc[OTX_NLOC];
	unsigned char req[256], rep[1024], h[128], *p;
	long one[] = { 1 }, thr[] = { 5000 }, kp[] = { 1, 30 };
	int n, len = 0;

	memset(loc, 0, sizeof loc);
	len += xopt(req + len, OT_INET_TCP, OT_TCP_NODELAY, 1, one);
	len += xopt(req + len, OT_INET_TCP, 0x99L, 1, one);
	len += xopt(req + len, OT_INET_TCP, OT_TCP_NOTIFY_THRESHOLD, 1, thr);
	len += xopt(req + len, OT_INET_TCP, OT_TCP_KEEPALIVE, 2, kp);
	check("opt.start", otx_oe_start(&e, (long)OT_NEGOTIATE, req, len, OTX_TCP, loc,
	    rep, (int)sizeof rep) == 0);
	{
		long want[] = { H_OPTMGMT_REQ, 16, 16, H_NEGOTIATE, H_IPPROTO_TCP, 1, 4, 1 };
		n = otx_oe_next(&e, h, (int)sizeof h);
		check("opt.nodelay_host_req", n == 32 && eq(h, 8, want));
	}
	{
		long ack[] = { H_OPTMGMT_ACK, 16, 16, H_NEGOTIATE, H_IPPROTO_TCP, 1, 4, 1 };
		L(h, 8, ack);
		otx_oe_host(&e, h, 32);
	}
	{
		long want[] = { H_OPTMGMT_REQ, 16, 16, H_NEGOTIATE, H_SOL_SOCKET, 8, 4, 1 };
		n = otx_oe_next(&e, h, (int)sizeof h);
		check("opt.keepalive_host_req", n == 32 && eq(h, 8, want));
	}
	{
		long err[] = { H_ERROR_ACK, H_OPTMGMT_REQ, 2, 0 };
		L(h, 4, err);
		otx_oe_host(&e, h, 16);
	}
	check("opt.done", otx_oe_next(&e, h, (int)sizeof h) == 0);
	n = otx_oe_reply(&e, out, (int)sizeof out);
	p = out + 16;
	check("opt.reply_flags", n == 16 + 4 * 20 + 4 && OB_G32(out) == OT_OPTMGMT_ACK &&
	    OB_G32(out + 12) == OT_NOTSUPPORT);
	check("opt.nodelay_success", OB_G32(p + 12) == OT_SUCCESS && OB_G32(p + 16) == 1);
	p += 20;
	check("opt.unknown_notsupport", OB_G32(p + 8) == 0x99 && OB_G32(p + 12) == OT_NOTSUPPORT);
	p += 20;
	check("opt.local_threshold", OB_G32(p + 12) == OT_SUCCESS && OB_G32(p + 16) == 5000);
	p += 20;
	check("opt.keepalive_failure", OB_G32(p) == 24 && OB_G32(p + 12) == OT_FAILURE);

	/* the stored local value and the default */
	len = xopt(req, OT_INET_TCP, OT_TCP_NOTIFY_THRESHOLD, 0, one);
	otx_oe_start(&e, (long)OT_CURRENT, req, len, OTX_TCP, loc, rep, (int)sizeof rep);
	check("opt.current_local", otx_oe_next(&e, h, (int)sizeof h) == 0 &&
	    otx_oe_reply(&e, out, (int)sizeof out) == 36 && OB_G32(out + 32) == 5000);
	otx_oe_start(&e, (long)OT_DEFAULT, req, len, OTX_TCP, loc, rep, (int)sizeof rep);
	check("opt.default_local", otx_oe_next(&e, h, (int)sizeof h) == 0 &&
	    otx_oe_reply(&e, out, (int)sizeof out) == 36 && OB_G32(out + 32) == 10000);

	/* current value from the host: T_CHECK is the host's "get" */
	len = xopt(req, OT_XTI_GENERIC, OT_XTI_SNDBUF, 0, one);
	otx_oe_start(&e, (long)OT_CURRENT, req, len, OTX_UDP, loc, rep, (int)sizeof rep);
	n = otx_oe_next(&e, h, (int)sizeof h);
	check("opt.current_host_get", n == 32 && OB_G32(h + 12) == H_CHECK &&
	    OB_G32(h + 16) == H_SOL_SOCKET && OB_G32(h + 20) == 0x1001);
	{
		long ack[] = { H_OPTMGMT_ACK, 16, 16, H_CHECK, H_SOL_SOCKET, 0x1001, 4, 8192 };
		L(h, 8, ack);
		otx_oe_host(&e, h, 32);
		check("opt.current_host_value", otx_oe_next(&e, h, (int)sizeof h) == 0 &&
		    otx_oe_reply(&e, out, (int)sizeof out) == 36 &&
		    OB_G32(out + 28) == OT_SUCCESS && OB_G32(out + 32) == 8192);
	}

	/* TCP options on a UDP endpoint; bad requests */
	len = xopt(req, OT_INET_TCP, OT_TCP_NODELAY, 1, one);
	otx_oe_start(&e, (long)OT_NEGOTIATE, req, len, OTX_UDP, loc, rep, (int)sizeof rep);
	check("opt.wrong_proto", otx_oe_next(&e, h, (int)sizeof h) == 0 &&
	    otx_oe_reply(&e, out, (int)sizeof out) > 16 && OB_G32(out + 28) == OT_NOTSUPPORT);
	check("opt.bad_flags", otx_oe_start(&e, (long)(OT_NEGOTIATE | OT_CHECK), req, len,
	    OTX_TCP, loc, rep, (int)sizeof rep) == -OT_TBADFLAG);
	OB_P32(req, 400);
	check("opt.bad_length", otx_oe_start(&e, (long)OT_NEGOTIATE, req, len, OTX_TCP, loc,
	    rep, (int)sizeof rep) == -OT_TBADOPT);
	len = xopt(req, OT_INET_TCP, OT_TCP_NODELAY, 2, kp);
	otx_oe_start(&e, (long)OT_NEGOTIATE, req, len, OTX_TCP, loc, rep, (int)sizeof rep);
	check("opt.bad_value_size", otx_oe_next(&e, h, (int)sizeof h) == 0 &&
	    otx_oe_reply(&e, out, (int)sizeof out) > 16 && OB_G32(out + 28) == OT_FAILURE);
}

int
main()
{
	records();
	down();
	up();
	options();
	printf("otxtest: %d pass, %d fail\n", passes, fails);
	return fails != 0;
}
