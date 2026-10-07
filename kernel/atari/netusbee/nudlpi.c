/*
 * STREAMS DLPI provider for the NetUSBee's Ethernet chip on the Atari
 * cartridge port.
 *
 * Same shape as the AMIX Amiga Ethernet driver, so the stock strcf
 * "addaen" and network-config run unchanged: DLPI style 1,
 * connectionless, minor = unit (bits 0-3) | stream slot + 1 (bits 4-7),
 * slot 0 meaning "the next free one"; ip and arp bind their Ethernet
 * types; a DL_UNITDATA_IND carries the destination and source
 * addresses at sizeof(union DL_primitives).  The Amiga driver's
 * status/config ioctls are answered with its structures, so
 * /usr/amiga/bin/aen -S works as a liveness test.
 *
 * Beyond it, for DLPI users such as an AppleTalk link layer:
 *   bind sap > 1500        Ethernet II, that type
 *   bind sap 2..0xfe, even IEEE 802.3 + 802.2 LLC UI frames to that SAP
 *   bind sap 0xAA, then    SNAP (802.2 AA AA 03 + OUI + type); up to
 *     DL_SUBS_BIND_REQ     NU_NSNAP 5-byte OUI/type subs per stream
 *   DL_ENABMULTI_REQ / DL_DISABMULTI_REQ / DL_PHYS_ADDR_REQ (DLPI 2.0
 *   numbering).  A multicast frame goes only to streams that enabled
 *   its address; broadcast goes to every matching stream.
 * Transmit addresses: 6 bytes, or 7 (LLC: + DSAP), or 11 (SNAP: +
 * OUI/type, otherwise the stream's first subs-bind).
 *
 * The card has no interrupt: a timeout polls it every tick while a
 * stream is open.  Everything that touches the chip or the stream table
 * runs at IPL 6, the clock's level.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/errno.h"
#include "sys/stream.h"
#include "sys/stropts.h"
#include "sys/cred.h"
#include "sys/dlpi.h"
#include "sys/socket.h"
#include "sys/sockio.h"
#include "net/if.h"
#include "amiga/driver/aen/aenuser.h"
#include "nu.h"

/* DLPI 2.0 primitives this dlpi.h predates */
#ifndef DL_ENABMULTI_REQ
#define DL_ENABMULTI_REQ	0x1d
#define DL_DISABMULTI_REQ	0x1e
#endif
#ifndef DL_PHYS_ADDR_REQ
#define DL_PHYS_ADDR_REQ	0x31
#define DL_PHYS_ADDR_ACK	0x32
#endif
#ifndef DL_TOOMANY
#define DL_TOOMANY		0x13
#define DL_NOTENAB		0x14
#endif

typedef struct {
	ulong	dl_primitive;
	ulong	dl_addr_length;
	ulong	dl_addr_offset;
} nu_multi_req_t;		/* DL_ENABMULTI_REQ, DL_DISABMULTI_REQ */

typedef struct {
	ulong	dl_primitive;
	ulong	dl_addr_length;
	ulong	dl_addr_offset;
} nu_phys_ack_t;

#define NU_NSTR		8	/* streams (slots) */
#define NU_NSNAP	2	/* SNAP subs-binds per stream */
#define NU_NSMC		4	/* multicast addresses per stream */
#define NU_IPL		0x600
#define ETHERMTU	1500
#define HDRSZ		14
#define LLCSZ		3
#define SNAPSZ		8

/* stream framing */
#define NUT_ETHER	1
#define NUT_LLC		2
#define NUT_SNAP	3

struct nu_str {
	queue_t		*q;		/* read queue */
	int		state;		/* DL_UNBOUND, DL_IDLE */
	int		type;		/* NUT_*, 0 when unbound */
	unsigned short	sap;
	int		flags;		/* AEN_RAW_DATA */
	int		nsnap;
	unsigned char	snap[NU_NSNAP][5];
	int		nmc;
	unsigned char	mc[NU_NSMC][6];
};

#define SR_GET(s)	__asm__ __volatile__("movew %%sr,%0" : "=d" (s) : : "memory")
#define SR_SET(s)	__asm__ __volatile__("movew %0,%%sr" : : "d" (s) : "memory")

extern struct ifstats *ifstats;
extern char *panicstr;
extern int timeout(), untimeout();
extern int bcmp(), printf();
extern void bcopy(), bzero();
extern int ata_busprobe();

int nuopen(), nuclose(), nuwput(), nuwsrv();
static void nu_tick(), nu_ioctl(), nu_proto();

static struct module_info nu_minfo = {
	0x6e75, "nu", 1, ETHERMTU, 5 * 1024, 3 * 1024,
};

static struct qinit nu_rinit = {
	NULL, NULL, nuopen, nuclose, NULL, &nu_minfo, NULL,
};

static struct qinit nu_winit = {
	nuwput, nuwsrv, NULL, NULL, NULL, &nu_minfo, NULL,
};

