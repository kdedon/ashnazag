/*
 * STREAMS DLPI provider for the Quadra 800 on-board SONIC.
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
 *   bind sap 1..0xff       IEEE 802.3 + 802.2 LLC UI frames to that SAP
 *   bind sap 0xAA, then    SNAP (802.2 AA AA 03 + OUI + type); up to
 *     DL_SUBS_BIND_REQ     SN_NSNAP 5-byte OUI/type subs per stream
 *   DL_ENABMULTI_REQ / DL_DISABMULTI_REQ / DL_PHYS_ADDR_REQ (DLPI 2.0
 *   numbering).  A multicast frame goes only to streams that enabled
 *   its address; broadcast goes to every matching stream.
 * Transmit addresses: 6 bytes, or 7 (LLC: + DSAP), or 11 (SNAP: +
 * OUI/type, otherwise the stream's first subs-bind).
 *
 * Levels: the chip interrupts through VIA2 at IPL 2; everything that
 * touches the chip or the stream table runs at IPL 2 or above.
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
#include "sonic.h"

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
} sn_multi_req_t;		/* DL_ENABMULTI_REQ, DL_DISABMULTI_REQ */

typedef struct {
	ulong	dl_primitive;
	ulong	dl_addr_type;
} sn_phys_req_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_addr_length;
	ulong	dl_addr_offset;
} sn_phys_ack_t;

#define SN_NSTR		8	/* streams (slots) */
#define SN_NSNAP	2	/* SNAP subs-binds per stream */
#define SN_NSMC		4	/* multicast addresses per stream */
#define SN_HZ		60
#define SN_IPL		0x200
#define SN_PROM		0x50F08000	/* MAC address PROM */
#define SN_BOARD	0x50F0A000
#define VIA2_BASE	0x50F02000
#define VIA_PCR		0x1800
#define VIA_IER		0x1C00
#define via2(r)		(*(VOL unsigned char *)(VIA2_BASE + (r)))
#define ETHERMTU	1500
#define HDRSZ		14
#define LLCSZ		3
#define SNAPSZ		8

/* stream framing */
#define SNT_ETHER	1
#define SNT_LLC		2
#define SNT_SNAP	3

struct sn_str {
	queue_t		*q;		/* read queue */
	int		state;		/* DL_UNBOUND, DL_IDLE */
	int		type;		/* SNT_*, 0 when unbound */
	unsigned short	sap;
	int		flags;		/* AEN_RAW_DATA */
	int		nsnap;
	unsigned char	snap[SN_NSNAP][5];
	int		nmc;
	unsigned char	mc[SN_NSMC][6];
};

#define SR_GET(s)	__asm__ __volatile__("movew %%sr,%0" : "=d" (s) : : "memory")
#define SR_SET(s)	__asm__ __volatile__("movew %0,%%sr" : : "d" (s) : "memory")

extern struct ifstats *ifstats;
extern unsigned long mac_model;
extern char *panicstr;
extern int timeout(), untimeout();
extern int bcmp();

int snopen(), snclose(), snwput(), snwsrv();
void snintr();
static void sn_watchdog(), sn_ioctl(), sn_proto();

static struct module_info sn_minfo = {
	0x736e, "sn", 1, ETHERMTU, 5 * 1024, 3 * 1024,
};

static struct qinit sn_rinit = {
	NULL, NULL, snopen, snclose, NULL, &sn_minfo, NULL,
};

static struct qinit sn_winit = {
	snwput, snwsrv, NULL, NULL, NULL, &sn_minfo, NULL,
};

struct streamtab sninfo = {
	&sn_rinit, &sn_winit, NULL, NULL,
};

struct sn_softc sn_sc;
unsigned char sn_pool[SN_POOLALLOC];	/* chip-visible: descriptors, buffers */
struct sn_str sn_str[SN_NSTR];
struct ifstats sn_ifstats;
enum BOARD_STATE sn_bstate;
int sn_up;			/* 1 running, -1 no usable chip */
int sn_wid;			/* watchdog timeout id */
unsigned long sn_couldnt_put, sn_debug;
unsigned long sn_nslot;		/* VIA2 slot interrupts from other slots */
unsigned long sn_nintr;		/* interrupts taken through VIA2 CA1 */
int sn_mcref[SN_NCAM];		/* streams per CAM entry (1..15) */

