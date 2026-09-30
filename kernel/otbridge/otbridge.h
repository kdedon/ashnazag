/*
 * otbridge.h -- translation tables (kernel and host build) and the
 * kernel's endpoint and session state.
 *
 * otxlate.c works on byte buffers through OB_G32/OB_P32, so the host
 * build (-DOTB_HOST) runs on any byte order and word size.
 */

#ifndef OTBRIDGE_H
#define OTBRIDGE_H

#include "otwire.h"

#define	OB_G16(p)	((unsigned long)((unsigned char *)(p))[0] << 8 | \
			 ((unsigned char *)(p))[1])
#define	OB_G32(p)	(OB_G16(p) << 16 | OB_G16((unsigned char *)(p) + 2))
#define	OB_P16(p, v)	(((unsigned char *)(p))[0] = ((v) >> 8) & 0xff, \
			 ((unsigned char *)(p))[1] = (v) & 0xff)
#define	OB_P32(p, v)	(OB_P16(p, (unsigned long)(v) >> 16), \
			 OB_P16((unsigned char *)(p) + 2, (v) & 0xffff))
/* low 32 bits as a signed long */
#define	OB_S32(x)	((x) & 0x80000000L ? -(long)((~(x) & 0x7fffffffL) + 1) \
			: (long)((x) & 0x7fffffffL))

/* host TPI (SVR4) */
#define	H_CONN_REQ	0
#define	H_CONN_RES	1
#define	H_DISCON_REQ	2
#define	H_DATA_REQ	3
#define	H_EXDATA_REQ	4
#define	H_INFO_REQ	5
#define	H_BIND_REQ	6
#define	H_UNBIND_REQ	7
#define	H_UNITDATA_REQ	8
#define	H_OPTMGMT_REQ	9
#define	H_ORDREL_REQ	10
#define	H_CONN_IND	11
#define	H_CONN_CON	12
#define	H_DISCON_IND	13
#define	H_DATA_IND	14
#define	H_EXDATA_IND	15
#define	H_INFO_ACK	16
#define	H_BIND_ACK	17
#define	H_ERROR_ACK	18
#define	H_OK_ACK	19
#define	H_UNITDATA_IND	20
#define	H_UDERROR_IND	21
#define	H_OPTMGMT_ACK	22
#define	H_ORDREL_IND	23

#define	H_NEGOTIATE	0x004
#define	H_CHECK		0x008
#define	H_DEFAULT	0x010
#define	H_SUCCESS	0x020
#define	H_FAILURE	0x040
#define	H_SOL_SOCKET	0xffff
#define	H_IPPROTO_IP	0
#define	H_IPPROTO_TCP	6
#define	H_OPTHDR	12		/* struct opthdr: level, name, len */

/* ---- primitive tables ---- */

/* what happens to a primitive */
#define	OX_PASS		1	/* renumber, check addresses */
#define	OX_CONNRES	2	/* acceptor by cookie */
#define	OX_ADDR		3	/* answered from recorded addresses */
#define	OX_OPTMGMT	4	/* option engine */
#define	OX_NOTSUP	5	/* T_ERROR_ACK TNOTSUPPORT */

struct otx_prim {
	short	p_ot, p_host;
	short	p_len;		/* minimum control part */
	short	p_how;		/* OX_* (down) */
	short	p_addr;		/* offset of an address length, or 0 */
	short	p_opt;		/* offset of an options length, or 0 */
	short	p_data;		/* 1: carries user data (flow controlled) */
};

extern struct otx_prim otx_down[], otx_up[];

/* result of otx_xdown / otx_xup */
struct otx_res {
	int		r_how;		/* OX_*, down only */
	long		r_prim;		/* OT primitive */
	long		r_hprim;	/* host primitive */
	int		r_data;		/* flow controlled */
	int		r_optoff, r_optlen;	/* options in the input, stripped */
	int		r_addroff, r_addrlen;	/* address in the output */
	unsigned long	r_cookie;	/* OX_CONNRES */
	long		r_seq;		/* T_CONN_IND, T_CONN_RES */
	long		r_eprim;	/* T_ERROR_ACK, T_OK_ACK: OT primitive */
};

/* ---- options ---- */

#define	OK_HOST		1	/* one host T_OPTMGMT_REQ */
#define	OK_LOCAL	2	/* kept per endpoint, not sent */
#define	OK_NOTSUP	3

#define	OC_INT		1	/* 4 bytes both sides */
#define	OC_KPALIVE	2	/* { onoff, minutes } -> int onoff */
#define	OC_LINGER	3	/* { onoff, seconds } both sides */
#define	OC_RAW		4	/* bytes as they are */

struct otx_opt {
	long	o_level, o_name;	/* OT */
	short	o_proto;		/* 0 any, OTX_TCP, OTX_UDP */
	short	o_kind;			/* OK_* */
	long	o_hlevel, o_hname;	/* host */
	short	o_len;			/* OT value length, 0 = 1..64 */
	short	o_conv;			/* OC_* */
	long	o_def;			/* OK_LOCAL default */
};

extern struct otx_opt otx_opts[];

#define	OTX_NLOC	8

