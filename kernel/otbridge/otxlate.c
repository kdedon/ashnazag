/*
 * otxlate.c -- OT TPI dialect <-> host TPI: primitive numbers,
 * addresses, errors and the XTI option engine.  Pure functions on byte
 * buffers, shared by the kernel module and the host tests.
 *
 * K&R C.
 */

#ifdef OTB_HOST
#include <string.h>
#endif
#include "otbridge.h"

#ifndef OTB_HOST
extern void bcopy(), bzero();
#define	memcpy(d, s, n)	bcopy((char *)(s), (char *)(d), n)
#define	memset(d, c, n)	bzero((char *)(d), n)
#endif

#define	OTX_TCP_	OTX_TCP
#define	OTX_UDP_	OTX_UDP

struct otx_prim otx_down[] = {
	/* ot		host		len  how	 addr opt data */
	{ OT_BIND_REQ,	H_BIND_REQ,	16, OX_PASS,	 4, 0, 0 },
	{ OT_CONN_REQ,	H_CONN_REQ,	20, OX_PASS,	 4, 12, 0 },
	{ OT_CONN_RES,	H_CONN_RES,	20, OX_CONNRES,	 0, 8, 0 },
	{ OT_DATA_REQ,	H_DATA_REQ,	 8, OX_PASS,	 0, 0, 1 },
	{ OT_DISCON_REQ, H_DISCON_REQ,	 8, OX_PASS,	 0, 0, 0 },
	{ OT_EXDATA_REQ, H_EXDATA_REQ,	 8, OX_PASS,	 0, 0, 1 },
	{ OT_INFO_REQ,	H_INFO_REQ,	 4, OX_PASS,	 0, 0, 0 },
	{ OT_OPTMGMT_REQ, H_OPTMGMT_REQ, 16, OX_OPTMGMT, 0, 4, 0 },
	{ OT_ORDREL_REQ, H_ORDREL_REQ,	 4, OX_PASS,	 0, 0, 0 },
	{ OT_UNBIND_REQ, H_UNBIND_REQ,	 4, OX_PASS,	 0, 0, 0 },
	{ OT_UNITDATA_REQ, H_UNITDATA_REQ, 20, OX_PASS,	 4, 12, 1 },
	{ OT_ADDR_REQ,	-1,		 4, OX_ADDR,	 0, 0, 0 },
	{ 0 }
};

struct otx_prim otx_up[] = {
	{ OT_CONN_IND,	H_CONN_IND,	24, 0, 4, 12, 0 },
	{ OT_CONN_CON,	H_CONN_CON,	20, 0, 4, 12, 0 },
	{ OT_DISCON_IND, H_DISCON_IND,	12, 0, 0, 0, 0 },
	{ OT_DATA_IND,	H_DATA_IND,	 8, 0, 0, 0, 1 },
	{ OT_EXDATA_IND, H_EXDATA_IND,	 8, 0, 0, 0, 1 },
	{ OT_INFO_ACK,	H_INFO_ACK,	44, 0, 0, 0, 0 },
	{ OT_BIND_ACK,	H_BIND_ACK,	16, 0, 4, 0, 0 },
	{ OT_ERROR_ACK,	H_ERROR_ACK,	16, 0, 0, 0, 0 },
	{ OT_OK_ACK,	H_OK_ACK,	 8, 0, 0, 0, 0 },
	{ OT_UNITDATA_IND, H_UNITDATA_IND, 20, 0, 4, 12, 1 },
	{ OT_UDERROR_IND, H_UDERROR_IND, 24, 0, 4, 12, 0 },
	{ OT_OPTMGMT_ACK, H_OPTMGMT_ACK, 16, 0, 0, 4, 0 },
	{ OT_ORDREL_IND, H_ORDREL_IND,	 4, 0, 0, 0, 0 },
	{ 0 }
};