/* patchable */
unsigned char sn_eaddr[6] = { 0 };	/* nonzero: use instead of the PROM */
int sn_anymodel = 0;			/* nonzero: skip the Quadra 800 check */
char *sn_ifname = "aen";		/* ifstats name, as netstat shows it */

static unsigned char sn_bcast[6] = { 255, 255, 255, 255, 255, 255 };

/* raise to IPL 2, never lower; returns the old SR */
static int
sn_spl()
{
	int s, n;

	SR_GET(s);
	if ((s & 0x700) < SN_IPL) {
		n = (s & ~0x700) | SN_IPL;
		SR_SET(n);
	}
	return s;
}

static void
sn_splx(s)
int s;
{
	SR_SET(s);
}

/* ------------------------------------------------------------ address */

static unsigned char
sn_bitrev(b)
register unsigned int b;
{
	register int i, r;

	for (r = 0, i = 0; i < 8; i++, b >>= 1)
		r = (r << 1) | (b & 1);
	return r;
}

static int
sn_apple(a)
unsigned char *a;
{
	return (a[0] == 0x08 && a[1] == 0x00 && a[2] == 0x07) ||
	    (a[0] == 0x00 && a[1] == 0xa0 && a[2] == 0x40) ||
	    (a[0] == 0x00 && a[1] == 0x05 && a[2] == 0x02);
}

static int
sn_usable(a)
unsigned char *a;
{
	register int i, or, and;

	for (or = 0, and = 0xff, i = 0; i < 6; i++) {
		or |= a[i];
		and &= a[i];
	}
	return or != 0 && and != 0xff && (a[0] & 1) == 0;
}

/*
 * Station address.  The PROM holds it with each byte's bits reversed
 * (Apple's OUI 08:00:07 then reads 10:00:e0); a first byte of 0x10 selects
 * the reversal, and a result with an Apple OUI is preferred.  The last
 * resort is CAM entry 15, where the ROM's driver leaves the address; it
 * must be read before the first reset.  Returns the source's name.
 */
static char *
sn_getaddr(ea)
unsigned char *ea;
{
	unsigned char raw[6], rev[6];
	register int i;

	for (i = 0; i < 6; i++)
		if (sn_eaddr[i])
			break;
	if (i < 6) {
		bcopy((caddr_t)sn_eaddr, (caddr_t)ea, 6);
		return "patched";
	}
	for (i = 0; i < 6; i++) {
		raw[i] = ((VOL unsigned char *)SN_PROM)[i];
		rev[i] = sn_bitrev(raw[i]);
	}
	if (raw[0] == 0x10 && sn_usable(rev)) {
		bcopy((caddr_t)rev, (caddr_t)ea, 6);
		return "PROM, bit-reversed";
	}
	if (sn_apple(raw) && sn_usable(raw)) {
		bcopy((caddr_t)raw, (caddr_t)ea, 6);
		return "PROM";
	}
	if (sn_apple(rev) && sn_usable(rev)) {
		bcopy((caddr_t)rev, (caddr_t)ea, 6);
		return "PROM, bit-reversed";
	}
	SN_WR(SN_CR, CR_RST);
	SN_WR(SN_CEP, 15);
	i = SN_RD(SN_CAP2); ea[5] = i >> 8; ea[4] = i;
	i = SN_RD(SN_CAP1); ea[3] = i >> 8; ea[2] = i;
	i = SN_RD(SN_CAP0); ea[1] = i >> 8; ea[0] = i;
	SN_WR(SN_CR, 0);
	if (sn_usable(ea))
		return "CAM 15 (left by the ROM)";
	if (sn_usable(raw)) {
		bcopy((caddr_t)raw, (caddr_t)ea, 6);
		return "PROM, unknown OUI";
	}
	return (char *)0;
}

/* ------------------------------------------------------- chip bring-up */

