/*
 * STREAMS DLPI provider for a DaynaPORT SCSI/Link (or an SD-card SCSI
 * emulator acting as one) on the Falcon SCSI port.
 *
 * Same shape as the AMIX Amiga Ethernet driver, so the stock strcf
 * "addaen" and network-config run unchanged: DLPI style 1,
 * connectionless, minor = unit (bits 0-3) | stream slot + 1 (bits 4-7),
 * slot 0 meaning "the next free one"; ip and arp bind their Ethernet
 * types; a DL_UNITDATA_IND carries the destination and source
 * addresses at sizeof(union DL_primitives).  The Amiga driver's
 * status/config ioctls are answered with its structures.
 *
 * Beyond it, for DLPI users such as an AppleTalk link layer:
 *   bind sap > 1500        Ethernet II, that type
 *   bind sap 2..0xfe, even IEEE 802.3 + 802.2 LLC UI frames to that SAP
 *   bind sap 0xAA, then    SNAP (802.2 AA AA 03 + OUI + type); up to
 *     DL_SUBS_BIND_REQ     DP_NSNAP 5-byte OUI/type subs per stream
 *   DL_ENABMULTI_REQ / DL_DISABMULTI_REQ / DL_PHYS_ADDR_REQ (DLPI 2.0
 *   numbering).  A multicast frame goes only to streams that enabled
 *   its address; broadcast goes to every matching stream.
 * Transmit addresses: 6 bytes, or 7 (LLC: + DSAP), or 11 (SNAP: +
 * OUI/type, otherwise the stream's first subs-bind).
 *
 * The adapter has no interrupt and a SCSI command takes up to ~15 ms of
 * polled PIO, too long to hold off the clock.  So the bus is driven only
 * from the write service procedure, which the STREAMS scheduler runs at
 * IPL 1: frames are queued by put, and a timeout qenables the service to
 * poll for input.  Stream state is changed at IPL 6, briefly.
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
#include "dp.h"

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
} dp_multi_req_t;		/* DL_ENABMULTI_REQ, DL_DISABMULTI_REQ */

typedef struct {
	ulong	dl_primitive;
	ulong	dl_addr_length;
	ulong	dl_addr_offset;
} dp_phys_ack_t;

#define DP_NSTR		8	/* streams (slots) */
#define DP_NSNAP	2	/* SNAP subs-binds per stream */
#define DP_NSMC		4	/* multicast addresses per stream */
#define DP_IPL		0x600
#define DP_BURST	8	/* commands per service run */
#define DP_IDLE		100	/* empty polls before polling slower */
#define DP_SLOW		3	/* ticks between polls when idle */
#define ETHERMTU	1500
#define HDRSZ		14
#define LLCSZ		3
#define SNAPSZ		8

/* stream framing */
#define DPT_ETHER	1
#define DPT_LLC		2
#define DPT_SNAP	3

struct dp_str {
	queue_t		*q;		/* read queue */
	int		state;		/* DL_UNBOUND, DL_IDLE */
	int		type;		/* DPT_*, 0 when unbound */
	unsigned short	sap;
	int		flags;		/* AEN_RAW_DATA */
	int		nsnap;
	unsigned char	snap[DP_NSNAP][5];
	int		nmc;
	unsigned char	mc[DP_NSMC][6];
};

#define SR_GET(s)	__asm__ __volatile__("movew %%sr,%0" : "=d" (s) : : "memory")
#define SR_SET(s)	__asm__ __volatile__("movew %0,%%sr" : : "d" (s) : "memory")

extern struct ifstats *ifstats;
extern char *panicstr;
extern int timeout(), untimeout();
extern int bcmp(), printf();
extern void bcopy(), bzero();

int dpnopen(), dpnclose(), dpnwput(), dpnwsrv();
static void dp_tick(), dp_ioctl(), dp_proto();

static struct module_info dp_minfo = {
	0x6470, "dpn", 1, ETHERMTU, 5 * 1024, 3 * 1024,
};

static struct qinit dp_rinit = {
	NULL, NULL, dpnopen, dpnclose, NULL, &dp_minfo, NULL,
};

static struct qinit dp_winit = {
	dpnwput, dpnwsrv, NULL, NULL, NULL, &dp_minfo, NULL,
};

struct streamtab dpninfo = {
	&dp_rinit, &dp_winit, NULL, NULL,
};