/* host errno -> OT (BSD) errno, where they differ */
static short errmap[][2] = {
	{ 11, 35 },		/* EAGAIN */
	{ 63, 72 },		/* ENOSR */
	{ 95, 38 }, { 96, 39 }, { 97, 40 }, { 98, 41 }, { 99, 42 },
	{ 120, 43 }, { 121, 44 }, { 122, 45 }, { 123, 46 }, { 124, 47 },
	{ 125, 48 },		/* EADDRINUSE */
	{ 126, 49 }, { 127, 50 }, { 128, 51 }, { 129, 52 }, { 130, 53 },
	{ 131, 54 },		/* ECONNRESET */
	{ 132, 55 }, { 133, 56 }, { 134, 57 }, { 143, 58 }, { 144, 59 },
	{ 145, 60 },		/* ETIMEDOUT */
	{ 146, 61 },		/* ECONNREFUSED */
	{ 147, 64 }, { 148, 65 }, { 149, 37 }, { 150, 36 },
	{ 0, 0 }
};

struct otx_opt otx_opts[] = {
	/* OT level, name, proto, kind, host level, name, len, conv, default */
	{ OT_INET_TCP, OT_TCP_NODELAY, OTX_TCP_, OK_HOST, H_IPPROTO_TCP, 1, 4, OC_INT, 0 },
	{ OT_INET_TCP, OT_TCP_MAXSEG, OTX_TCP_, OK_HOST, H_IPPROTO_TCP, 2, 4, OC_INT, 0 },
	{ OT_INET_TCP, OT_TCP_KEEPALIVE, OTX_TCP_, OK_HOST, H_SOL_SOCKET, 0x8, 8, OC_KPALIVE, 120 },
	{ OT_INET_TCP, OT_TCP_NOTIFY_THRESHOLD, OTX_TCP_, OK_LOCAL, 0, 0, 4, OC_INT, 10000 },
	{ OT_INET_TCP, OT_TCP_ABORT_THRESHOLD, OTX_TCP_, OK_LOCAL, 0, 0, 4, OC_INT, 240000 },
	{ OT_INET_TCP, OT_TCP_CONN_NOTIFY_THRESHOLD, OTX_TCP_, OK_LOCAL, 0, 0, 4, OC_INT, 10000 },
	{ OT_INET_TCP, OT_TCP_CONN_ABORT_THRESHOLD, OTX_TCP_, OK_LOCAL, 0, 0, 4, OC_INT, 75000 },
	{ OT_INET_TCP, OT_TCP_OOBINLINE, OTX_TCP_, OK_LOCAL, 0, 0, 4, OC_INT, 0 },
	{ OT_INET_TCP, OT_TCP_URGENT_PTR_TYPE, OTX_TCP_, OK_LOCAL, 0, 0, 4, OC_INT, 0 },
	{ OT_INET_UDP, OT_UDP_CHECKSUM, OTX_UDP_, OK_LOCAL, 0, 0, 4, OC_INT, 1 },
	{ OT_INET_IP, OT_IP_OPTIONS, 0, OK_HOST, H_IPPROTO_IP, 1, 0, OC_RAW, 0 },
	{ OT_INET_IP, OT_IP_REUSEADDR, 0, OK_HOST, H_SOL_SOCKET, 0x4, 4, OC_INT, 0 },
	{ OT_INET_IP, OT_IP_DONTROUTE, 0, OK_HOST, H_SOL_SOCKET, 0x10, 4, OC_INT, 0 },
	{ OT_INET_IP, OT_IP_BROADCAST, 0, OK_HOST, H_SOL_SOCKET, 0x20, 4, OC_INT, 0 },
	{ OT_XTI_GENERIC, OT_XTI_DEBUG, 0, OK_HOST, H_SOL_SOCKET, 0x1, 4, OC_INT, 0 },
	{ OT_XTI_GENERIC, OT_XTI_LINGER, 0, OK_HOST, H_SOL_SOCKET, 0x80, 8, OC_LINGER, 0 },
	{ OT_XTI_GENERIC, OT_XTI_SNDBUF, 0, OK_HOST, H_SOL_SOCKET, 0x1001, 4, OC_INT, 0 },
	{ OT_XTI_GENERIC, OT_XTI_RCVBUF, 0, OK_HOST, H_SOL_SOCKET, 0x1002, 4, OC_INT, 0 },
	{ OT_XTI_GENERIC, OT_XTI_SNDLOWAT, 0, OK_HOST, H_SOL_SOCKET, 0x1003, 4, OC_INT, 0 },
	{ OT_XTI_GENERIC, OT_XTI_RCVLOWAT, 0, OK_HOST, H_SOL_SOCKET, 0x1004, 4, OC_INT, 0 },
	{ 0 }
};