static int
sn_start()
{
	unsigned char ea[6];
	char *how;
	int s;

	if (sn_up)
		return sn_up > 0;
	if (mac_model != 35 && !sn_anymodel) {
		printf("sn0: machine %d is not a Quadra 800; no Ethernet\n",
		    (int)mac_model);
		sn_up = -1;
		return 0;
	}
	if (sn_attach(&sn_sc, sn_pool) < 0) {
		sn_up = -1;
		return 0;
	}
	s = sn_spl();
	how = sn_getaddr(ea);
	if (how == 0) {
		sn_splx(s);
		printf("sn0: no usable Ethernet address\n");
		sn_up = -1;
		return 0;
	}
	bcopy((caddr_t)ea, (caddr_t)sn_sc.cam[0], 6);
	sn_sc.camvalid = 1;
	sn_bstate = BOARD_IN_INIT_CODE;
	if (sn_init(&sn_sc) < 0) {
		sn_stop(&sn_sc);
		sn_splx(s);
		printf("sn0: SONIC did not initialise\n");
		sn_bstate = BOARD_RESET;
		sn_up = -1;
		return 0;
	}
	/* VIA2 CA1 (slot interrupts, falling edge) on */
	via2(VIA_PCR) &= ~1;
	via2(VIA_IER) = 0x82;
	sn_bstate = BOARD_RUNNING;
	sn_up = 1;
	sn_splx(s);
	printf("sn0: SONIC rev %x at %x, Ethernet %x:%x:%x:%x:%x:%x (%s)\n",
	    SN_RD(SN_SR), SN_BOARD, ea[0], ea[1], ea[2], ea[3], ea[4], ea[5], how);
	sn_wid = timeout(sn_watchdog, (caddr_t)0, SN_HZ);
	return 1;
}

static void
sn_watchdog()
{
	int s;

	s = sn_spl();
	if (sn_up > 0 && sn_watch(&sn_sc) != 0)
		sn_txdone(&sn_sc);
	sn_splx(s);
	if (sn_up > 0)
		sn_wid = timeout(sn_watchdog, (caddr_t)0, SN_HZ);
}

/* From p2int, IPL 2, when VIA2 port A shows slot $9 asserted. */
void
snintr()
{
	if (sn_up <= 0 || panicstr) {
		if (sn_sc.tda)		/* attached: the chip exists */
			SN_WR(SN_IMR, 0);
		return;
	}
	sn_nintr++;
	(void)sn_intr(&sn_sc);
}

/* ------------------------------------------------------------ open/close */

/* ARGSUSED */
int
snopen(q, devp, flag, sflag, credp)
queue_t *q;
dev_t *devp;
int flag, sflag;
cred_t *credp;
{
	register struct sn_str *sp;
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
	if (!sn_start())
		return ENXIO;
	s = sn_spl();
	slot = (minor >> 4) & 0xf;
	if (slot == 0) {
		for (slot = 0; slot < SN_NSTR; slot++)
			if (sn_str[slot].q == 0)
				break;
		if (slot == SN_NSTR) {
			sn_splx(s);
			return ENOSPC;
		}
	} else if (--slot >= SN_NSTR) {
		sn_splx(s);
		return ENXIO;
	} else if (sn_str[slot].q) {
		sn_splx(s);
		return EBUSY;
	}
	sp = &sn_str[slot];
	bzero((caddr_t)sp, sizeof(*sp));
	sp->q = q;
	sp->state = DL_UNBOUND;
	q->q_ptr = (char *)sp;
	WR(q)->q_ptr = (char *)sp;
	*devp = makedevice(getmajor(*devp), (slot + 1) << 4);
	if (sn_ifstats.ifs_name == 0) {
		sn_ifstats.ifs_name = sn_ifname;
		sn_ifstats.ifs_unit = 0;
		sn_ifstats.ifs_mtu = ETHERMTU;
		sn_ifstats.ifs_next = ifstats;
		ifstats = &sn_ifstats;
	}
	sn_ifstats.ifs_active = 1;
	sn_splx(s);
	return 0;
}

static void sn_dropmulti();

int
snclose(q)
queue_t *q;
{
	register struct sn_str *sp = (struct sn_str *)q->q_ptr;
	register int i, s;

	s = sn_spl();
	while (sp->nmc > 0)
		sn_dropmulti(sp, sp->mc[0]);
	sp->q = 0;
	sp->state = DL_UNBOUND;
	q->q_ptr = 0;
	WR(q)->q_ptr = 0;
	for (i = 0; i < SN_NSTR; i++)
		if (sn_str[i].q)
			break;
	if (i == SN_NSTR && sn_up > 0) {
		sn_stop(&sn_sc);
		sn_bstate = BOARD_RESET;
		sn_up = 0;
		sn_ifstats.ifs_active = 0;
		untimeout(sn_wid);
	}
	sn_splx(s);
	return 0;
}