struct dp_softc dp_sc;
struct dp_str dp_str[DP_NSTR];
struct ifstats dp_ifstats;
enum BOARD_STATE dp_bstate;
int dp_up;			/* 1 running, 2 starting, -1 no adapter */
int dp_found;			/* probed: 1 adapter, -1 none */
int dp_tid;			/* poll timeout id */
int dp_busy;			/* the bus is in use */
int dp_rxdue;			/* a receive poll is due */
int dp_idle;			/* empty polls in a row */
int dp_wait;			/* ticks until the next idle poll */
int dp_rr;			/* next stream to transmit from */
unsigned long dp_couldnt_put, dp_debug;

/* patchable */
char *dp_ifname = "dpn";		/* ifstats name, as netstat shows it */

static unsigned char dp_bcast[6] = { 255, 255, 255, 255, 255, 255 };

/* raise to IPL 6, never lower; returns the old SR */
static int
dp_spl()
{
	int s, n;

	SR_GET(s);
	if ((s & 0x700) < DP_IPL) {
		n = (s & ~0x700) | DP_IPL;
		SR_SET(n);
	}
	return s;
}

static void
dp_splx(s)
int s;
{
	SR_SET(s);
}

/* ------------------------------------------------------ adapter bring-up */

/* Probe once, in process context. */
static int
dp_find()
{
	if (dp_found)
		return dp_found > 0;
	dp_found = -1;
	if (dp_probe(&dp_sc) < 0)
		return 0;		/* optional hardware: the open's ENXIO says it */
	dp_found = 1;
	return 1;
}

/* First open: probe, enable, read the address.  Sleeps. */
static int
dp_start()
{
	int s;

	for (;;) {
		s = dp_spl();
		if (dp_up != 2 && !dp_busy)
			break;
		dp_splx(s);
		delay(1);
	}
	if (dp_up) {
		dp_splx(s);
		return dp_up > 0;
	}
	dp_up = 2;
	dp_busy = 1;
	dp_bstate = BOARD_IN_INIT_CODE;
	dp_splx(s);
	if (!dp_find()) {
		dp_up = -1;
		dp_bstate = BOARD_RESET;
	} else if (dp_init(&dp_sc) < 0) {
		printf("dpn0: DaynaPORT at SCSI ID %d did not come up\n", dp_sc.id);
		dp_up = 0;
		dp_bstate = BOARD_RESET;
	} else {
		printf("dpn0: DaynaPORT at SCSI ID %d, firmware %c%c%c%c, Ethernet %x:%x:%x:%x:%x:%x\n",
		    dp_sc.id, dp_sc.rev[0], dp_sc.rev[1], dp_sc.rev[2], dp_sc.rev[3],
		    dp_sc.ea[0], dp_sc.ea[1], dp_sc.ea[2], dp_sc.ea[3], dp_sc.ea[4],
		    dp_sc.ea[5]);
		dp_bstate = BOARD_RUNNING;
		dp_idle = 0;
		dp_rxdue = 1;
		dp_up = 1;
		dp_tid = timeout(dp_tick, (caddr_t)0, 1);
	}
	dp_busy = 0;
	return dp_up > 0;
}

/* Each tick (or every DP_SLOW when idle): ask the service for a poll. */
static void
dp_tick()
{
	int i, s;

	if (dp_up != 1)
		return;
	s = dp_spl();
	if (dp_idle < DP_IDLE || --dp_wait <= 0) {
		dp_wait = DP_SLOW;
		dp_rxdue = 1;
		for (i = 0; i < DP_NSTR; i++)
			if (dp_str[i].q) {
				qenable(WR(dp_str[i].q));
				break;
			}
	}
	dp_splx(s);
	dp_tid = timeout(dp_tick, (caddr_t)0, 1);
}

/* ------------------------------------------------------------ open/close */

