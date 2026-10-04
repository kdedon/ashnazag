/*
 * otxti.c -- STREAMS module pushed on a host transport stream (/dev/tcp,
 * /dev/udp) for one Open Transport endpoint.
 *
 * Down, each M_DATA record from the Mac becomes a host TPI message; up,
 * each host message becomes one M_DATA record read in RMSGN mode:
 *
 *	Mac  write(record) -> otxwput: cut records, putq
 *	                      otxwsrv: translate, putnext -> tcp/udp
 *	Mac  read(record)  <- otxrsrv <- otxrput: translate <- tcp/udp
 *
 * Options go through the option engine, one host T_OPTMGMT_REQ per
 * option; the write queue waits meanwhile.  Readable and writable
 * events go to the attached session's ring.
 *
 * K&R C.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/stream.h"
#include "sys/stropts.h"
#include "sys/errno.h"
#include "sys/cred.h"
#include "sys/immu.h"
#include "sys/proc.h"
#include "sys/cmn_err.h"
#include "otbridge.h"

extern struct proc *curproc;
extern int canput(), putq(), putbq(), pullupmsg(), msgdsize();
extern int bufcall(), putctl1();
extern void qenable(), freemsg(), freeb(), flushq(), qreply(), linkb(), unbufcall();
extern mblk_t *allocb(), *dupb(), *getq();
extern void bcopy(), bzero();

static int otxopen(), otxclose(), otxwput(), otxwsrv(), otxrput(), otxrsrv();

static struct module_info otx_minfo = {
	0x6f78, "otxti", 0, INFPSZ, 16384, 4096,
};

static struct qinit otx_rinit = {
	otxrput, otxrsrv, otxopen, otxclose, 0, &otx_minfo, 0,
};

static struct qinit otx_winit = {
	otxwput, otxwsrv, 0, 0, 0, &otx_minfo, 0,
};

struct streamtab otxtiinfo = { &otx_rinit, &otx_winit, 0, 0 };

/* option engine steps (x_oest) */
#define	OES_SEND	1	/* build and send the next host request */
#define	OES_WAIT	2	/* waiting for the host's answer */

#define	RAWNEXT(mp)	(mp)->b_next

static int	otx_upmsg();
static void	otx_orun();

/*
 * Cut mp after n bytes; returns the rest (0 if none), or -1 if a block
 * had to be shared and dupb failed.
 */
static mblk_t *
cut(mp, n)
	mblk_t *mp;
	long n;
{
	mblk_t *b, *r;
	long l;

	for (b = mp; b; b = b->b_cont) {
		l = b->b_wptr - b->b_rptr;
		if (n < l) {
			if ((r = dupb(b)) == 0)
				return (mblk_t *)-1;
			r->b_rptr += n;
			r->b_cont = b->b_cont;
			b->b_wptr = b->b_rptr + n;
			b->b_cont = 0;
			return r;
		}
		n -= l;
		if (n == 0) {
			r = b->b_cont;
			b->b_cont = 0;
			return r;
		}
	}
	return 0;
}

static void
retry(q, x, n)
	queue_t *q;
	struct otx *x;
	int n;
{
	int *id = q->q_flag & QREADR ? &x->x_rbufcall : &x->x_wbufcall;

	if (*id == 0)
		*id = bufcall((unsigned)n, BPRI_MED, qenable, (long)q);
}

/* ---- open and close ---- */