/* --------------------------------------------------------------- output */

/* The n bytes at off in mp's first block, or 0 if they lie outside it. */
static unsigned char *
sn_field(mp, off, n)
mblk_t *mp;
unsigned long off, n;
{
	unsigned long len = mp->b_wptr - mp->b_rptr;

	if (off > len || n > len - off)
		return 0;
	return mp->b_rptr + off;
}

/* Frame one DL_UNITDATA_REQ into the chip; 0 when the ring is full. */
static int
sn_xmit(sp, mp)
register struct sn_str *sp;
mblk_t *mp;
{
	register unsigned char *b, *a;
	register mblk_t *bp;
	dl_unitdata_req_t *dp = (dl_unitdata_req_t *)mp->b_rptr;
	unsigned long alen;
	int hl, n, room;

	if (!sn_txroom(&sn_sc))
		return 0;
	alen = dp->dl_dest_addr_length;
	a = sn_field(mp, dp->dl_dest_addr_offset, alen);
	if (a == 0 || alen < 6 || sp->state != DL_IDLE)
		goto drop;
	b = sn_txbuf(&sn_sc);
	bcopy((caddr_t)a, (caddr_t)b, 6);
	bcopy((caddr_t)sn_sc.cam[0], (caddr_t)b + 6, 6);
	hl = HDRSZ;
	if (sp->type == SNT_ETHER) {
		b[12] = sp->sap >> 8;
		b[13] = sp->sap;
	} else if (sp->type == SNT_LLC) {
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
	room = SN_MAXFRAME - hl;
	for (n = 0, bp = mp->b_cont; bp; bp = bp->b_cont) {
		if (bp->b_datap->db_type != M_DATA || bp->b_wptr <= bp->b_rptr)
			continue;
		if (n + (bp->b_wptr - bp->b_rptr) > room)
			goto drop;
		bcopy((caddr_t)bp->b_rptr, (caddr_t)b + hl + n,
		    bp->b_wptr - bp->b_rptr);
		n += bp->b_wptr - bp->b_rptr;
	}
	if (sp->type != SNT_ETHER) {
		b[12] = (n + hl - HDRSZ) >> 8;	/* 802.3 length */
		b[13] = n + hl - HDRSZ;
	}
	sn_txstart(&sn_sc, hl + n);
	sn_ifstats.ifs_opackets++;
	freemsg(mp);
	return 1;
drop:
	sn_sc.st.oerrors++;
	sn_ifstats.ifs_oerrors++;
	freemsg(mp);
	return 1;
}

int
snwput(q, mp)
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
		s = sn_spl();
		sn_proto(q, mp);
		sn_splx(s);
		break;
	case M_IOCTL:
	case M_IOCDATA:
		sn_ioctl(q, mp);
		break;
	default:
		freemsg(mp);
		break;
	}
	return 0;
}

int
snwsrv(q)
queue_t *q;
{
	register mblk_t *mp;
	int s;

	s = sn_spl();
	while ((mp = getq(q)) != 0) {
		if (!sn_xmit((struct sn_str *)q->q_ptr, mp)) {
			putbq(q, mp);
			break;
		}
	}
	sn_splx(s);
	return 0;
}

/* Descriptors freed: restart the streams that were waiting for them. */
void
sn_txdone(sc)
struct sn_softc *sc;
{
	register int i;

	for (i = 0; i < SN_NSTR; i++)
		if (sn_str[i].q && WR(sn_str[i].q)->q_first)
			qenable(WR(sn_str[i].q));
}

/* ---------------------------------------------------------------- input */