/* ARGSUSED */
int
dpnopen(q, devp, flag, sflag, credp)
queue_t *q;
dev_t *devp;
int flag, sflag;
cred_t *credp;
{
	register struct dp_str *sp;
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
	if (!dp_start())
		return ENXIO;
	s = dp_spl();
	slot = (minor >> 4) & 0xf;
	if (slot == 0) {
		for (slot = 0; slot < DP_NSTR; slot++)
			if (dp_str[slot].q == 0)
				break;
		if (slot == DP_NSTR) {
			dp_splx(s);
			return ENOSPC;
		}
	} else if (--slot >= DP_NSTR) {
		dp_splx(s);
		return ENXIO;
	} else if (dp_str[slot].q) {
		dp_splx(s);
		return EBUSY;
	}
	sp = &dp_str[slot];
	bzero((caddr_t)sp, sizeof(*sp));
	sp->q = q;
	sp->state = DL_UNBOUND;
	q->q_ptr = (char *)sp;
	WR(q)->q_ptr = (char *)sp;
	*devp = makedevice(getmajor(*devp), (slot + 1) << 4);
	if (dp_ifstats.ifs_name == 0) {
		dp_ifstats.ifs_name = dp_ifname;
		dp_ifstats.ifs_unit = 0;
		dp_ifstats.ifs_mtu = ETHERMTU;
		dp_ifstats.ifs_next = ifstats;
		ifstats = &dp_ifstats;
	}
	dp_ifstats.ifs_active = 1;
	dp_splx(s);
	return 0;
}

int
dpnclose(q)
queue_t *q;
{
	register struct dp_str *sp = (struct dp_str *)q->q_ptr;
	register int i, s;

	s = dp_spl();
	sp->nmc = 0;
	sp->q = 0;
	sp->state = DL_UNBOUND;
	q->q_ptr = 0;
	WR(q)->q_ptr = 0;
	for (i = 0; i < DP_NSTR; i++)
		if (dp_str[i].q)
			break;
	if (i == DP_NSTR && dp_up > 0) {
		untimeout(dp_tid);
		dp_up = 0;
		dp_bstate = BOARD_RESET;
		dp_ifstats.ifs_active = 0;
		dp_busy = 1;
		dp_splx(s);
		dp_stop(&dp_sc);
		dp_sc.nmc = 0;
		dp_busy = 0;
		return 0;
	}
	dp_splx(s);
	return 0;
}

/* the module goes: the statistics leave the interface list */
int
dp_unload()
{
	register struct ifstats **p;
	int s;

	s = dp_spl();
	for (p = &ifstats; *p; p = &(*p)->ifs_next)
		if (*p == &dp_ifstats) {
			*p = dp_ifstats.ifs_next;
			break;
		}
	dp_splx(s);
	return 0;
}

/* --------------------------------------------------------------- output */

/* The n bytes at off in mp's first block, or 0 if they lie outside it. */
static unsigned char *
dp_field(mp, off, n)
mblk_t *mp;
unsigned long off, n;
{
	unsigned long len = mp->b_wptr - mp->b_rptr;

	if (off > len || n > len - off)
		return 0;
	return mp->b_rptr + off;
}