/*ARGSUSED*/
static int
otxopen(q, devp, flag, sflag, cr)
	queue_t *q;
	dev_t *devp;
	int flag, sflag;
	cred_t *cr;
{
	struct stroptions *so;
	struct otx *x;
	mblk_t *mp;
	int s;

	if (sflag != MODOPEN)
		return EINVAL;
	if (q->q_ptr)
		return 0;
	otb_incall++;
	if ((mp = allocb(sizeof (struct stroptions), BPRI_MED)) == 0) {
		otb_incall--;
		return ENOSR;
	}
	x = (struct otx *)otb_alloc(sizeof (struct otx));
	x->x_rq = q;
	x->x_ringidx = -1;
	OTB_SPLSTR(s);
	x->x_next = otb_eps;
	otb_eps = x;
	otb_st.st_endpoints++;
	OTB_SPLX(s);
	q->q_ptr = WR(q)->q_ptr = (caddr_t)x;

	/* one read returns one record */
	mp->b_datap->db_type = M_SETOPTS;
	so = (struct stroptions *)mp->b_rptr;
	bzero((caddr_t)so, sizeof *so);
	so->so_flags = SO_READOPT;
	so->so_readopt = RMSGN;
	mp->b_wptr += sizeof *so;
	putnext(q, mp);
	otb_incall--;
	return 0;
}

static void
freeraw(x)
	struct otx *x;
{
	if (x->x_pend) {
		freemsg(x->x_pend);
		x->x_pend = 0;
	}
}

static void
oefree(x)
	struct otx *x;
{
	if (x->x_oebuf) {
		freeb(x->x_oebuf);
		x->x_oebuf = 0;
	}
	x->x_oe.e_req = x->x_oe.e_rep = 0;
	x->x_flags &= ~(XF_OPT | XF_OPTQUIET);
}

/*ARGSUSED*/
static int
otxclose(q, flag, cr)
	queue_t *q;
	int flag;
	cred_t *cr;
{
	struct otx *x = (struct otx *)q->q_ptr, **pp;
	mblk_t *mp;
	int s;

	/* at splstr throughout: the driver below puts at interrupt level */
	otb_incall++;
	OTB_SPLSTR(s);
	otb_sdetach(x);
	for (pp = &otb_eps; *pp; pp = &(*pp)->x_next)
		if (*pp == x) {
			*pp = x->x_next;
			break;
		}
	otb_st.st_endpoints--;
	if (x->x_rbufcall)
		unbufcall(x->x_rbufcall);
	if (x->x_wbufcall)
		unbufcall(x->x_wbufcall);
	if (x->x_part)
		freemsg(x->x_part);
	while ((mp = x->x_rraw) != 0) {
		x->x_rraw = RAWNEXT(mp);
		RAWNEXT(mp) = 0;
		freemsg(mp);
	}
	freeraw(x);
	oefree(x);
	q->q_ptr = WR(q)->q_ptr = 0;
	OTB_SPLX(s);
	otb_free((char *)x, sizeof (struct otx));
	otb_incall--;
	return 0;
}

/* ---- down ---- */

/* the Mac sent something that is not a record: the stream is unusable */
static void
proterr(q, x)
	queue_t *q;
	struct otx *x;
{
	if (x->x_part) {
		freemsg(x->x_part);
		x->x_part = 0;
	}
	(void)putctl1(RD(q)->q_next, M_ERROR, EPROTO);
	otb_post(x, OTB_EV_ERR);
}

static void
wdata(q, x, mp)
	queue_t *q;
	struct otx *x;
	mblk_t *mp;
{
	mblk_t *rest;
	long n, dlen;
	int type, fl, clen;

	if (x->x_part)
		linkb(x->x_part, mp);
	else
		x->x_part = mp;
	while (x->x_part && (n = msgdsize(x->x_part)) >= OTB_RHDR) {
		if (!pullupmsg(x->x_part, OTB_RHDR) ||
		    otx_rhdr(x->x_part->b_rptr, &type, &fl, &clen, &dlen) < 0) {
			proterr(q, x);
			return;
		}
		if (n < OTB_RHDR + clen + dlen)
			break;
		if ((rest = cut(x->x_part, OTB_RHDR + clen + dlen)) == (mblk_t *)-1) {
			proterr(q, x);
			return;
		}
		(void)putq(q, x->x_part);
		x->x_part = rest;
	}
	if (q->q_flag & QFULL)
		x->x_flags |= XF_WBLOCK;
}