struct streamtab nuinfo = {
	&nu_rinit, &nu_winit, NULL, NULL,
};

struct nu_softc nu_sc;
struct nu_str nu_str[NU_NSTR];
struct ifstats nu_ifstats;
enum BOARD_STATE nu_bstate;
int nu_up;			/* 1 running, -1 no card */
int nu_found;			/* probed: 1 card, -1 none */
int nu_tid;			/* poll timeout id */
int nu_retry;			/* ticks since a failed re-init */
unsigned long nu_couldnt_put, nu_debug;

/* patchable */
unsigned char nu_eaddr[6] = { 0 };	/* nonzero: use instead of the PROM */
char *nu_ifname = "aen";		/* ifstats name, as netstat shows it */

static unsigned char nu_bcast[6] = { 255, 255, 255, 255, 255, 255 };

/* raise to IPL 6, never lower; returns the old SR */
static int
nu_spl()
{
	int s, n;

	SR_GET(s);
	if ((s & 0x700) < NU_IPL) {
		n = (s & ~0x700) | NU_IPL;
		SR_SET(n);
	}
	return s;
}

static void
nu_splx(s)
int s;
{
	SR_SET(s);
}

static int
nu_usable(a)
unsigned char *a;
{
	register int i, or, and;

	for (or = 0, and = 0xff, i = 0; i < 6; i++) {
		or |= a[i];
		and &= a[i];
	}
	return or != 0 && and != 0xff && (a[0] & 1) == 0;
}

/* ------------------------------------------------------- chip bring-up */

/*
 * Probe once: both windows must answer without a bus error before the
 * chip is looked at.
 */
static int
nu_find()
{
	if (nu_found)
		return nu_found > 0;
	nu_found = -1;
	if (!ata_busprobe(NU_RDBASE) || !ata_busprobe(NU_WRBASE) ||
	    nu_probe(&nu_sc) < 0)
		return 0;		/* optional hardware: the open's ENXIO says it */
	if (nu_usable(nu_eaddr))
		bcopy((caddr_t)nu_eaddr, (caddr_t)nu_sc.ea, 6);
	else if (nu_usable(nu_sc.prom))
		bcopy((caddr_t)nu_sc.prom, (caddr_t)nu_sc.ea, 6);
	else {
		printf("nu0: no usable Ethernet address (patch nu_eaddr)\n");
		return 0;
	}
	nu_found = 1;
	printf("nu0: %s on the cartridge port, %d KB, Ethernet %x:%x:%x:%x:%x:%x\n",
	    nu_sc.rtl ? "RTL8019" : "8390",
	    (nu_sc.memhi - nu_sc.memlo) >> 2, nu_sc.ea[0], nu_sc.ea[1],
	    nu_sc.ea[2], nu_sc.ea[3], nu_sc.ea[4], nu_sc.ea[5]);
	return 1;
}

static int
nu_start()
{
	int s;

	if (nu_up)
		return nu_up > 0;
	s = nu_spl();
	if (!nu_find()) {
		nu_up = -1;
		nu_splx(s);
		return 0;
	}
	nu_sc.rcr = RCR_AB;
	nu_bstate = BOARD_IN_INIT_CODE;
	if (nu_init(&nu_sc) < 0) {
		nu_stop(&nu_sc);
		nu_splx(s);
		printf("nu0: chip did not initialise\n");
		nu_bstate = BOARD_RESET;
		return 0;
	}
	nu_bstate = BOARD_RUNNING;
	nu_up = 1;
	nu_splx(s);
	nu_tid = timeout(nu_tick, (caddr_t)0, 1);
	return 1;
}

static void
nu_tick()
{
	int s;

	s = nu_spl();
	if (nu_up > 0 && !panicstr) {
		if (nu_sc.running)
			(void)nu_poll(&nu_sc);
		else if (++nu_retry >= 60) {
			nu_retry = 0;
			(void)nu_init(&nu_sc);
		}
	}
	nu_splx(s);
	if (nu_up > 0)
		nu_tid = timeout(nu_tick, (caddr_t)0, 1);
}

/* the module goes: the statistics leave the interface list */
int
nu_unload()
{
	register struct ifstats **p;
	int s;

	s = nu_spl();
	for (p = &ifstats; *p; p = &(*p)->ifs_next)
		if (*p == &nu_ifstats) {
			*p = nu_ifstats.ifs_next;
			break;
		}
	nu_splx(s);
	return 0;
}

/* ------------------------------------------------------------ open/close */