/* Frame one DL_UNITDATA_REQ and send it. */
static void
dp_xmit(sp, mp)
register struct dp_str *sp;
mblk_t *mp;
{
	register unsigned char *b, *a;
	register mblk_t *bp;
	dl_unitdata_req_t *dp = (dl_unitdata_req_t *)mp->b_rptr;
	unsigned long alen;
	int hl, n, room;

	alen = dp->dl_dest_addr_length;
	a = dp_field(mp, dp->dl_dest_addr_offset, alen);
	if (a == 0 || alen < 6 || sp->state != DL_IDLE)
		goto drop;
	b = dp_sc.txbuf;
	bcopy((caddr_t)a, (caddr_t)b, 6);
	bcopy((caddr_t)dp_sc.ea, (caddr_t)b + 6, 6);
	hl = HDRSZ;
	if (sp->type == DPT_ETHER) {
		b[12] = sp->sap >> 8;
		b[13] = sp->sap;
	} else if (sp->type == DPT_LLC) {
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
	room = DP_MAXFRAME - hl;
	for (n = 0, bp = mp->b_cont; bp; bp = bp->b_cont) {
		if (bp->b_datap->db_type != M_DATA || bp->b_wptr <= bp->b_rptr)
			continue;
		if (n + (bp->b_wptr - bp->b_rptr) > room)
			goto drop;
		bcopy((caddr_t)bp->b_rptr, (caddr_t)b + hl + n,
		    bp->b_wptr - bp->b_rptr);
		n += bp->b_wptr - bp->b_rptr;
	}
	if (sp->type != DPT_ETHER) {
		b[12] = (n + hl - HDRSZ) >> 8;	/* 802.3 length */
		b[13] = n + hl - HDRSZ;
	}
	freemsg(mp);
	if (dp_send(&dp_sc, hl + n) < 0)
		dp_ifstats.ifs_oerrors++;
	else
		dp_ifstats.ifs_opackets++;
	return;
drop:
	dp_sc.st.oerrors++;
	dp_ifstats.ifs_oerrors++;
	freemsg(mp);
}

/*
 * The only code that drives the bus once the link is up: transmit what
 * the streams queued, then poll for input while frames keep coming.
 * Called from the write service at IPL 1.
 */
static void
dp_run()
{
	register struct dp_str *sp;
	mblk_t *mp;
	int i, n, r, s;

	s = dp_spl();
	if (dp_busy || dp_up != 1 || (s & 0x700) >= DP_IPL) {
		dp_splx(s);
		return;
	}
	dp_busy = 1;
	dp_splx(s);
	for (n = 0; n < DP_BURST && dp_ready(&dp_sc); n++) {
		for (mp = 0, sp = 0, i = 0; i < DP_NSTR && mp == 0; i++) {
			sp = &dp_str[dp_rr];
			if (++dp_rr == DP_NSTR)
				dp_rr = 0;
			if (sp->q)
				mp = getq(WR(sp->q));
		}
		if (mp) {
			dp_xmit(sp, mp);
			dp_rxdue = 1;	/* the answer is probably on its way */
			continue;
		}
		if (!dp_rxdue)
			break;
		r = dp_recv(&dp_sc);
		if (r > 0)
			dp_idle = 0;
		else {
			dp_rxdue = 0;
			if (r == 0 && dp_idle < DP_IDLE)
				dp_idle++;
		}
	}
	dp_busy = 0;
}

int
dpnwput(q, mp)
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
		s = dp_spl();
		dp_proto(q, mp);
		dp_splx(s);
		break;
	case M_IOCTL:
	case M_IOCDATA:
		dp_ioctl(q, mp);
		break;
	default:
		freemsg(mp);
		break;
	}
	return 0;
}

/* ARGSUSED */
int
dpnwsrv(q)
queue_t *q;
{
	dp_run();
	return 0;
}

/* ---------------------------------------------------------------- input */