static void
attach(q, x, mp)
	queue_t *q;
	struct otx *x;
	mblk_t *mp;
{
	struct iocblk *ioc = (struct iocblk *)mp->b_rptr;
	struct otbs *ss;
	struct otx *e;
	unsigned char *a;
	unsigned long cookie;
	long proto;
	int err = 0, s;

	if (ioc->ioc_count != sizeof (struct otx_attach) || mp->b_cont == 0 ||
	    !pullupmsg(mp->b_cont, sizeof (struct otx_attach))) {
		err = EINVAL;
		goto out;
	}
	a = mp->b_cont->b_rptr;
	cookie = OB_G32(a + 4);
	proto = OB_S32(OB_G32(a + 8));
	OTB_SPLSTR(s);
	if ((ss = otb_sfind(OB_S32(OB_G32(a)))) == 0)
		err = ENXIO;
	else if (ss->s_proc != curproc || ss->s_pid != curproc->p_pid)
		err = EPERM;
	else if (x->x_sess)
		err = EBUSY;
	else if (proto != OTX_TCP && proto != OTX_UDP)
		err = EINVAL;
	else {
		for (e = ss->s_eps; e; e = e->x_snext)
			if (e->x_cookie == cookie)
				err = EEXIST;
		if (err == 0) {
			x->x_cookie = cookie;
			x->x_proto = proto;
			x->x_sess = ss;
			x->x_snext = ss->s_eps;
			ss->s_eps = x;
		}
	}
	OTB_SPLX(s);
out:
	if (mp->b_cont) {
		freemsg(mp->b_cont);
		mp->b_cont = 0;
	}
	ioc->ioc_count = 0;
	ioc->ioc_rval = 0;
	ioc->ioc_error = err;
	mp->b_datap->db_type = err ? M_IOCNAK : M_IOCACK;
	qreply(q, mp);
}

static int
otxwput(q, mp)
	queue_t *q;
	mblk_t *mp;
{
	struct otx *x = (struct otx *)q->q_ptr;

	if (x == 0) {
		putnext(q, mp);
		return 0;
	}
	switch (mp->b_datap->db_type) {
	case M_DATA:
		wdata(q, x, mp);
		return 0;
	case M_FLUSH:
		if (*mp->b_rptr & FLUSHW) {
			flushq(q, FLUSHDATA);
			if (x->x_part) {
				freemsg(x->x_part);
				x->x_part = 0;
			}
		}
		break;
	case M_IOCTL:
		if (((struct iocblk *)mp->b_rptr)->ioc_cmd == OTX_ATTACH) {
			attach(q, x, mp);
			return 0;
		}
		break;
	}
	putnext(q, mp);
	return 0;
}

/* one record up; the ring hears about it */
static void
uprec(q, x, rec)
	queue_t *q;
	struct otx *x;
	mblk_t *rec;
{
	mblk_t *rest, *h;
	long n, clen;

	while (rec) {
		n = msgdsize(rec);
		rest = 0;
		if (n > OTB_MAXREC) {
			clen = OB_G16(rec->b_rptr + 2);
			if ((h = allocb(OTB_RHDR, BPRI_MED)) == 0 ||
			    (rest = cut(rec, (long)OTB_MAXREC)) == (mblk_t *)-1) {
				if (h)
					freeb(h);
				freemsg(rec);
				otb_post(x, OTB_EV_ERR);
				return;
			}
			rec->b_rptr[1] |= OTB_RF_MORE;
			OB_P32(rec->b_rptr + 4, OTB_MAXREC - OTB_RHDR - clen);
			otx_mkrhdr(h->b_wptr, OTB_R_DATA, 0, 0, n - OTB_MAXREC);
			if (rec->b_rptr[0] != OTB_R_DATA)
				h->b_wptr[0] = rec->b_rptr[0];
			h->b_wptr += OTB_RHDR;
			h->b_cont = rest;
			rest = h;
		}
		if (q->q_first == 0 && canput(q->q_next))
			putnext(q, rec);
		else
			(void)putq(q, rec);
		rec = rest;
	}
	otb_post(x, OTB_EV_READ);
}