/* ARGSUSED */
int
nuopen(q, devp, flag, sflag, credp)
queue_t *q;
dev_t *devp;
int flag, sflag;
cred_t *credp;
{
	register struct nu_str *sp;
	register int slot, s;
	int minor;

	if (sflag == MODOPEN)
		return EINVAL;
	if (q->q_ptr)
		return 0;
	minor = getminor(*devp);
	if (sflag == CLONEOPEN)
		minor = 0;
	if ((minor & 0xf) != 0 || (minor & ~0xff) != 0)
		return ENXIO;
	if (!nu_start())
		return ENXIO;
	s = nu_spl();
	slot = (minor >> 4) & 0xf;
	if (slot == 0) {
		for (slot = 0; slot < NU_NSTR; slot++)
			if (nu_str[slot].q == 0)
				break;
		if (slot == NU_NSTR) {
			nu_splx(s);
			return ENOSPC;
		}
	} else if (--slot >= NU_NSTR) {
		nu_splx(s);
		return ENXIO;
	} else if (nu_str[slot].q) {
		nu_splx(s);
		return EBUSY;
	}
	sp = &nu_str[slot];
	bzero((caddr_t)sp, sizeof(*sp));
	sp->q = q;
	sp->state = DL_UNBOUND;
	q->q_ptr = (char *)sp;
	WR(q)->q_ptr = (char *)sp;
	*devp = makedevice(getmajor(*devp), (slot + 1) << 4);
	if (nu_ifstats.ifs_name == 0) {
		nu_ifstats.ifs_name = nu_ifname;
		nu_ifstats.ifs_unit = 0;
		nu_ifstats.ifs_mtu = ETHERMTU;
		nu_ifstats.ifs_next = ifstats;
		ifstats = &nu_ifstats;
	}
	nu_ifstats.ifs_active = 1;
	nu_splx(s);
	return 0;
}

int
nuclose(q)
queue_t *q;
{
	register struct nu_str *sp = (struct nu_str *)q->q_ptr;
	register int i, s;

	s = nu_spl();
	while (sp->nmc > 0)
		(void)nu_delmc(&nu_sc, sp->mc[--sp->nmc]);
	sp->q = 0;
	sp->state = DL_UNBOUND;
	q->q_ptr = 0;
	WR(q)->q_ptr = 0;
	for (i = 0; i < NU_NSTR; i++)
		if (nu_str[i].q)
			break;
	if (i == NU_NSTR && nu_up > 0) {
		nu_stop(&nu_sc);
		nu_bstate = BOARD_RESET;
		nu_up = 0;
		nu_ifstats.ifs_active = 0;
		untimeout(nu_tid);
	}
	nu_splx(s);
	return 0;
}

/* --------------------------------------------------------------- output */

/* The n bytes at off in mp's first block, or 0 if they lie outside it. */
static unsigned char *
nu_field(mp, off, n)
mblk_t *mp;
unsigned long off, n;
{
	unsigned long len = mp->b_wptr - mp->b_rptr;

	if (off > len || n > len - off)
		return 0;
	return mp->b_rptr + off;
}

/* Frame one DL_UNITDATA_REQ into the chip; 0 when both buffers are busy. */
static int
nu_xmit(sp, mp)
register struct nu_str *sp;
mblk_t *mp;
{
	register unsigned char *b, *a;
	register mblk_t *bp;
	dl_unitdata_req_t *dp = (dl_unitdata_req_t *)mp->b_rptr;
	unsigned long alen;
	int hl, n, room;

	if (nu_up <= 0 || !nu_sc.running)
		goto drop;
	if (!nu_txroom(&nu_sc))
		return 0;
	alen = dp->dl_dest_addr_length;
	a = nu_field(mp, dp->dl_dest_addr_offset, alen);
	if (a == 0 || alen < 6 || sp->state != DL_IDLE)
		goto drop;
	b = nu_sc.txbuf;
	bcopy((caddr_t)a, (caddr_t)b, 6);
	bcopy((caddr_t)nu_sc.ea, (caddr_t)b + 6, 6);
	hl = HDRSZ;
	if (sp->type == NUT_ETHER) {
		b[12] = sp->sap >> 8;
		b[13] = sp->sap;
	} else if (sp->type == NUT_LLC) {
		b[14] = alen >= 7 ? a[6] : sp->sap;
		b[15] = sp->sap;
		b[16] = 0x03;
		hl += LLCSZ;
	} else {
		b[14] = b[15] = 0xaa;
		b[16] = 0x03;
		if (alen >= 11)
			bcopy((caddr_t)a + 6, (caddr_t)b + 17, 5);
		else if (sp->nsnap > 0)
			bcopy((caddr_t)sp->snap[0], (caddr_t)b + 17, 5);
		else
			goto drop;
		hl += SNAPSZ;
	}
	room = NU_MAXFRAME - hl;
	for (n = 0, bp = mp->b_cont; bp; bp = bp->b_cont) {
		if (bp->b_datap->db_type != M_DATA || bp->b_wptr <= bp->b_rptr)
			continue;
		if (n + (bp->b_wptr - bp->b_rptr) > room)
			goto drop;
		bcopy((caddr_t)bp->b_rptr, (caddr_t)b + hl + n,
		    bp->b_wptr - bp->b_rptr);
		n += bp->b_wptr - bp->b_rptr;
	}
	if (sp->type != NUT_ETHER) {
		b[12] = (n + hl - HDRSZ) >> 8;	/* 802.3 length */
		b[13] = n + hl - HDRSZ;
	}
	nu_txstart(&nu_sc, hl + n);
	nu_ifstats.ifs_opackets++;
	freemsg(mp);
	return 1;
drop:
	nu_sc.st.oerrors++;
	nu_ifstats.ifs_oerrors++;
	freemsg(mp);
	return 1;
}