static void
dp_deliver(sp, pkt, len, pay, plen)
register struct dp_str *sp;
unsigned char *pkt, *pay;
int len, plen;
{
	register mblk_t *mp;
	register dl_unitdata_ind_t *dp;
	int off = sizeof(union DL_primitives);

	if (!canput(sp->q->q_next)) {
		dp_couldnt_put++;
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
	dp_sc.st.nomem++;
}

static int
dp_hasmc(sp, a)
register struct dp_str *sp;
unsigned char *a;
{
	register int i;

	for (i = 0; i < sp->nmc; i++)
		if (bcmp((caddr_t)sp->mc[i], (caddr_t)a, 6) == 0)
			return 1;
	return 0;
}

/*
 * One received frame (len without CRC), from the poll.  The adapter may
 * pass other groups too; only streams that enabled the address get one.
 */
void
dp_input(sc, pkt, len)
struct dp_softc *sc;
unsigned char *pkt;
int len;
{
	register struct dp_str *sp;
	register int i, k;
	unsigned char *pay, *llc;
	unsigned short tl;
	int plen, type, mcast, other;

	dp_ifstats.ifs_ipackets++;
	tl = pkt[12] << 8 | pkt[13];
	llc = pkt + HDRSZ;
	if (tl > ETHERMTU) {
		type = DPT_ETHER;
		pay = llc;
		plen = len - HDRSZ;
	} else {
		plen = tl < len - HDRSZ ? tl : len - HDRSZ;
		if (plen >= SNAPSZ && llc[0] == 0xaa && llc[1] == 0xaa &&
		    llc[2] == 0x03) {
			type = DPT_SNAP;
			pay = llc + SNAPSZ;
			plen -= SNAPSZ;
		} else if (plen >= LLCSZ) {
			type = DPT_LLC;
			pay = llc + LLCSZ;
			plen -= LLCSZ;
		} else
			return;
	}
	mcast = (pkt[0] & 1) && bcmp((caddr_t)pkt, (caddr_t)dp_bcast, 6) != 0;
	other = !(pkt[0] & 1) && bcmp((caddr_t)pkt, (caddr_t)sc->ea, 6) != 0;
	for (i = 0, sp = dp_str; i < DP_NSTR; i++, sp++) {
		if (sp->q == 0)
			continue;
		if (sp->flags & AEN_RAW_DATA) {
			dp_deliver(sp, pkt, len, pay, plen);
			continue;
		}
		if (sp->state != DL_IDLE || sp->type != type || other)
			continue;
		if (mcast && !dp_hasmc(sp, pkt))
			continue;
		if (type == DPT_ETHER && sp->sap != tl)
			continue;
		if (type == DPT_LLC && sp->sap != llc[0])
			continue;
		if (type == DPT_SNAP) {
			for (k = 0; k < sp->nsnap; k++)
				if (bcmp((caddr_t)sp->snap[k], (caddr_t)llc + 3, 5) == 0)
					break;
			if (k == sp->nsnap)
				continue;
		}
		dp_deliver(sp, pkt, len, pay, plen);
	}
}

/* ------------------------------------------------------------ primitives */

static void
dp_errack(q, mp, prim, err, uerr)
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
dp_okack(q, mp, prim)
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
dp_infoack(q, sp)
queue_t *q;
register struct dp_str *sp;
{
	register mblk_t *mp;
	register dl_info_ack_t *a;

	if ((mp = allocb(sizeof(dl_info_ack_t) + 6, BPRI_HI)) == 0)
		return;
	mp->b_datap->db_type = M_PCPROTO;
	a = (dl_info_ack_t *)mp->b_wptr;
	bzero((caddr_t)a, sizeof(dl_info_ack_t));
	a->dl_primitive = DL_INFO_ACK;
	a->dl_max_sdu = sp->type == DPT_SNAP ? ETHERMTU - SNAPSZ :
	    sp->type == DPT_LLC ? ETHERMTU - LLCSZ : ETHERMTU;
	a->dl_min_sdu = 1;
	a->dl_addr_length = 6;
	a->dl_mac_type = sp->type == DPT_ETHER || sp->type == 0 ?
	    DL_ETHER : DL_CSMACD;
	a->dl_current_state = sp->state;
	a->dl_max_idu = a->dl_max_sdu;
	a->dl_service_mode = DL_CLDLS;
	a->dl_provider_style = DL_STYLE1;
	mp->b_wptr += sizeof(dl_info_ack_t);
	if (sp->state == DL_IDLE) {
		a->dl_addr_offset = sizeof(dl_info_ack_t);
		bcopy((caddr_t)dp_sc.ea, (caddr_t)mp->b_wptr, 6);
		mp->b_wptr += 6;
	}
	qreply(q, mp);
}

static void
dp_bindack(q, sp)
queue_t *q;
struct dp_str *sp;
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
	bcopy((caddr_t)dp_sc.ea, (caddr_t)mp->b_wptr, 6);
	mp->b_wptr += 6;
	qreply(q, mp);
}

static int
dp_addmulti(sp, a)
register struct dp_str *sp;
unsigned char *a;
{
	if (dp_hasmc(sp, a))
		return 0;
	if (sp->nmc == DP_NSMC || dp_addmc(&dp_sc, a) < 0)
		return DL_TOOMANY;
	bcopy((caddr_t)a, (caddr_t)sp->mc[sp->nmc++], 6);
	return 0;
}

static void
dp_dropmulti(sp, a)
register struct dp_str *sp;
unsigned char *a;
{
	register int i;

	for (i = 0; i < sp->nmc; i++)
		if (bcmp((caddr_t)sp->mc[i], (caddr_t)a, 6) == 0)
			break;
	if (i == sp->nmc)
		return;
	for (sp->nmc--; i < sp->nmc; i++)
		bcopy((caddr_t)sp->mc[i + 1], (caddr_t)sp->mc[i], 6);
}