struct otx_prim *
otx_pfind(tab, prim, byhost)
	struct otx_prim *tab;
	long prim;
	int byhost;
{
	for (; tab->p_ot; tab++)
		if ((byhost ? tab->p_host : tab->p_ot) == prim)
			return tab;
	return 0;
}

long
otx_errno(e)
	long e;
{
	int i;

	for (i = 0; errmap[i][0]; i++)
		if (errmap[i][0] == e)
			return errmap[i][1];
	return e;
}

long
otx_tlierr(e)
	long e;
{
	return e >= 1 && e <= 19 ? e : OT_TSYSERR;
}

/* record header: 0, or -1 if it cannot be a record */
int
otx_rhdr(h, type, flags, ctllen, datalen)
	unsigned char *h;
	int *type, *flags, *ctllen;
	long *datalen;
{
	*type = h[0];
	*flags = h[1];
	*ctllen = OB_G16(h + 2);
	*datalen = OB_S32(OB_G32(h + 4));
	if (*type != OTB_R_DATA && *type != OTB_R_PROTO && *type != OTB_R_PCPROTO)
		return -1;
	if (*ctllen > OTB_MAXCTL || *datalen < 0 || *datalen > OTB_MAXDATA)
		return -1;
	if (*type == OTB_R_DATA ? *ctllen != 0 : *ctllen < 4)
		return -1;
	return 0;
}

void
otx_mkrhdr(h, type, flags, ctllen, datalen)
	unsigned char *h;
	int type, flags, ctllen;
	long datalen;
{
	h[0] = type;
	h[1] = flags;
	OB_P16(h + 2, ctllen);
	OB_P32(h + 4, datalen);
}

/* [off, off + len) inside a control part of n bytes */
static int
inctl(off, len, n)
	unsigned long off, len;
	int n;
{
	return off <= (unsigned long)n && len <= (unsigned long)n - off;
}

/*
 * Copy the fixed part of ctl, renumber, move the address behind it and
 * drop the options (their place in the input goes into res).  down: the
 * address must be AF_INET; up: an AF_INET address is widened to 16
 * bytes.  Returns the output length, or -(OT TLI error).
 */
static int
xcommon(p, ctl, len, out, max, res, down)
	struct otx_prim *p;
	unsigned char *ctl, *out;
	int len, max, down;
	struct otx_res *res;
{
	unsigned long al, ao, ol, oo;
	int n = p->p_len;