static void
uperr(q, x, prim, tli, ux)
	queue_t *q;
	struct otx *x;
	long prim, tli, ux;
{
	mblk_t *c;

	if ((c = allocb(OTB_RHDR + 16, BPRI_HI)) == 0) {
		otb_post(x, OTB_EV_ERR);
		return;
	}
	otx_mkrhdr(c->b_wptr, OTB_R_PCPROTO, 0, 16, 0L);
	c->b_wptr += OTB_RHDR;
	c->b_wptr += otx_mkerr(c->b_wptr, prim, tli, ux);
	uprec(RD(q), x, c);
}

static struct otx *
bycookie(x, cookie)
	struct otx *x;
	unsigned long cookie;
{
	struct otx *e;

	if (x->x_sess == 0)
		return x->x_cookie == cookie ? x : 0;
	for (e = x->x_sess->s_eps; e; e = e->x_snext)
		if (e->x_cookie == cookie)
			return e;
	return 0;
}

/* bytes for the engine's copy of len option bytes and its reply */
#define	OEBUFSZ(len)	((len) + otx_oe_repsize(len))

/*
 * Start the engine on opts in *obp, a block of OEBUFSZ(len) bytes that
 * the endpoint then owns; 0, or -(OT TLI error).
 */
static int
oestart(x, flags, opts, len, obp)
	struct otx *x;
	long flags;
	unsigned char *opts;
	int len;
	mblk_t **obp;
{
	unsigned char *b = (*obp)->b_rptr;
	int e;

	bcopy((caddr_t)opts, (caddr_t)b, len);
	if ((e = otx_oe_start(&x->x_oe, flags, b, len, x->x_proto, x->x_loc,
	    b + len, otx_oe_repsize(len))) < 0)
		return e;
	x->x_oebuf = *obp;
	*obp = 0;
	x->x_flags |= XF_OPT;
	x->x_oest = OES_SEND;
	return 0;
}

/*
 * One record from the write queue.  Returns 1 when it went back on the
 * queue (flow control, no memory), else 0.
 */