static void
dp_proto(q, mp)
queue_t *q;
mblk_t *mp;
{
	register struct dp_str *sp = (struct dp_str *)q->q_ptr;
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
		putq(q, mp);
		return;

	case DL_INFO_REQ:
		freemsg(mp);
		dp_infoack(q, sp);
		return;

	case DL_BIND_REQ:
		if (len < sizeof(dl_bind_req_t)) {
			dp_errack(q, mp, prim, DL_BADPRIM, 0);
			return;
		}
		if (sp->state != DL_UNBOUND) {
			dp_errack(q, mp, prim, DL_OUTSTATE, 0);
			return;
		}
		sap = p->bind_req.dl_sap;
		if (sap > ETHERMTU && sap <= 0xffff)
			sp->type = DPT_ETHER;
		else if (sap == 0xaa)
			sp->type = DPT_SNAP;
		else if (sap > 0 && sap <= 0xff && (sap & 1) == 0)
			sp->type = DPT_LLC;
		else {
			dp_errack(q, mp, prim, DL_BADSAP, 0);
			return;
		}
		sp->sap = sap;
		sp->nsnap = 0;
		sp->state = DL_IDLE;
		freemsg(mp);
		dp_bindack(q, sp);
		return;

	case DL_SUBS_BIND_REQ:
		if (len < sizeof(dl_subs_bind_req_t) || sp->type != DPT_SNAP ||
		    sp->state != DL_IDLE) {
			dp_errack(q, mp, prim, sp->state != DL_IDLE ?
			    DL_OUTSTATE : DL_BADSAP, 0);
			return;
		}
		a = dp_field(mp, p->subs_bind_req.dl_subs_sap_offset,
		    (unsigned long)5);
		if (p->subs_bind_req.dl_subs_sap_len != 5 || a == 0) {
			dp_errack(q, mp, prim, DL_BADADDR, 0);
			return;
		}
		if (sp->nsnap == DP_NSNAP) {
			dp_errack(q, mp, prim, DL_TOOMANY, 0);
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
			dp_errack(q, mp, prim, DL_OUTSTATE, 0);
			return;
		}
		flushq(q, FLUSHDATA);
		sp->state = DL_UNBOUND;
		sp->type = 0;
		sp->sap = 0;
		sp->nsnap = 0;
		dp_okack(q, mp, prim);
		return;

	case DL_ENABMULTI_REQ:
	case DL_DISABMULTI_REQ:
		a = len < sizeof(dp_multi_req_t) ? 0 : dp_field(mp,
		    ((dp_multi_req_t *)p)->dl_addr_offset, (unsigned long)6);
		if (a == 0 || ((dp_multi_req_t *)p)->dl_addr_length != 6 ||
		    (a[0] & 1) == 0) {
			dp_errack(q, mp, prim, DL_BADADDR, 0);
			return;
		}
		if (prim == DL_DISABMULTI_REQ) {
			if (!dp_hasmc(sp, a)) {
				dp_errack(q, mp, prim, DL_NOTENAB, 0);
				return;
			}
			dp_dropmulti(sp, a);
		} else if ((r = dp_addmulti(sp, a)) != 0) {
			dp_errack(q, mp, prim, r, 0);
			return;
		}
		dp_okack(q, mp, prim);
		return;

	case DL_PHYS_ADDR_REQ:
		freemsg(mp);
		if ((mp = allocb(sizeof(dp_phys_ack_t) + 6, BPRI_HI)) == 0)
			return;
		mp->b_datap->db_type = M_PCPROTO;
		((dp_phys_ack_t *)mp->b_wptr)->dl_primitive = DL_PHYS_ADDR_ACK;
		((dp_phys_ack_t *)mp->b_wptr)->dl_addr_length = 6;
		((dp_phys_ack_t *)mp->b_wptr)->dl_addr_offset = sizeof(dp_phys_ack_t);
		mp->b_wptr += sizeof(dp_phys_ack_t);
		bcopy((caddr_t)dp_sc.ea, (caddr_t)mp->b_wptr, 6);
		mp->b_wptr += 6;
		qreply(q, mp);
		return;

	case DL_ATTACH_REQ:
	case DL_DETACH_REQ:
		dp_errack(q, mp, prim, DL_NOTSUPPORTED, 0);
		return;

	default:
		dp_errack(q, mp, prim, DL_BADPRIM, 0);
		return;
	}
}

/* ---------------------------------------------------------------- ioctl */

static void
dp_iocack(q, mp, db, count, err)
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
dp_getstatus(st)
register aen_status_t *st;
{
	register struct dp_stats *s = &dp_sc.st;

	bzero((caddr_t)st, sizeof(*st));
	st->board_state = dp_bstate;
	st->packets_sent = s->opackets;
	st->packets_received = s->ipackets;
	st->allocbs_failed = s->nomem;
	st->couldnt_put = dp_couldnt_put;
	st->miss_error = s->wedged;
	st->bad_start = s->rxbad;
	st->buffer_error = s->ierrors;
	st->board_debug = dp_debug;
	st->retry_errors = s->oerrors;
}