	if (len < p->p_len || max < p->p_len + OT_INETADDR_LEN)
		return -OT_TSYSERR;
	memcpy(out, ctl, p->p_len);
	OB_P32(out, down ? p->p_host : p->p_ot);
	res->r_prim = p->p_ot;
	res->r_hprim = p->p_host;
	res->r_data = p->p_data;
	res->r_optoff = res->r_optlen = 0;
	res->r_addroff = res->r_addrlen = 0;
	if (p->p_addr) {
		al = OB_G32(ctl + p->p_addr);
		ao = OB_G32(ctl + p->p_addr + 4);
		if (al && !inctl(ao, al, len))
			return -OT_TBADADDR;
		if (al >= 2 && OB_G16(ctl + ao) == OT_AF_INET) {
			if (al < 8 || (down && al > OT_INETADDR_LEN))
				return -OT_TBADADDR;
			memset(out + n, 0, OT_INETADDR_LEN);
			memcpy(out + n, ctl + ao, al > OT_INETADDR_LEN ? OT_INETADDR_LEN : al);
			al = OT_INETADDR_LEN;
		} else if (al && down)
			return -OT_TBADADDR;
		else if (al) {
			if (OT_OPTALIGN(al) > (unsigned long)(max - n))
				return -OT_TSYSERR;
			memcpy(out + n, ctl + ao, al);
		}
		OB_P32(out + p->p_addr, al);
		OB_P32(out + p->p_addr + 4, al ? n : 0);
		res->r_addroff = n;
		res->r_addrlen = al;
		n += OT_OPTALIGN(al);
	}
	if (p->p_opt) {
		ol = OB_G32(ctl + p->p_opt);
		oo = OB_G32(ctl + p->p_opt + 4);
		if (ol && !inctl(oo, ol, len))
			return -OT_TBADOPT;
		res->r_optoff = ol ? oo : 0;
		res->r_optlen = ol;
		OB_P32(out + p->p_opt, 0);
		OB_P32(out + p->p_opt + 4, 0);
	}
	return n;
}

/* OT -> host */
int
otx_xdown(ctl, len, out, max, res)
	unsigned char *ctl, *out;
	int len, max;
	struct otx_res *res;
{
	struct otx_prim *p;
	int n;

	memset(res, 0, sizeof *res);
	if (len < 4)
		return -OT_TSYSERR;
	res->r_prim = OB_G32(ctl);
	if ((p = otx_pfind(otx_down, res->r_prim, 0)) == 0) {
		res->r_how = OX_NOTSUP;
		return -OT_TNOTSUPPORT;
	}
	res->r_how = p->p_how;
	if (p->p_host < 0) {
		res->r_hprim = -1;
		return 0;
	}
	if ((n = xcommon(p, ctl, len, out, max, res, 1)) < 0)
		return n;
	if (p->p_ot == OT_CONN_RES) {
		res->r_cookie = OB_G32(ctl + 4);
		res->r_seq = OB_S32(OB_G32(ctl + 16));
		OB_P32(out + 4, 0);
	}
	return n;
}

/* host -> OT; -1 for a primitive the table does not know */
int
otx_xup(ctl, len, out, max, res)
	unsigned char *ctl, *out;
	int len, max;
	struct otx_res *res;
{
	struct otx_prim *p, *q;
	long v;
	int n;

	memset(res, 0, sizeof *res);
	if (len < 4 || (p = otx_pfind(otx_up, (long)OB_G32(ctl), 1)) == 0)
		return -1;
	if ((n = xcommon(p, ctl, len, out, max, res, 0)) < 0)
		return n;
	switch (p->p_ot) {
	case OT_ERROR_ACK:
		v = OB_S32(OB_G32(ctl + 4));
		q = otx_pfind(otx_down, v, 1);
		res->r_eprim = q ? q->p_ot : v;
		OB_P32(out + 4, res->r_eprim);
		OB_P32(out + 8, otx_tlierr(OB_S32(OB_G32(ctl + 8))));
		OB_P32(out + 12, otx_errno(OB_S32(OB_G32(ctl + 12))));
		break;
	case OT_OK_ACK:
		v = OB_S32(OB_G32(ctl + 4));
		q = otx_pfind(otx_down, v, 1);
		res->r_eprim = q ? q->p_ot : v;
		OB_P32(out + 4, res->r_eprim);
		break;
	case OT_DISCON_IND:
		OB_P32(out + 4, otx_errno(OB_S32(OB_G32(ctl + 4))));
		break;
	case OT_UDERROR_IND:
		OB_P32(out + 20, otx_errno(OB_S32(OB_G32(ctl + 20))));
		break;
	case OT_CONN_IND:
		res->r_seq = OB_S32(OB_G32(ctl + 20));
		break;
	}
	return n;
}