int
nuwput(q, mp)
queue_t *q;
mblk_t *mp;
{
	int s;

	switch (mp->b_datap->db_type) {
	case M_FLUSH:
		if (*mp->b_rptr & FLUSHW)
			flushq(q, FLUSHALL);
		if (*mp->b_rptr & FLUSHR) {
			flushq(RD(q), FLUSHALL);
			*mp->b_rptr &= ~FLUSHW;
			qreply(q, mp);
		} else
			freemsg(mp);
		break;
	case M_PROTO:
	case M_PCPROTO:
		s = nu_spl();
		nu_proto(q, mp);
		nu_splx(s);
		break;
	case M_IOCTL:
	case M_IOCDATA:
		nu_ioctl(q, mp);
		break;
	default:
		freemsg(mp);
		break;
	}
	return 0;
}

int
nuwsrv(q)
queue_t *q;
{
	register mblk_t *mp;
	int s;

	s = nu_spl();
	while ((mp = getq(q)) != 0) {
		if (!nu_xmit((struct nu_str *)q->q_ptr, mp)) {
			putbq(q, mp);
			break;
		}
	}
	nu_splx(s);
	return 0;
}

/* A transmit buffer is free: restart the streams that were waiting. */
/* ARGSUSED */
void
nu_txdone(sc)
struct nu_softc *sc;
{
	register int i;

	for (i = 0; i < NU_NSTR; i++)
		if (nu_str[i].q && WR(nu_str[i].q)->q_first)
			qenable(WR(nu_str[i].q));
}

/* ---------------------------------------------------------------- input */

static void
nu_deliver(sp, pkt, len, pay, plen)
register struct nu_str *sp;
unsigned char *pkt, *pay;
int len, plen;
{
	register mblk_t *mp;
	register dl_unitdata_ind_t *dp;
	int off = sizeof(union DL_primitives);

	if (!canput(sp->q->q_next)) {
		nu_couldnt_put++;
		return;
	}
	if (sp->flags & AEN_RAW_DATA) {
		/* the Amiga driver's raw form: status block, whole frame */
		if ((mp = allocb(sizeof(aen_status_t), BPRI_MED)) == 0)
			goto nomem;
		if ((mp->b_cont = allocb(len, BPRI_MED)) == 0) {
			freeb(mp);
			goto nomem;
		}
		mp->b_datap->db_type = M_PROTO;
		bzero((caddr_t)mp->b_wptr, sizeof(aen_status_t));
		mp->b_wptr += sizeof(aen_status_t);
		bcopy((caddr_t)pkt, (caddr_t)mp->b_cont->b_wptr, len);
		mp->b_cont->b_wptr += len;
		putnext(sp->q, mp);
		return;
	}
	if ((mp = allocb(off + 12, BPRI_MED)) == 0)
		goto nomem;
	if ((mp->b_cont = allocb(plen, BPRI_MED)) == 0) {
		freeb(mp);
		goto nomem;
	}
	mp->b_datap->db_type = M_PROTO;
	dp = (dl_unitdata_ind_t *)mp->b_wptr;
	dp->dl_primitive = DL_UNITDATA_IND;
	dp->dl_dest_addr_length = 6;
	dp->dl_dest_addr_offset = off;
	dp->dl_src_addr_length = 6;
	dp->dl_src_addr_offset = off + 6;
	dp->dl_reserved = 0;
	bcopy((caddr_t)pkt, (caddr_t)mp->b_wptr + off, 12);
	mp->b_wptr += off + 12;
	bcopy((caddr_t)pay, (caddr_t)mp->b_cont->b_wptr, plen);
	mp->b_cont->b_wptr += plen;
	putnext(sp->q, mp);
	return;
nomem:
	nu_sc.st.nomem++;
}

static int
nu_hasmc(sp, a)
register struct nu_str *sp;
unsigned char *a;
{
	register int i;

	for (i = 0; i < sp->nmc; i++)
		if (bcmp((caddr_t)sp->mc[i], (caddr_t)a, 6) == 0)
			return 1;
	return 0;
}

/*
 * One received frame (len without CRC), from the poll.  The hash filter
 * passes other groups too; only streams that enabled the address get one.
 */