static void
sn_deliver(sp, pkt, len, pay, plen)
register struct sn_str *sp;
unsigned char *pkt, *pay;
int len, plen;
{
	register mblk_t *mp;
	register dl_unitdata_ind_t *dp;
	int off = sizeof(union DL_primitives);

	if (!canput(sp->q->q_next)) {
		sn_couldnt_put++;
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
	sn_sc.st.nomem++;
}

static int
sn_hasmc(sp, a)
register struct sn_str *sp;
unsigned char *a;
{
	register int i;

	for (i = 0; i < sp->nmc; i++)
		if (bcmp((caddr_t)sp->mc[i], (caddr_t)a, 6) == 0)
			return 1;
	return 0;
}

/* One received frame (len without CRC), from sn_intr at IPL 2. */
void
sn_input(sc, pkt, len, st)
struct sn_softc *sc;
unsigned char *pkt;
int len;
unsigned int st;
{
	register struct sn_str *sp;
	register int i, k;
	unsigned char *pay, *llc;
	unsigned short tl;
	int plen, type, mcast, other;

	sn_ifstats.ifs_ipackets++;
	tl = pkt[12] << 8 | pkt[13];
	llc = pkt + HDRSZ;
	if (tl > ETHERMTU) {
		type = SNT_ETHER;
		pay = llc;
		plen = len - HDRSZ;
	} else {
		plen = tl < len - HDRSZ ? tl : len - HDRSZ;
		if (plen >= SNAPSZ && llc[0] == 0xaa && llc[1] == 0xaa &&
		    llc[2] == 0x03) {
			type = SNT_SNAP;
			pay = llc + SNAPSZ;
			plen -= SNAPSZ;
		} else if (plen >= LLCSZ) {
			type = SNT_LLC;
			pay = llc + LLCSZ;
			plen -= LLCSZ;
		} else
			return;
	}
	mcast = (pkt[0] & 1) && bcmp((caddr_t)pkt, (caddr_t)sn_bcast, 6) != 0;
	other = !(pkt[0] & 1) && bcmp((caddr_t)pkt, (caddr_t)sc->cam[0], 6) != 0;
	for (i = 0, sp = sn_str; i < SN_NSTR; i++, sp++) {
		if (sp->q == 0)
			continue;
		if (sp->flags & AEN_RAW_DATA) {
			sn_deliver(sp, pkt, len, pay, plen);
			continue;
		}
		if (sp->state != DL_IDLE || sp->type != type || other)
			continue;
		if (mcast && !sn_hasmc(sp, pkt))
			continue;
		if (type == SNT_ETHER && sp->sap != tl)
			continue;
		if (type == SNT_LLC && sp->sap != llc[0])
			continue;
		if (type == SNT_SNAP) {
			for (k = 0; k < sp->nsnap; k++)
				if (bcmp((caddr_t)sp->snap[k], (caddr_t)llc + 3, 5) == 0)
					break;
			if (k == sp->nsnap)
				continue;
		}
		sn_deliver(sp, pkt, len, pay, plen);
	}
}

/* ------------------------------------------------------------ primitives */

static void
sn_errack(q, mp, prim, err, uerr)
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
sn_okack(q, mp, prim)
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
sn_infoack(q, sp)
queue_t *q;
register struct sn_str *sp;
{
	register mblk_t *mp;
	register dl_info_ack_t *a;

	if ((mp = allocb(sizeof(dl_info_ack_t) + 6, BPRI_HI)) == 0)
		return;
	mp->b_datap->db_type = M_PCPROTO;
	a = (dl_info_ack_t *)mp->b_wptr;
	bzero((caddr_t)a, sizeof(dl_info_ack_t));
	a->dl_primitive = DL_INFO_ACK;
	a->dl_max_sdu = sp->type == SNT_SNAP ? ETHERMTU - SNAPSZ :
	    sp->type == SNT_LLC ? ETHERMTU - LLCSZ : ETHERMTU;
	a->dl_min_sdu = 1;
	a->dl_addr_length = 6;
	a->dl_mac_type = sp->type == SNT_ETHER || sp->type == 0 ?
	    DL_ETHER : DL_CSMACD;
	a->dl_current_state = sp->state;
	a->dl_max_idu = a->dl_max_sdu;
	a->dl_service_mode = DL_CLDLS;
	a->dl_provider_style = DL_STYLE1;
	mp->b_wptr += sizeof(dl_info_ack_t);
	if (sp->state == DL_IDLE) {
		a->dl_addr_offset = sizeof(dl_info_ack_t);
		bcopy((caddr_t)sn_sc.cam[0], (caddr_t)mp->b_wptr, 6);
		mp->b_wptr += 6;
	}
	qreply(q, mp);
}

static void
sn_bindack(q, sp)
queue_t *q;
struct sn_str *sp;
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
	bcopy((caddr_t)sn_sc.cam[0], (caddr_t)mp->b_wptr, 6);
	mp->b_wptr += 6;
	qreply(q, mp);
}