int
otx_mkerr(out, prim, tlierr, unixerr)
	unsigned char *out;
	long prim, tlierr, unixerr;
{
	OB_P32(out, OT_ERROR_ACK);
	OB_P32(out + 4, prim);
	OB_P32(out + 8, tlierr);
	OB_P32(out + 12, unixerr);
	return 16;
}

int
otx_mkaddrack(out, la, lalen, ra, ralen)
	unsigned char *out, *la, *ra;
	int lalen, ralen;
{
	int n = OT_ADDRACK_LEN;

	OB_P32(out, OT_ADDR_ACK);
	OB_P32(out + 4, lalen);
	OB_P32(out + 8, lalen ? n : 0);
	memcpy(out + n, la, lalen);
	n += lalen;
	OB_P32(out + 12, ralen);
	OB_P32(out + 16, ralen ? n : 0);
	memcpy(out + n, ra, ralen);
	return n + ralen;
}

/* ---- option engine ---- */

/* worst first: NOTSUPPORT, READONLY, FAILURE, PARTSUCCESS, SUCCESS */
static int
rank(st)
	long st;
{
	switch (st) {
	case OT_NOTSUPPORT:	return 5;
	case OT_READONLY:	return 4;
	case OT_FAILURE:	return 3;
	case OT_PARTSUCCESS:	return 2;
	default:		return 1;
	}
}

static struct otx_opt *
ofind(level, name, proto)
	long level, name;
	int proto;
{
	struct otx_opt *o;

	for (o = otx_opts; o->o_kind; o++)
		if (o->o_level == level && o->o_name == name)
			return o->o_proto == 0 || proto == 0 || o->o_proto == proto ? o : 0;
	return 0;
}

static struct otx_lopt *
lslot(e, level, name, make)
	struct otx_oeng *e;
	long level, name;
	int make;
{
	struct otx_lopt *l, *fr = 0;

	for (l = e->e_loc; l < e->e_loc + OTX_NLOC; l++)
		if (l->l_name == name && l->l_level == level)
			return l;
		else if (l->l_name == 0 && fr == 0)
			fr = l;
	if (make && fr) {
		fr->l_level = level;
		fr->l_name = name;
	}
	return make ? fr : 0;
}

static void
append(e, level, name, st, val, vlen)
	struct otx_oeng *e;
	long level, name, st;
	unsigned char *val;
	int vlen;
{
	unsigned char *o;

	if (vlen < 0 || vlen > OE_MAXVAL)
		vlen = 0;
	if (e->e_replen + OT_OPTHDR + OT_OPTALIGN(vlen) > e->e_repmax) {
		if (rank((long)OT_FAILURE) > e->e_worst)
			e->e_worst = rank((long)OT_FAILURE);
		return;
	}
	o = e->e_rep + e->e_replen;
	OB_P32(o, OT_OPTHDR + vlen);
	OB_P32(o + 4, level);
	OB_P32(o + 8, name);
	OB_P32(o + 12, st);
	memset(o + OT_OPTHDR, 0, OT_OPTALIGN(vlen));
	if (vlen)
		memcpy(o + OT_OPTHDR, val, vlen);
	e->e_replen += OT_OPTHDR + OT_OPTALIGN(vlen);
	if (rank(st) > e->e_worst)
		e->e_worst = rank(st);
}

int
otx_oe_repsize(reqlen)
	int reqlen;
{
	return reqlen + (reqlen / OT_OPTHDR + 1) * OE_MAXVAL;
}

/* 0, or -(OT TLI error) for the whole request */
int
otx_oe_start(e, flags, opts, len, proto, loc, rep, repmax)
	struct otx_oeng *e;
	long flags;
	unsigned char *opts, *rep;
	int len, proto, repmax;
	struct otx_lopt *loc;
{
	unsigned long ol;
	int off;