void
nu_input(sc, pkt, len)
struct nu_softc *sc;
unsigned char *pkt;
int len;
{
	register struct nu_str *sp;
	register int i, k;
	unsigned char *pay, *llc;
	unsigned short tl;
	int plen, type, mcast, other;

	nu_ifstats.ifs_ipackets++;
	tl = pkt[12] << 8 | pkt[13];
	llc = pkt + HDRSZ;
	if (tl > ETHERMTU) {
		type = NUT_ETHER;
		pay = llc;
		plen = len - HDRSZ;
	} else {
		plen = tl < len - HDRSZ ? tl : len - HDRSZ;
		if (plen >= SNAPSZ && llc[0] == 0xaa && llc[1] == 0xaa &&
		    llc[2] == 0x03) {
			type = NUT_SNAP;
			pay = llc + SNAPSZ;
			plen -= SNAPSZ;
		} else if (plen >= LLCSZ) {
			type = NUT_LLC;
			pay = llc + LLCSZ;
			plen -= LLCSZ;
		} else
			return;
	}
	mcast = (pkt[0] & 1) && bcmp((caddr_t)pkt, (caddr_t)nu_bcast, 6) != 0;
	other = !(pkt[0] & 1) && bcmp((caddr_t)pkt, (caddr_t)sc->ea, 6) != 0;
	for (i = 0, sp = nu_str; i < NU_NSTR; i++, sp++) {
		if (sp->q == 0)
			continue;
		if (sp->flags & AEN_RAW_DATA) {
			nu_deliver(sp, pkt, len, pay, plen);
			continue;
		}
		if (sp->state != DL_IDLE || sp->type != type || other)
			continue;
		if (mcast && !nu_hasmc(sp, pkt))
			continue;
		if (type == NUT_ETHER && sp->sap != tl)
			continue;
		if (type == NUT_LLC && sp->sap != llc[0])
			continue;
		if (type == NUT_SNAP) {
			for (k = 0; k < sp->nsnap; k++)
				if (bcmp((caddr_t)sp->snap[k], (caddr_t)llc + 3, 5) == 0)
					break;
			if (k == sp->nsnap)
				continue;
		}
		nu_deliver(sp, pkt, len, pay, plen);
	}
}

/* ------------------------------------------------------------ primitives */

static void
nu_errack(q, mp, prim, err, uerr)
queue_t *q;
mblk_t *mp;
int prim, err, uerr;
{
	dl_error_ack_t *e;

	freemsg(mp);
	if ((mp = allocb(sizeof(dl_error_ack_t), BPRI_HI)) == 0)
		return;
	mp->b_datap->db_type = M_PCPROTO;
	e = (dl_error_ack_t *)mp->b_wptr;
	e->dl_primitive = DL_ERROR_ACK;
	e->dl_error_primitive = prim;
	e->dl_errno = err;
	e->dl_unix_errno = uerr;
	mp->b_wptr += sizeof(dl_error_ack_t);
	qreply(q, mp);
}

static void
nu_okack(q, mp, prim)
queue_t *q;
mblk_t *mp;
int prim;
{
	dl_ok_ack_t *k;

	freemsg(mp);
	if ((mp = allocb(sizeof(dl_ok_ack_t), BPRI_HI)) == 0)
		return;
	mp->b_datap->db_type = M_PCPROTO;
	k = (dl_ok_ack_t *)mp->b_wptr;
	k->dl_primitive = DL_OK_ACK;
	k->dl_correct_primitive = prim;
	mp->b_wptr += sizeof(dl_ok_ack_t);
	qreply(q, mp);
}

static void
nu_infoack(q, sp)
queue_t *q;
register struct nu_str *sp;
{
	register mblk_t *mp;
	register dl_info_ack_t *a;

	if ((mp = allocb(sizeof(dl_info_ack_t) + 6, BPRI_HI)) == 0)
		return;
	mp->b_datap->db_type = M_PCPROTO;
	a = (dl_info_ack_t *)mp->b_wptr;
	bzero((caddr_t)a, sizeof(dl_info_ack_t));
	a->dl_primitive = DL_INFO_ACK;
	a->dl_max_sdu = sp->type == NUT_SNAP ? ETHERMTU - SNAPSZ :
	    sp->type == NUT_LLC ? ETHERMTU - LLCSZ : ETHERMTU;
	a->dl_min_sdu = 1;
	a->dl_addr_length = 6;
	a->dl_mac_type = sp->type == NUT_ETHER || sp->type == 0 ?
	    DL_ETHER : DL_CSMACD;
	a->dl_current_state = sp->state;
	a->dl_max_idu = a->dl_max_sdu;
	a->dl_service_mode = DL_CLDLS;
	a->dl_provider_style = DL_STYLE1;
	mp->b_wptr += sizeof(dl_info_ack_t);
	if (sp->state == DL_IDLE) {
		a->dl_addr_offset = sizeof(dl_info_ack_t);
		bcopy((caddr_t)nu_sc.ea, (caddr_t)mp->b_wptr, 6);
		mp->b_wptr += 6;
	}
	qreply(q, mp);
}

static void
nu_bindack(q, sp)
queue_t *q;
struct nu_str *sp;
{
	register mblk_t *mp;
	register dl_bind_ack_t *a;