static int
wrec(q, x, mp)
	queue_t *q;
	struct otx *x;
	mblk_t *mp;
{
	struct otx_prim *p;
	struct otx_res res;
	struct otx *a;
	queue_t *aq;
	mblk_t *c, *data, *ob = 0;
	unsigned char *ctl, *o;
	long dlen;
	int type, fl, clen, n, i, sz;

	(void)otx_rhdr(mp->b_rptr, &type, &fl, &clen, &dlen);
	if (type == OTB_R_DATA) {
		if (!canput(q->q_next)) {
			(void)putbq(q, mp);
			return 1;
		}
		mp->b_rptr += OTB_RHDR;
		putnext(q, mp);
		return 0;
	}
	if (!pullupmsg(mp, OTB_RHDR + clen)) {
		(void)putbq(q, mp);
		retry(q, x, OTB_RHDR + clen);
		return 1;
	}
	ctl = mp->b_rptr + OTB_RHDR;
	p = otx_pfind(otx_down, (long)OB_G32(ctl), 0);
	if (p && p->p_data && !canput(q->q_next)) {
		(void)putbq(q, mp);
		return 1;
	}
	/* every allocation before the record is taken apart */
	if (p && p->p_opt && (ob = allocb(OEBUFSZ(clen), BPRI_MED)) == 0) {
		(void)putbq(q, mp);
		retry(q, x, OEBUFSZ(clen));
		return 1;
	}
	sz = clen + OT_INETADDR_LEN + 64;
	if ((c = allocb(sz, BPRI_MED)) == 0) {
		if (ob)
			freeb(ob);
		(void)putbq(q, mp);
		retry(q, x, sz);
		return 1;
	}
	if ((data = cut(mp, (long)OTB_RHDR + clen)) == (mblk_t *)-1) {
		if (ob)
			freeb(ob);
		freeb(c);
		(void)putbq(q, mp);
		retry(q, x, 64);
		return 1;
	}
	n = otx_xdown(ctl, clen, c->b_wptr, sz, &res);
	if (n < 0) {
		uperr(q, x, res.r_prim, (long)-n, -n == OT_TSYSERR ? (long)OT_EINVAL : 0L);
		goto drop;
	}
	o = c->b_wptr;
	c->b_datap->db_type = type == OTB_R_PCPROTO ? M_PCPROTO : M_PROTO;
	switch (res.r_how) {
	case OX_ADDR:
		otx_mkrhdr(c->b_wptr, OTB_R_PCPROTO, 0,
		    OT_ADDRACK_LEN + x->x_lalen + x->x_ralen, 0L);
		c->b_wptr += OTB_RHDR;
		c->b_wptr += otx_mkaddrack(c->b_wptr, x->x_la, x->x_lalen,
		    x->x_ra, x->x_ralen);
		c->b_datap->db_type = M_DATA;
		uprec(RD(q), x, c);
		c = 0;
		goto drop;
	case OX_OPTMGMT:
		if ((i = oestart(x, OB_S32(OB_G32(ctl + 12)), ctl + res.r_optoff,
		    res.r_optlen, &ob)) < 0) {
			uperr(q, x, res.r_prim, (long)-i, 0L);
			goto drop;
		}
		otx_orun(q, x);
		goto drop;
	case OX_CONNRES:
		/* a TCP acceptor: the host takes its queue on trust */
		if ((a = bycookie(x, res.r_cookie)) == 0 || a->x_proto != OTX_TCP ||
		    x->x_proto != OTX_TCP) {
			uperr(q, x, res.r_prim, (long)OT_TBADF, 0L);
			goto drop;
		}
		for (aq = WR(a->x_rq); aq->q_next; aq = aq->q_next)
			;
		OB_P32(o + 4, (long)RD(aq));
		for (i = 0; i < OTB_NIND; i++)
			if (x->x_ind[i].seq == res.r_seq) {
				bcopy((caddr_t)x->x_ind[i].a, (caddr_t)a->x_ra, OT_INETADDR_LEN);
				a->x_ralen = OT_INETADDR_LEN;
			}
		bcopy((caddr_t)x->x_la, (caddr_t)a->x_la, OT_INETADDR_LEN);
		a->x_lalen = x->x_lalen;
		break;
	case OX_PASS:
		if (res.r_prim == OT_CONN_REQ && res.r_addrlen) {
			bcopy((caddr_t)o + res.r_addroff, (caddr_t)x->x_rp, OT_INETADDR_LEN);
			x->x_rplen = OT_INETADDR_LEN;
		}
		break;
	}
	c->b_wptr += n;
	c->b_cont = data;
	data = 0;
	if (res.r_optlen) {
		/* options first, then the primitive without them */
		if ((i = oestart(x, (long)OT_NEGOTIATE, ctl + res.r_optoff,
		    res.r_optlen, &ob)) < 0) {
			uperr(q, x, res.r_prim, (long)-i, 0L);
			goto drop;
		}
		x->x_flags |= XF_OPTQUIET;
		x->x_pend = c;
		c = 0;
		otx_orun(q, x);
		goto drop;
	}
	putnext(q, c);
	c = 0;
drop:
	if (ob)
		freeb(ob);
	if (c)
		freemsg(c);
	if (data)
		freemsg(data);
	freemsg(mp);
	return 0;
}

/* run the option engine until it waits for the host or finishes */
static void
otx_orun(q, x)
	queue_t *q;
	struct otx *x;
{
	mblk_t *c;
	int n, sz = 16 + H_OPTHDR + OE_MAXVAL;