	if (flags != OT_NEGOTIATE && flags != OT_CHECK && flags != OT_DEFAULT &&
	    flags != OT_CURRENT)
		return -OT_TBADFLAG;
	for (off = 0; off < len; off += OT_OPTALIGN(ol)) {
		if (len - off < OT_OPTHDR)
			return -OT_TBADOPT;
		ol = OB_G32(opts + off);
		if (ol < OT_OPTHDR || ol > (unsigned long)(len - off))
			return -OT_TBADOPT;
	}
	e->e_req = opts;
	e->e_reqlen = len;
	e->e_off = 0;
	e->e_flags = flags;
	e->e_proto = proto;
	e->e_rep = rep;
	e->e_replen = 0;
	e->e_repmax = repmax;
	e->e_worst = 0;
	e->e_loc = loc;
	e->e_cur = 0;
	return 0;
}

/*
 * Answer options locally until one needs the host.  Returns the length
 * of the host T_OPTMGMT_REQ built in hreq, or 0 when all are answered.
 */
int
otx_oe_next(e, hreq, max)
	struct otx_oeng *e;
	unsigned char *hreq;
	int max;
{
	struct otx_opt *r;
	struct otx_lopt *l;
	unsigned char *o, *val, b[8];
	long level, name, v;
	int vlen, hvlen, n;

	while (e->e_off < e->e_reqlen) {
		o = e->e_req + e->e_off;
		vlen = OB_G32(o) - OT_OPTHDR;
		level = OB_S32(OB_G32(o + 4));
		name = OB_S32(OB_G32(o + 8));
		val = o + OT_OPTHDR;
		e->e_off += OT_OPTALIGN(vlen + OT_OPTHDR);
		r = ofind(level, name, e->e_proto);
		if (r == 0 || r->o_kind == OK_NOTSUP) {
			append(e, level, name, (long)OT_NOTSUPPORT, val, vlen);
			continue;
		}
		if ((e->e_flags == OT_NEGOTIATE || e->e_flags == OT_CHECK) &&
		    (r->o_len ? vlen != r->o_len : vlen < 1 || vlen > OE_MAXVAL)) {
			append(e, level, name, (long)OT_FAILURE, val, vlen);
			continue;
		}
		if (r->o_kind == OK_LOCAL) {
			l = lslot(e, level, name, e->e_flags == OT_NEGOTIATE);
			if (e->e_flags == OT_NEGOTIATE && l == 0) {
				append(e, level, name, (long)OT_FAILURE, val, vlen);
				continue;
			}
			switch (e->e_flags) {
			case OT_NEGOTIATE:
				l->l_val = OB_S32(OB_G32(val));
				/* FALLTHROUGH */
			case OT_CHECK:
				v = OB_S32(OB_G32(val));
				break;
			case OT_CURRENT:
				v = l ? l->l_val : r->o_def;
				break;
			default:
				v = r->o_def;
				break;
			}
			OB_P32(b, v);
			append(e, level, name, (long)OT_SUCCESS, b, 4);
			continue;
		}
		/* host */
		hvlen = r->o_conv == OC_LINGER ? 8 : r->o_conv == OC_RAW ?
		    (vlen > 0 ? vlen : 0) : 4;
		n = 16 + H_OPTHDR + OT_OPTALIGN(hvlen);
		if (n > max) {
			append(e, level, name, (long)OT_FAILURE, val, vlen);
			continue;
		}
		memset(hreq, 0, n);
		OB_P32(hreq, H_OPTMGMT_REQ);
		OB_P32(hreq + 4, H_OPTHDR + OT_OPTALIGN(hvlen));
		OB_P32(hreq + 8, 16);
		OB_P32(hreq + 12, e->e_flags == OT_NEGOTIATE ? H_NEGOTIATE :
		    e->e_flags == OT_DEFAULT ? H_DEFAULT : H_CHECK);
		OB_P32(hreq + 16, r->o_hlevel);
		OB_P32(hreq + 20, r->o_hname);
		OB_P32(hreq + 24, OT_OPTALIGN(hvlen));
		if (vlen >= hvlen && hvlen)
			memcpy(hreq + 28, val, hvlen);
		if (r->o_conv == OC_KPALIVE && vlen == 8 && e->e_flags == OT_NEGOTIATE &&
		    (l = lslot(e, level, name, 1)) != 0)
			l->l_val = OB_S32(OB_G32(val + 4));
		e->e_cur = r;
		e->e_curoff = o - e->e_req;
		return n;
	}
	return 0;
}