	if ((mp = allocb(sizeof(dl_bind_ack_t) + 6, BPRI_HI)) == 0)
		return;
	mp->b_datap->db_type = M_PCPROTO;
	a = (dl_bind_ack_t *)mp->b_wptr;
	a->dl_primitive = DL_BIND_ACK;
	a->dl_sap = sp->sap;
	a->dl_addr_length = 6;
	a->dl_addr_offset = sizeof(dl_bind_ack_t);
	a->dl_max_conind = 0;
	a->dl_growth = 0;
	mp->b_wptr += sizeof(dl_bind_ack_t);
	bcopy((caddr_t)nu_sc.ea, (caddr_t)mp->b_wptr, 6);
	mp->b_wptr += 6;
	qreply(q, mp);
}

static int
nu_addmulti(sp, a)
register struct nu_str *sp;
unsigned char *a;
{
	if (nu_hasmc(sp, a))
		return 0;
	if (sp->nmc == NU_NSMC || nu_addmc(&nu_sc, a) < 0)
		return DL_TOOMANY;
	bcopy((caddr_t)a, (caddr_t)sp->mc[sp->nmc++], 6);
	return 0;
}

static void
nu_dropmulti(sp, a)
register struct nu_str *sp;
unsigned char *a;
{
	register int i;

	for (i = 0; i < sp->nmc; i++)
		if (bcmp((caddr_t)sp->mc[i], (caddr_t)a, 6) == 0)
			break;
	if (i == sp->nmc)
		return;
	(void)nu_delmc(&nu_sc, sp->mc[i]);
	for (sp->nmc--; i < sp->nmc; i++)
		bcopy((caddr_t)sp->mc[i + 1], (caddr_t)sp->mc[i], 6);
}

static void
nu_proto(q, mp)
queue_t *q;
mblk_t *mp;
{
	register struct nu_str *sp = (struct nu_str *)q->q_ptr;
	union DL_primitives *p = (union DL_primitives *)mp->b_rptr;
	int len = mp->b_wptr - mp->b_rptr, prim, r;
	unsigned char *a;
	unsigned long sap;

	if (len < sizeof(ulong)) {
		freemsg(mp);
		return;
	}
	prim = p->dl_primitive;
	switch (prim) {
	case DL_UNITDATA_REQ:
		if (len < sizeof(dl_unitdata_req_t)) {
			freemsg(mp);
			return;
		}
		if (q->q_first || !nu_xmit(sp, mp))
			putq(q, mp);
		return;

	case DL_INFO_REQ:
		freemsg(mp);
		nu_infoack(q, sp);
		return;

	case DL_BIND_REQ:
		if (len < sizeof(dl_bind_req_t)) {
			nu_errack(q, mp, prim, DL_BADPRIM, 0);
			return;
		}
		if (sp->state != DL_UNBOUND) {
			nu_errack(q, mp, prim, DL_OUTSTATE, 0);
			return;
		}
		sap = p->bind_req.dl_sap;
		if (sap > ETHERMTU && sap <= 0xffff)
			sp->type = NUT_ETHER;
		else if (sap == 0xaa)
			sp->type = NUT_SNAP;
		else if (sap > 0 && sap <= 0xff && (sap & 1) == 0)
			sp->type = NUT_LLC;
		else {
			nu_errack(q, mp, prim, DL_BADSAP, 0);
			return;
		}
		sp->sap = sap;
		sp->nsnap = 0;
		sp->state = DL_IDLE;
		freemsg(mp);
		nu_bindack(q, sp);
		return;

	case DL_SUBS_BIND_REQ:
		if (len < sizeof(dl_subs_bind_req_t) || sp->type != NUT_SNAP ||
		    sp->state != DL_IDLE) {
			nu_errack(q, mp, prim, sp->state != DL_IDLE ?
			    DL_OUTSTATE : DL_BADSAP, 0);
			return;
		}
		a = nu_field(mp, p->subs_bind_req.dl_subs_sap_offset,
		    (unsigned long)5);
		if (p->subs_bind_req.dl_subs_sap_len != 5 || a == 0) {
			nu_errack(q, mp, prim, DL_BADADDR, 0);
			return;
		}
		if (sp->nsnap == NU_NSNAP) {
			nu_errack(q, mp, prim, DL_TOOMANY, 0);
			return;
		}
		bcopy((caddr_t)a, (caddr_t)sp->snap[sp->nsnap++], 5);
		/* the ack echoes the request: same layout, new primitive */
		p->dl_primitive = DL_SUBS_BIND_ACK;
		mp->b_datap->db_type = M_PCPROTO;
		qreply(q, mp);
		return;

	case DL_UNBIND_REQ:
		if (sp->state != DL_IDLE) {
			nu_errack(q, mp, prim, DL_OUTSTATE, 0);
			return;
		}
		flushq(q, FLUSHDATA);
		sp->state = DL_UNBOUND;
		sp->type = 0;
		sp->sap = 0;
		sp->nsnap = 0;
		nu_okack(q, mp, prim);
		return;

	case DL_ENABMULTI_REQ:
	case DL_DISABMULTI_REQ:
		a = len < sizeof(nu_multi_req_t) ? 0 : nu_field(mp,
		    ((nu_multi_req_t *)p)->dl_addr_offset, (unsigned long)6);
		if (a == 0 || ((nu_multi_req_t *)p)->dl_addr_length != 6 ||
		    (a[0] & 1) == 0) {
			nu_errack(q, mp, prim, DL_BADADDR, 0);
			return;
		}
		if (prim == DL_DISABMULTI_REQ) {
			if (!nu_hasmc(sp, a)) {
				nu_errack(q, mp, prim, DL_NOTENAB, 0);
				return;
			}
			nu_dropmulti(sp, a);
		} else if ((r = nu_addmulti(sp, a)) != 0) {
			nu_errack(q, mp, prim, r, 0);
			return;
		}
		nu_okack(q, mp, prim);
		return;

	case DL_PHYS_ADDR_REQ:
		freemsg(mp);
		if ((mp = allocb(sizeof(nu_phys_ack_t) + 6, BPRI_HI)) == 0)
			return;
		mp->b_datap->db_type = M_PCPROTO;
		((nu_phys_ack_t *)mp->b_wptr)->dl_primitive = DL_PHYS_ADDR_ACK;
		((nu_phys_ack_t *)mp->b_wptr)->dl_addr_length = 6;
		((nu_phys_ack_t *)mp->b_wptr)->dl_addr_offset = sizeof(nu_phys_ack_t);
		mp->b_wptr += sizeof(nu_phys_ack_t);
		bcopy((caddr_t)nu_sc.ea, (caddr_t)mp->b_wptr, 6);
		mp->b_wptr += 6;
		qreply(q, mp);
		return;

	case DL_ATTACH_REQ:
	case DL_DETACH_REQ:
		nu_errack(q, mp, prim, DL_NOTSUPPORTED, 0);
		return;

	default:
		nu_errack(q, mp, prim, DL_BADPRIM, 0);
		return;
	}
}