struct otx_lopt {			/* an endpoint's local values */
	long	l_level, l_name, l_val;
};

#define	OE_MAXVAL	64

struct otx_oeng {
	unsigned char	*e_req;		/* OT options */
	int		e_reqlen, e_off;
	long		e_flags;	/* OT_NEGOTIATE ... */
	int		e_proto;
	unsigned char	*e_rep;		/* reply options */
	int		e_replen, e_repmax;
	int		e_worst;	/* rank of the worst status */
	struct otx_lopt	*e_loc;		/* OTX_NLOC entries */
	struct otx_opt	*e_cur;		/* waiting for the host */
	int		e_curoff;
};

/* otxlate.c */
struct otx_prim *otx_pfind();		/* (tab, prim, byhost) */
int	otx_rhdr();			/* (hdr, &type, &flags, &ctllen, &datalen) */
void	otx_mkrhdr();			/* (hdr, type, flags, ctllen, datalen) */
int	otx_xdown();			/* (ctl, len, out, max, res) */
int	otx_xup();			/* (ctl, len, out, max, res) */
int	otx_mkerr();			/* (out, otprim, tlierr, unixerr) */
int	otx_mkaddrack();		/* (out, la, lalen, ra, ralen) */
long	otx_errno();			/* (host errno) OT errno */
long	otx_tlierr();			/* (host TLI error) OT TLI error */
int	otx_oe_start();			/* (e, flags, opts, len, proto, loc, rep, repmax) */
int	otx_oe_next();			/* (e, hreq, max) */
void	otx_oe_host();			/* (e, ack, len) */
int	otx_oe_reply();			/* (e, out, max) */
int	otx_oe_repsize();		/* (reqlen) */

#if !defined(OTB_HOST) && defined(_SYS_STREAM_H)

/* ---- kernel state ---- */

#define	OTB_MAJOR	55
#define	OTB_NSESS	32
#define	OTB_STMINOR	64		/* stations: 64.. */
#define	OTB_NIND	8		/* pending T_CONN_INDs remembered */

struct otbs;

struct otx {				/* one endpoint: q_ptr of both queues */
	queue_t		*x_rq;
	struct otbs	*x_sess;
	struct otx	*x_snext;	/* the session's endpoints */
	struct otx	*x_next;	/* all endpoints */
	unsigned long	x_cookie;
	int		x_proto;
	int		x_ringidx;	/* this endpoint's ring entry, or -1 */
	int		x_flags;
	mblk_t		*x_part;	/* downstream bytes short of a record */
	mblk_t		*x_pend;	/* primitive waiting for its options */
	mblk_t		*x_rraw;	/* host messages not yet translated */
	int		x_nind;
	int		x_oest;		/* option engine step */
	int		x_rbufcall, x_wbufcall;	/* per queue */
	unsigned char	x_la[OT_INETADDR_LEN], x_ra[OT_INETADDR_LEN], x_rp[OT_INETADDR_LEN];
	int		x_lalen, x_ralen, x_rplen;
	struct {
		long		seq;
		unsigned char	a[OT_INETADDR_LEN];
	}		x_ind[OTB_NIND];
	struct otx_oeng	x_oe;
	mblk_t		*x_oebuf;	/* holds e_req and e_rep */
	struct otx_lopt	x_loc[OTX_NLOC];
};

#define	XF_WBLOCK	0x01		/* write queue filled up */
#define	XF_OPT		0x02		/* option engine running */
#define	XF_OPTQUIET	0x04		/* for x_pend: no reply */

struct otbs {				/* one session */
	int		s_inuse;
	int		s_id;
	struct proc	*s_proc;
	long		s_pid;
	uid_t		s_uid;
	struct otx	*s_eps;
	/* ring */
	int		s_ringon;
	struct as	*s_as;
	unsigned long	s_uva;
	unsigned long	s_pa[4];	/* per page from s_uva's page */
	int		s_npg;		/* locked: s_pa entries in use */
	int		s_sig;
	int		s_head;
};

extern struct otbs	otb_sess[];
extern struct otx	*otb_eps;
extern struct otb_stats	otb_st;
extern int		otb_incall;

char	*otb_alloc();
void	otb_free();
void	otb_post();			/* (x, events) */
struct otbs *otb_sfind();		/* (id) */
void	otb_sdetach();			/* (x) */
extern int	otb_nst;
int	otbst_open(), otbst_close(), otbst_read(), otbst_write(), otbst_ioctl(),
	otbst_poll();

/* IPL 4, the level splstr() stands for; never lowers */
#define	OTB_SPLSTR(s) do { \
	__asm__ __volatile__("movew %%sr,%0" : "=d" (s) : : "memory"); \
	if (((s) & 0x0700) < 0x0400) \
		__asm__ __volatile__("movew %0,%%sr" : : "d" (((s) & ~0x0700) | 0x0400) \
		    : "memory"); \
} while (0)
#define	OTB_SPLX(s)	__asm__ __volatile__("movew %0,%%sr" : : "d" (s) : "memory")

#endif	/* OTB_HOST */

#endif	/* OTBRIDGE_H */