/* the host's answer to the request otx_oe_next built */
void
otx_oe_host(e, ack, len)
	struct otx_oeng *e;
	unsigned char *ack;
	int len;
{
	struct otx_opt *r = e->e_cur;
	struct otx_lopt *l;
	unsigned char *o, *val, *hv = 0, b[OE_MAXVAL];
	unsigned long ol, oo, hl = 0;
	long level, name, st;
	int vlen, off, n;

	if (r == 0)
		return;
	e->e_cur = 0;
	o = e->e_req + e->e_curoff;
	vlen = OB_G32(o) - OT_OPTHDR;
	level = OB_S32(OB_G32(o + 4));
	name = OB_S32(OB_G32(o + 8));
	val = o + OT_OPTHDR;
	if (len < 16 || OB_G32(ack) != H_OPTMGMT_ACK ||
	    (OB_G32(ack + 12) & H_FAILURE)) {
		append(e, level, name, (long)OT_FAILURE, val, vlen);
		return;
	}
	ol = OB_G32(ack + 4);
	oo = OB_G32(ack + 8);
	if (inctl(oo, ol, len))
		for (off = 0; off + H_OPTHDR <= ol; off += H_OPTHDR + OT_OPTALIGN(hl)) {
			hl = OB_G32(ack + oo + off + 8);
			if (hl > ol - off - H_OPTHDR)
				break;
			if (OB_G32(ack + oo + off) == r->o_hlevel &&
			    OB_G32(ack + oo + off + 4) == r->o_hname) {
				hv = ack + oo + off + H_OPTHDR;
				break;
			}
		}
	if (hv == 0 || e->e_flags == OT_CHECK) {
		st = e->e_flags == OT_NEGOTIATE || e->e_flags == OT_CHECK ?
		    OT_SUCCESS : OT_FAILURE;
		append(e, level, name, st, val, vlen);
		return;
	}
	switch (r->o_conv) {
	case OC_KPALIVE:
		OB_P32(b, hl >= 4 && OB_G32(hv) ? 1 : 0);
		l = lslot(e, level, name, 0);
		OB_P32(b + 4, l ? l->l_val : r->o_def);
		n = 8;
		break;
	case OC_LINGER:
		memset(b, 0, 8);
		memcpy(b, hv, hl >= 8 ? 8 : hl >= 4 ? 4 : 0);
		n = 8;
		break;
	case OC_RAW:
		n = hl > OE_MAXVAL ? OE_MAXVAL : hl;
		memcpy(b, hv, n);
		break;
	default:
		memset(b, 0, 4);
		memcpy(b, hv, hl >= 4 ? 4 : 0);
		n = 4;
		break;
	}
	append(e, level, name, (long)OT_SUCCESS, b, n);
}

/* OT T_OPTMGMT_ACK */
int
otx_oe_reply(e, out, max)
	struct otx_oeng *e;
	unsigned char *out;
	int max;
{
	static long st[] = { OT_SUCCESS, OT_SUCCESS, OT_PARTSUCCESS, OT_FAILURE,
		OT_READONLY, OT_NOTSUPPORT };

	if (max < 16 + e->e_replen)
		return -OT_TSYSERR;
	OB_P32(out, OT_OPTMGMT_ACK);
	OB_P32(out + 4, e->e_replen);
	OB_P32(out + 8, e->e_replen ? 16 : 0);
	OB_P32(out + 12, st[e->e_worst]);
	memcpy(out + 16, e->e_rep, e->e_replen);
	return 16 + e->e_replen;
}