/* CAM entries 1..15 are shared, reference-counted multicast addresses. */
static int
sn_addmulti(sp, a)
register struct sn_str *sp;
unsigned char *a;
{
	register int k, fr;

	if (sn_hasmc(sp, a))
		return 0;
	if (sp->nmc == SN_NSMC)
		return DL_TOOMANY;
	fr = -1;
	for (k = 1; k < SN_NCAM; k++) {
		if (sn_mcref[k] && bcmp((caddr_t)sn_sc.cam[k], (caddr_t)a, 6) == 0)
			break;
		if (!sn_mcref[k] && fr < 0)
			fr = k;
	}
	if (k == SN_NCAM) {
		if (fr < 0)
			return DL_TOOMANY;
		k = fr;
		bcopy((caddr_t)a, (caddr_t)sn_sc.cam[k], 6);
		sn_sc.camvalid |= 1 << k;
		if (sn_filter(&sn_sc) < 0) {
			sn_sc.camvalid &= ~(1 << k);
			return DL_SYSERR;
		}
	}
	sn_mcref[k]++;
	bcopy((caddr_t)a, (caddr_t)sp->mc[sp->nmc++], 6);
	return 0;
}

static void
sn_dropmulti(sp, a)
register struct sn_str *sp;
unsigned char *a;
{
	register int i, k;
	unsigned char m[6];

	bcopy((caddr_t)a, (caddr_t)m, 6);
	for (i = 0; i < sp->nmc; i++)
		if (bcmp((caddr_t)sp->mc[i], (caddr_t)m, 6) == 0)
			break;
	if (i == sp->nmc)
		return;
	for (sp->nmc--; i < sp->nmc; i++)
		bcopy((caddr_t)sp->mc[i + 1], (caddr_t)sp->mc[i], 6);
	for (k = 1; k < SN_NCAM; k++)
		if (sn_mcref[k] && bcmp((caddr_t)sn_sc.cam[k], (caddr_t)m, 6) == 0)
			break;
	if (k < SN_NCAM && --sn_mcref[k] == 0) {
		sn_sc.camvalid &= ~(1 << k);
		(void)sn_filter(&sn_sc);
	}
}