/* ---------------------------------------------------------------- ioctl */

static void
nu_iocack(q, mp, db, count, err)
queue_t *q;
mblk_t *mp;
int db, count, err;
{
	struct iocblk *ioc = (struct iocblk *)mp->b_rptr;

	mp->b_datap->db_type = db;
	ioc->ioc_count = count;
	ioc->ioc_rval = 0;
	ioc->ioc_error = err;
	qreply(q, mp);
}

static void
nu_getstatus(st)
register aen_status_t *st;
{
	register struct nu_stats *s = &nu_sc.st;

	bzero((caddr_t)st, sizeof(*st));
	st->board_state = nu_bstate;
	st->packets_sent = s->opackets;
	st->packets_received = s->ipackets;
	st->allocbs_failed = s->nomem;
	st->couldnt_put = nu_couldnt_put;
	st->miss_error = s->missed;
	st->collision_error = s->collisions;
	st->bad_start = s->rxbad;
	st->buffer_error = s->ierrors;
	st->overflow = s->ovw;
	st->framming = s->fae;
	st->crc = s->crc;
	st->board_debug = nu_debug;
	st->late_collisions = s->txowc;
	st->loss_of_carrier = s->txcarrier;
	st->retry_errors = s->txabt;
}

static void
nu_getconfig(c, sp)
register aen_config_t *c;
struct nu_str *sp;
{
	bzero((caddr_t)c, sizeof(*c));
	c->board_base = NU_RDBASE;
	c->board_debug = nu_debug;
	bcopy((caddr_t)nu_sc.ea, (caddr_t)c->paddress, 6);
	bcopy((caddr_t)nu_sc.mar, (caddr_t)c->laddress, 8);
	c->mode = (nu_sc.rcr & RCR_PRO) ? AEN_MODE_PROM : 0;
	c->flags = sp->flags;
}

/* AEN_SET_CONFIG: station address, promiscuous mode, raw flag. */
static int
nu_setconfig(c, sp)
register aen_config_t *c;
struct nu_str *sp;
{
	int s, r = 0;
	unsigned char rcr;

	nu_debug = c->board_debug;
	sp->flags = c->flags & AEN_RAW_DATA;
	s = nu_spl();
	rcr = (nu_sc.rcr & ~RCR_PRO) | ((c->mode & AEN_MODE_PROM) ? RCR_PRO : 0);
	if (bcmp((caddr_t)c->paddress, (caddr_t)nu_sc.ea, 6) != 0 ||
	    rcr != nu_sc.rcr) {
		if (!nu_usable(c->paddress))
			r = EINVAL;
		else {
			bcopy((caddr_t)c->paddress, (caddr_t)nu_sc.ea, 6);
			nu_sc.rcr = rcr;
			(void)nu_filter(&nu_sc);
		}
	}
	nu_splx(s);
	return r;
}

/*
 * The Amiga driver's ioctls, I_STR or transparent.  cq_private marks a
 * finished copy (0) or the SET_CONFIG copyin (1).
 */