	if (!(x->x_flags & XF_OPT) || x->x_oest != OES_SEND)
		return;
	if ((c = allocb(sz, BPRI_MED)) == 0) {
		retry(q, x, sz);
		return;
	}
	if ((n = otx_oe_next(&x->x_oe, c->b_wptr, sz)) > 0) {
		c->b_wptr += n;
		c->b_datap->db_type = M_PROTO;
		x->x_oest = OES_WAIT;
		putnext(q, c);
		return;
	}
	freeb(c);
	if (x->x_flags & XF_OPTQUIET) {
		c = x->x_pend;
		x->x_pend = 0;
		oefree(x);
		if (c)
			putnext(q, c);
	} else {
		sz = OTB_RHDR + 16 + x->x_oe.e_replen;
		if ((c = allocb(sz, BPRI_MED)) == 0) {
			retry(q, x, sz);
			return;
		}
		otx_mkrhdr(c->b_wptr, OTB_R_PCPROTO, 0, 16 + x->x_oe.e_replen, 0L);
		c->b_wptr += OTB_RHDR;
		c->b_wptr += otx_oe_reply(&x->x_oe, c->b_wptr, sz - OTB_RHDR);
		oefree(x);
		uprec(RD(q), x, c);
	}
	qenable(q);
}

static int
otxwsrv(q)
	queue_t *q;
{
	struct otx *x = (struct otx *)q->q_ptr;
	mblk_t *mp;

	if (x == 0)
		return 0;
	x->x_wbufcall = 0;
	otx_orun(q, x);
	while (!(x->x_flags & XF_OPT) && (mp = getq(q)) != 0) {
		if (mp->b_datap->db_type != M_DATA) {
			putnext(q, mp);
			continue;
		}
		if (wrec(q, x, mp))
			break;
	}
	if ((x->x_flags & XF_WBLOCK) && q->q_count <= q->q_lowat) {
		x->x_flags &= ~XF_WBLOCK;
		otb_post(x, OTB_EV_WRITE);
	}
	return 0;
}

/* ---- up ---- */

/* translate one host message into a record; 1 if out of memory */
static int
otx_upmsg(q, x, mp)
	queue_t *q;
	struct otx *x;
	mblk_t *mp;
{
	struct otx_res res;
	unsigned char *ctl, *o;
	mblk_t *c;
	int len, n, i, type;

	if (mp->b_datap->db_type == M_DATA) {
		if ((c = allocb(OTB_RHDR, BPRI_MED)) == 0)
			return 1;
		otx_mkrhdr(c->b_wptr, OTB_R_DATA, 0, 0, (long)msgdsize(mp));
		c->b_wptr += OTB_RHDR;
		c->b_cont = mp;
		uprec(q, x, c);
		return 0;
	}
	type = mp->b_datap->db_type == M_PCPROTO ? OTB_R_PCPROTO : OTB_R_PROTO;
	len = mp->b_wptr - mp->b_rptr;
	ctl = mp->b_rptr;
	if (len > OTB_MAXREC - OTB_RHDR - OT_INETADDR_LEN - 16) {
		freemsg(mp);			/* its record would not fit */
		otb_post(x, OTB_EV_ERR);
		return 0;
	}
	if ((c = allocb(OTB_RHDR + len + OT_INETADDR_LEN + 16, BPRI_MED)) == 0)
		return 1;
	o = c->b_wptr + OTB_RHDR;
	n = otx_xup(ctl, len, o, len + OT_INETADDR_LEN + 16, &res);
	if (n < 0) {
		bcopy((caddr_t)ctl, (caddr_t)o, len);
		n = len;
		res.r_prim = 0;
	}
	switch (res.r_prim) {
	case OT_BIND_ACK:
		if (res.r_addrlen == OT_INETADDR_LEN) {
			bcopy((caddr_t)o + res.r_addroff, (caddr_t)x->x_la, OT_INETADDR_LEN);
			x->x_lalen = OT_INETADDR_LEN;
		}
		break;
	case OT_CONN_CON:
		if (res.r_addrlen == OT_INETADDR_LEN) {
			bcopy((caddr_t)o + res.r_addroff, (caddr_t)x->x_ra, OT_INETADDR_LEN);
			x->x_ralen = OT_INETADDR_LEN;
		} else if (x->x_rplen) {
			bcopy((caddr_t)x->x_rp, (caddr_t)x->x_ra, OT_INETADDR_LEN);
			x->x_ralen = OT_INETADDR_LEN;
		}
		break;
	case OT_CONN_IND:
		i = x->x_nind++ % OTB_NIND;
		x->x_ind[i].seq = res.r_seq;
		bzero((caddr_t)x->x_ind[i].a, OT_INETADDR_LEN);
		if (res.r_addrlen == OT_INETADDR_LEN)
			bcopy((caddr_t)o + res.r_addroff, (caddr_t)x->x_ind[i].a,
			    OT_INETADDR_LEN);
		break;
	case OT_DISCON_IND:
		x->x_ralen = 0;
		break;
	case OT_OK_ACK:
		if (res.r_eprim == OT_UNBIND_REQ)
			x->x_lalen = x->x_ralen = 0;
		break;
	}
	otx_mkrhdr(c->b_wptr, type, 0, n, (long)msgdsize(mp->b_cont));
	c->b_wptr = o + n;
	c->b_cont = mp->b_cont;
	mp->b_cont = 0;
	freeb(mp);
	uprec(q, x, c);
	return 0;
}