static void
sn_proto(q, mp)
queue_t *q;
mblk_t *mp;
{
	register struct sn_str *sp = (struct sn_str *)q->q_ptr;
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
		if (q->q_first || !sn_xmit(sp, mp))
			putq(q, mp);
		return;

	case DL_INFO_REQ:
		freemsg(mp);
		sn_infoack(q, sp);
		return;

	case DL_BIND_REQ:
		if (len < sizeof(dl_bind_req_t)) {
			sn_errack(q, mp, prim, DL_BADPRIM, 0);
			return;
		}
		if (sp->state != DL_UNBOUND) {
			sn_errack(q, mp, prim, DL_OUTSTATE, 0);
			return;
		}
		sap = p->bind_req.dl_sap;
		if (sap > ETHERMTU && sap <= 0xffff)
			sp->type = SNT_ETHER;
		else if (sap == 0xaa)
			sp->type = SNT_SNAP;
		else if (sap > 0 && sap <= 0xff && (sap & 1) == 0)
			sp->type = SNT_LLC;
		else {
			sn_errack(q, mp, prim, DL_BADSAP, 0);
			return;
		}
		sp->sap = sap;
		sp->nsnap = 0;
		sp->state = DL_IDLE;
		freemsg(mp);
		sn_bindack(q, sp);
		return;

	case DL_SUBS_BIND_REQ:
		if (len < sizeof(dl_subs_bind_req_t) || sp->type != SNT_SNAP ||
		    sp->state != DL_IDLE) {
			sn_errack(q, mp, prim, sp->state != DL_IDLE ?
			    DL_OUTSTATE : DL_BADSAP, 0);
			return;
		}
		a = sn_field(mp, p->subs_bind_req.dl_subs_sap_offset,
		    (unsigned long)5);
		if (p->subs_bind_req.dl_subs_sap_len != 5 || a == 0) {
			sn_errack(q, mp, prim, DL_BADADDR, 0);
			return;
		}
		if (sp->nsnap == SN_NSNAP) {
			sn_errack(q, mp, prim, DL_TOOMANY, 0);
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
			sn_errack(q, mp, prim, DL_OUTSTATE, 0);
			return;
		}
		flushq(q, FLUSHDATA);
		sp->state = DL_UNBOUND;
		sp->type = 0;
		sp->sap = 0;
		sp->nsnap = 0;
		sn_okack(q, mp, prim);
		return;

	case DL_ENABMULTI_REQ:
	case DL_DISABMULTI_REQ:
		a = len < sizeof(sn_multi_req_t) ? 0 : sn_field(mp,
		    ((sn_multi_req_t *)p)->dl_addr_offset, (unsigned long)6);
		if (a == 0 || ((sn_multi_req_t *)p)->dl_addr_length != 6 ||
		    (a[0] & 1) == 0) {
			sn_errack(q, mp, prim, DL_BADADDR, 0);
			return;
		}
		if (prim == DL_DISABMULTI_REQ) {
			if (!sn_hasmc(sp, a)) {
				sn_errack(q, mp, prim, DL_NOTENAB, 0);
				return;
			}
			sn_dropmulti(sp, a);
		} else if ((r = sn_addmulti(sp, a)) != 0) {
			sn_errack(q, mp, prim, r, r == DL_SYSERR ? EIO : 0);
			return;
		}
		sn_okack(q, mp, prim);
		return;

	case DL_PHYS_ADDR_REQ:
		freemsg(mp);
		if ((mp = allocb(sizeof(sn_phys_ack_t) + 6, BPRI_HI)) == 0)
			return;
		mp->b_datap->db_type = M_PCPROTO;
		((sn_phys_ack_t *)mp->b_wptr)->dl_primitive = DL_PHYS_ADDR_ACK;
		((sn_phys_ack_t *)mp->b_wptr)->dl_addr_length = 6;
		((sn_phys_ack_t *)mp->b_wptr)->dl_addr_offset = sizeof(sn_phys_ack_t);
		mp->b_wptr += sizeof(sn_phys_ack_t);
		bcopy((caddr_t)sn_sc.cam[0], (caddr_t)mp->b_wptr, 6);
		mp->b_wptr += 6;
		qreply(q, mp);
		return;

	case DL_ATTACH_REQ:
	case DL_DETACH_REQ:
		sn_errack(q, mp, prim, DL_NOTSUPPORTED, 0);
		return;

	default:
		sn_errack(q, mp, prim, DL_BADPRIM, 0);
		return;
	}
}

/* ---------------------------------------------------------------- ioctl */

static void
sn_iocack(q, mp, db, count, err)
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
sn_getstatus(st)
register aen_status_t *st;
{
	register struct sn_stats *s = &sn_sc.st;

	bzero((caddr_t)st, sizeof(*st));
	st->board_state = sn_bstate;
	st->packets_sent = s->opackets;
	st->packets_received = s->ipackets;
	st->allocbs_failed = s->nomem;
	st->couldnt_put = sn_couldnt_put;
	st->memory_error = s->br;
	st->miss_error = s->rbe + s->rde;
	st->collision_error = s->collisions;
	st->bad_start = s->rxbad;
	st->buffer_error = s->rbae;
	st->overflow = s->rfo;
	st->framming = s->fae;
	st->crc = s->crc;
	st->board_debug = sn_debug;
	st->loss_of_carrier = s->txcarrier;
	st->retry_errors = s->txexc;
}

static void
sn_getconfig(c, sp)
register aen_config_t *c;
struct sn_str *sp;
{
	bzero((caddr_t)c, sizeof(*c));
	c->board_base = SN_BOARD;
	c->board_debug = sn_debug;
	bcopy((caddr_t)sn_sc.cam[0], (caddr_t)c->paddress, 6);
	c->mode = (sn_sc.rcr & RCR_PRO) ? AEN_MODE_PROM : 0;
	c->flags = sp->flags;
}