static void
nu_ioctl(q, mp)
queue_t *q;
mblk_t *mp;
{
	struct iocblk *ioc = (struct iocblk *)mp->b_rptr;
	struct nu_str *sp = (struct nu_str *)q->q_ptr;
	struct copyreq *cq;
	mblk_t *bp;
	caddr_t uaddr;
	int size, cmd = ioc->ioc_cmd, transp, s;

	if (mp->b_datap->db_type == M_IOCDATA) {
		struct copyresp *cp = (struct copyresp *)mp->b_rptr;

		if (cp->cp_rval) {
			freemsg(mp);
			return;
		}
		if (cp->cp_private == 0 || cmd != AEN_SET_CONFIG) {
			freemsg(unlinkb(mp));
			nu_iocack(q, mp, M_IOCACK, 0, 0);
			return;
		}
		if (mp->b_cont == 0 || pullupmsg(mp->b_cont, -1) == 0 ||
		    mp->b_cont->b_wptr - mp->b_cont->b_rptr != sizeof(aen_config_t)) {
			freemsg(unlinkb(mp));
			nu_iocack(q, mp, M_IOCNAK, 0, EINVAL);
			return;
		}
		s = nu_setconfig((aen_config_t *)mp->b_cont->b_rptr, sp);
		freemsg(unlinkb(mp));
		nu_iocack(q, mp, s ? M_IOCNAK : M_IOCACK, 0, s);
		return;
	}

	transp = ioc->ioc_count == TRANSPARENT;
	switch (cmd) {
	case AEN_CLEAR_STATUS:
		s = nu_spl();
		bzero((caddr_t)&nu_sc.st, sizeof(nu_sc.st));
		nu_couldnt_put = 0;
		nu_splx(s);
		freemsg(unlinkb(mp));
		nu_iocack(q, mp, M_IOCACK, 0, 0);
		return;

	case AEN_GET_STATUS:
		size = sizeof(aen_status_t);
		break;
	case AEN_GET_CONFIG:
		size = sizeof(aen_config_t);
		break;
	case AEN_NUMBER_OF_BOARDS:
		size = sizeof(int);
		break;

	case AEN_SET_CONFIG:
		if (transp) {
			if (mp->b_cont == 0) {
				nu_iocack(q, mp, M_IOCNAK, 0, EINVAL);
				return;
			}
			cq = (struct copyreq *)mp->b_rptr;
			cq->cq_addr = *(caddr_t *)mp->b_cont->b_rptr;
			freemsg(unlinkb(mp));
			cq->cq_size = sizeof(aen_config_t);
			cq->cq_flag = 0;
			cq->cq_private = (mblk_t *)1;
			mp->b_datap->db_type = M_COPYIN;
			mp->b_wptr = mp->b_rptr + sizeof(*cq);
			qreply(q, mp);
			return;
		}
		if (mp->b_cont == 0 || pullupmsg(mp->b_cont, -1) == 0 ||
		    mp->b_cont->b_wptr - mp->b_cont->b_rptr != sizeof(aen_config_t)) {
			freemsg(unlinkb(mp));
			nu_iocack(q, mp, M_IOCNAK, 0, EINVAL);
			return;
		}
		s = nu_setconfig((aen_config_t *)mp->b_cont->b_rptr, sp);
		freemsg(unlinkb(mp));
		nu_iocack(q, mp, s ? M_IOCNAK : M_IOCACK, 0, s);
		return;

	case SIOCSIFFLAGS:
	case SIOCGIFFLAGS:
		nu_iocack(q, mp, M_IOCACK, ioc->ioc_count == TRANSPARENT ?
		    0 : ioc->ioc_count, 0);
		return;

	default:
		freemsg(unlinkb(mp));
		nu_iocack(q, mp, M_IOCNAK, 0, EINVAL);
		return;
	}

	/* the three "get" ioctls */
	uaddr = (transp && mp->b_cont) ? *(caddr_t *)mp->b_cont->b_rptr : 0;
	freemsg(unlinkb(mp));
	if ((bp = allocb(size, BPRI_MED)) == 0) {
		nu_iocack(q, mp, M_IOCNAK, 0, ENOMEM);
		return;
	}
	if (cmd == AEN_GET_STATUS) {
		s = nu_spl();
		nu_getstatus((aen_status_t *)bp->b_wptr);
		nu_splx(s);
	} else if (cmd == AEN_GET_CONFIG)
		nu_getconfig((aen_config_t *)bp->b_wptr, sp);
	else
		*(int *)bp->b_wptr = 1;
	bp->b_wptr += size;
	mp->b_cont = bp;
	if (transp) {
		cq = (struct copyreq *)mp->b_rptr;
		cq->cq_addr = uaddr;
		cq->cq_size = size;
		cq->cq_flag = 0;
		cq->cq_private = 0;
		mp->b_datap->db_type = M_COPYOUT;
		mp->b_wptr = mp->b_rptr + sizeof(*cq);
		qreply(q, mp);
		return;
	}
	nu_iocack(q, mp, M_IOCACK, size, 0);
}