/* host messages that could not be translated for want of memory wait here */
static void
rawadd(x, mp)
	struct otx *x;
	mblk_t *mp;
{
	mblk_t **pp;

	for (pp = &x->x_rraw; *pp; pp = &RAWNEXT(*pp))
		;
	RAWNEXT(mp) = 0;
	*pp = mp;
}

static int
otxrput(q, mp)
	queue_t *q;
	mblk_t *mp;
{
	struct otx *x = (struct otx *)q->q_ptr;
	unsigned char *ctl;
	long hp;

	if (x == 0) {
		putnext(q, mp);
		return 0;
	}
	switch (mp->b_datap->db_type) {
	case M_PROTO:
	case M_PCPROTO:
		ctl = mp->b_rptr;
		if (mp->b_wptr - ctl >= 4 && (x->x_flags & XF_OPT) &&
		    x->x_oest == OES_WAIT) {
			hp = OB_G32(ctl);
			if (hp == H_OPTMGMT_ACK || (hp == H_ERROR_ACK &&
			    mp->b_wptr - ctl >= 8 && OB_G32(ctl + 4) == H_OPTMGMT_REQ)) {
				otx_oe_host(&x->x_oe, ctl, (int)(mp->b_wptr - ctl));
				x->x_oest = OES_SEND;
				freemsg(mp);
				qenable(WR(q));
				return 0;
			}
		}
		/* FALLTHROUGH */
	case M_DATA:
		if (x->x_rraw || otx_upmsg(q, x, mp)) {
			rawadd(x, mp);
			retry(q, x, OTB_RHDR + 64);
		}
		return 0;
	case M_FLUSH:
		if (*mp->b_rptr & FLUSHR)
			flushq(q, FLUSHDATA);
		break;
	case M_HANGUP:
		otb_post(x, OTB_EV_HUP);
		break;
	case M_ERROR:
		otb_post(x, OTB_EV_ERR);
		break;
	}
	putnext(q, mp);
	return 0;
}

static int
otxrsrv(q)
	queue_t *q;
{
	struct otx *x = (struct otx *)q->q_ptr;
	mblk_t *mp;

	if (x == 0)
		return 0;
	x->x_rbufcall = 0;
	while ((mp = getq(q)) != 0) {
		if (!canput(q->q_next)) {
			(void)putbq(q, mp);
			return 0;
		}
		putnext(q, mp);
	}
	while ((mp = x->x_rraw) != 0) {
		x->x_rraw = RAWNEXT(mp);
		RAWNEXT(mp) = 0;
		if (otx_upmsg(q, x, mp)) {
			RAWNEXT(mp) = x->x_rraw;
			x->x_rraw = mp;
			retry(q, x, OTB_RHDR + 64);
			break;
		}
	}
	return 0;
}