/* AEN_SET_CONFIG: station address, promiscuous mode, raw flag. */
static int
sn_setconfig(c, sp)
register aen_config_t *c;
struct sn_str *sp;
{
	unsigned short rcr;
	int s, r = 0;

	sn_debug = c->board_debug;
	sp->flags = c->flags & AEN_RAW_DATA;
	rcr = RCR_BRD | ((c->mode & AEN_MODE_PROM) ? RCR_PRO : 0);
	s = sn_spl();
	if (bcmp((caddr_t)c->paddress, (caddr_t)sn_sc.cam[0], 6) != 0 ||
	    rcr != sn_sc.rcr) {
		if (!sn_usable(c->paddress))
			r = EINVAL;
		else {
			bcopy((caddr_t)c->paddress, (caddr_t)sn_sc.cam[0], 6);
			sn_sc.rcr = rcr;
			if (sn_filter(&sn_sc) < 0)
				r = EIO;
		}
	}
	sn_splx(s);
	return r;
}

/*
 * The Amiga driver's ioctls, I_STR or transparent.  cq_private marks a
 * finished copy (0) or the SET_CONFIG copyin (1).
 */
static void
sn_ioctl(q, mp)
queue_t *q;
mblk_t *mp;
{
	struct iocblk *ioc = (struct iocblk *)mp->b_rptr;
	struct sn_str *sp = (struct sn_str *)q->q_ptr;
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
			sn_iocack(q, mp, M_IOCACK, 0, 0);
			return;
		}
		if (mp->b_cont == 0 || pullupmsg(mp->b_cont, -1) == 0 ||
		    mp->b_cont->b_wptr - mp->b_cont->b_rptr != sizeof(aen_config_t)) {
			freemsg(unlinkb(mp));
			sn_iocack(q, mp, M_IOCNAK, 0, EINVAL);
			return;
		}
		s = sn_setconfig((aen_config_t *)mp->b_cont->b_rptr, sp);
		freemsg(unlinkb(mp));
		sn_iocack(q, mp, s ? M_IOCNAK : M_IOCACK, 0, s);
		return;
	}

	transp = ioc->ioc_count == TRANSPARENT;
	switch (cmd) {
	case AEN_CLEAR_STATUS:
		s = sn_spl();
		bzero((caddr_t)&sn_sc.st, sizeof(sn_sc.st));
		sn_couldnt_put = 0;
		sn_splx(s);
		freemsg(unlinkb(mp));
		sn_iocack(q, mp, M_IOCACK, 0, 0);
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
				sn_iocack(q, mp, M_IOCNAK, 0, EINVAL);
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
			sn_iocack(q, mp, M_IOCNAK, 0, EINVAL);
			return;
		}
		s = sn_setconfig((aen_config_t *)mp->b_cont->b_rptr, sp);
		freemsg(unlinkb(mp));
		sn_iocack(q, mp, s ? M_IOCNAK : M_IOCACK, 0, s);
		return;

	case SIOCSIFFLAGS:
	case SIOCGIFFLAGS:
		sn_iocack(q, mp, M_IOCACK, ioc->ioc_count == TRANSPARENT ?
		    0 : ioc->ioc_count, 0);
		return;

	default:
		freemsg(unlinkb(mp));
		sn_iocack(q, mp, M_IOCNAK, 0, EINVAL);
		return;
	}

	/* the three "get" ioctls */
	uaddr = (transp && mp->b_cont) ? *(caddr_t *)mp->b_cont->b_rptr : 0;
	freemsg(unlinkb(mp));
	if ((bp = allocb(size, BPRI_MED)) == 0) {
		sn_iocack(q, mp, M_IOCNAK, 0, ENOMEM);
		return;
	}
	if (cmd == AEN_GET_STATUS) {
		s = sn_spl();
		sn_getstatus((aen_status_t *)bp->b_wptr);
		sn_splx(s);
	} else if (cmd == AEN_GET_CONFIG)
		sn_getconfig((aen_config_t *)bp->b_wptr, sp);
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
	sn_iocack(q, mp, M_IOCACK, size, 0);
}