static void
dp_getconfig(c, sp)
register aen_config_t *c;
struct dp_str *sp;
{
	bzero((caddr_t)c, sizeof(*c));
	c->board_base = dp_sc.id;
	c->board_debug = dp_debug;
	bcopy((caddr_t)dp_sc.ea, (caddr_t)c->paddress, 6);
	c->flags = sp->flags;
}

/*
 * AEN_SET_CONFIG: the raw flag.  The station address and promiscuous
 * mode are fixed (a Wi-Fi station sends only from its own address).
 */
static int
dp_setconfig(c, sp)
register aen_config_t *c;
struct dp_str *sp;
{
	dp_debug = c->board_debug;
	if (bcmp((caddr_t)c->paddress, (caddr_t)dp_sc.ea, 6) != 0 ||
	    (c->mode & AEN_MODE_PROM))
		return EINVAL;
	sp->flags = c->flags & AEN_RAW_DATA;
	return 0;
}

/*
 * The Amiga driver's ioctls, I_STR or transparent.  cq_private marks a
 * finished copy (0) or the SET_CONFIG copyin (1).
 */
static void
dp_ioctl(q, mp)
queue_t *q;
mblk_t *mp;
{
	struct iocblk *ioc = (struct iocblk *)mp->b_rptr;
	struct dp_str *sp = (struct dp_str *)q->q_ptr;
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
			dp_iocack(q, mp, M_IOCACK, 0, 0);
			return;
		}
		if (mp->b_cont == 0 || pullupmsg(mp->b_cont, -1) == 0 ||
		    mp->b_cont->b_wptr - mp->b_cont->b_rptr != sizeof(aen_config_t)) {
			freemsg(unlinkb(mp));
			dp_iocack(q, mp, M_IOCNAK, 0, EINVAL);
			return;
		}
		s = dp_setconfig((aen_config_t *)mp->b_cont->b_rptr, sp);
		freemsg(unlinkb(mp));
		dp_iocack(q, mp, s ? M_IOCNAK : M_IOCACK, 0, s);
		return;
	}

	transp = ioc->ioc_count == TRANSPARENT;
	switch (cmd) {
	case AEN_CLEAR_STATUS:
		s = dp_spl();
		bzero((caddr_t)&dp_sc.st, sizeof(dp_sc.st));
		dp_couldnt_put = 0;
		dp_splx(s);
		freemsg(unlinkb(mp));
		dp_iocack(q, mp, M_IOCACK, 0, 0);
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
				dp_iocack(q, mp, M_IOCNAK, 0, EINVAL);
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
			dp_iocack(q, mp, M_IOCNAK, 0, EINVAL);
			return;
		}
		s = dp_setconfig((aen_config_t *)mp->b_cont->b_rptr, sp);
		freemsg(unlinkb(mp));
		dp_iocack(q, mp, s ? M_IOCNAK : M_IOCACK, 0, s);
		return;

	case SIOCSIFFLAGS:
	case SIOCGIFFLAGS:
		dp_iocack(q, mp, M_IOCACK, ioc->ioc_count == TRANSPARENT ?
		    0 : ioc->ioc_count, 0);
		return;

	default:
		freemsg(unlinkb(mp));
		dp_iocack(q, mp, M_IOCNAK, 0, EINVAL);
		return;
	}

	/* the three "get" ioctls */
	uaddr = (transp && mp->b_cont) ? *(caddr_t *)mp->b_cont->b_rptr : 0;
	freemsg(unlinkb(mp));
	if ((bp = allocb(size, BPRI_MED)) == 0) {
		dp_iocack(q, mp, M_IOCNAK, 0, ENOMEM);
		return;
	}
	if (cmd == AEN_GET_STATUS) {
		s = dp_spl();
		dp_getstatus((aen_status_t *)bp->b_wptr);
		dp_splx(s);
	} else if (cmd == AEN_GET_CONFIG)
		dp_getconfig((aen_config_t *)bp->b_wptr, sp);
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
	dp_iocack(q, mp, M_IOCACK, size, 0);
}
