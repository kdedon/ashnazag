/*
 * otwire.h -- what crosses the bridge: Open Transport's TPI dialect,
 * its XTI options and errors, and the record, ioctl and event-ring
 * formats shared with the Mac relays.
 *
 * All fields are big-endian 32-bit longs unless noted.  OT's providers
 * use the SVR4 TPI message layouts; only the primitive numbers, the
 * option format and the errno numbering differ from the host's.
 */

#ifndef OTWIRE_H
#define OTWIRE_H

/* ---- OT TPI primitives (requests alphabetical from 101, then acks) ---- */

#define	OT_BIND_REQ		101
#define	OT_CONN_REQ		102
#define	OT_CONN_RES		103
#define	OT_DATA_REQ		104
#define	OT_DISCON_REQ		105
#define	OT_EXDATA_REQ		106
#define	OT_INFO_REQ		107
#define	OT_OPTMGMT_REQ		108
#define	OT_ORDREL_REQ		109
#define	OT_UNBIND_REQ		110
#define	OT_UNITDATA_REQ		111
#define	OT_ADDR_REQ		112
#define	OT_BIND_ACK		122
#define	OT_CONN_CON		123
#define	OT_CONN_IND		124
#define	OT_DATA_IND		125
#define	OT_DISCON_IND		126
#define	OT_ERROR_ACK		127
#define	OT_EXDATA_IND		128
#define	OT_INFO_ACK		129
#define	OT_OK_ACK		130
#define	OT_OPTMGMT_ACK		131
#define	OT_ORDREL_IND		132
#define	OT_UNITDATA_IND		133
#define	OT_UDERROR_IND		134
#define	OT_ADDR_ACK		135
#define	OT_RESOLVEADDR_REQ	142	/* OT-private; answered on the Mac */

/*
 * T_CONN_RES: the second long, the host's QUEUE_ptr, carries the
 * acceptor's cookie (OTX_ATTACH).
 * T_ADDR_ACK: PRIM, LOCADDR_length, LOCADDR_offset, REMADDR_length,
 * REMADDR_offset; 16-byte addresses follow.
 */
#define	OT_ADDRACK_LEN		20

/* ---- addresses ---- */

/* InetAddress: u_short type, u_short port, u_long host, 8 zero bytes */
#define	OT_AF_INET		2
#define	OT_AF_DNS		42	/* resolved on the Mac */
#define	OT_INETADDR_LEN		16

/* ---- XTI options ---- */

/* option header: len (header included), level, name, status; value */
#define	OT_OPTHDR		16
#define	OT_OPTALIGN(n)		(((n) + 3) & ~3)

/* MGMT_flags and per-option status */
#define	OT_NEGOTIATE		0x0004
#define	OT_CHECK		0x0008
#define	OT_DEFAULT		0x0010
#define	OT_SUCCESS		0x0020
#define	OT_FAILURE		0x0040
#define	OT_CURRENT		0x0080
#define	OT_PARTSUCCESS		0x0100
#define	OT_READONLY		0x0200
#define	OT_NOTSUPPORT		0x0400

#define	OT_XTI_GENERIC		0xffff
#define	OT_XTI_DEBUG		0x0001
#define	OT_XTI_LINGER		0x0080	/* { l_onoff, l_linger } */
#define	OT_XTI_SNDBUF		0x1001
#define	OT_XTI_RCVBUF		0x1002
#define	OT_XTI_SNDLOWAT		0x1003
#define	OT_XTI_RCVLOWAT		0x1004

#define	OT_INET_IP		0
#define	OT_IP_OPTIONS		0x0001
#define	OT_IP_TOS		0x0002
#define	OT_IP_TTL		0x0003
#define	OT_IP_REUSEADDR		0x0004
#define	OT_IP_RCVOPTS		0x0005
#define	OT_IP_DONTROUTE		0x0010
#define	OT_IP_BROADCAST		0x0020
#define	OT_IP_HDRINCL		0x1002
#define	OT_IP_RCVDSTADDR	0x1007
#define	OT_IP_MULTICAST_IF	0x1010
#define	OT_IP_MULTICAST_TTL	0x1011
#define	OT_IP_MULTICAST_LOOP	0x1012
#define	OT_IP_ADD_MEMBERSHIP	0x1013
#define	OT_IP_DROP_MEMBERSHIP	0x1014
#define	OT_IP_BROADCAST_IF	0x1015
#define	OT_IP_RCVIFADDR		0x1016

#define	OT_INET_TCP		6
#define	OT_TCP_NODELAY		0x0001
#define	OT_TCP_MAXSEG		0x0002
#define	OT_TCP_KEEPALIVE	0x0008	/* { kp_onoff, kp_timeout (minutes) } */
#define	OT_TCP_NOTIFY_THRESHOLD	0x0010
#define	OT_TCP_ABORT_THRESHOLD	0x0011
#define	OT_TCP_CONN_NOTIFY_THRESHOLD 0x0012
#define	OT_TCP_CONN_ABORT_THRESHOLD 0x0013
#define	OT_TCP_OOBINLINE	0x0014
#define	OT_TCP_URGENT_PTR_TYPE	0x0015

#define	OT_INET_UDP		17
#define	OT_UDP_RX_ICMP		0x0002
#define	OT_UDP_CHECKSUM		0x0600

/* ---- errors ---- */

/* T_ERROR_ACK TLI_error: XTI numbering; 1-19 equal the host's */
#define	OT_TBADADDR		1
#define	OT_TBADOPT		2
#define	OT_TACCES		3
#define	OT_TBADF		4
#define	OT_TNOADDR		5
#define	OT_TOUTSTATE		6
#define	OT_TBADSEQ		7
#define	OT_TSYSERR		8
#define	OT_TBADDATA		10
#define	OT_TBADFLAG		16
#define	OT_TNOTSUPPORT		18

/* UNIX_error, DISCON_reason, ERROR_type: BSD errno numbers */
#define	OT_EACCES		13
#define	OT_EINVAL		22
#define	OT_EAGAIN		35
#define	OT_EADDRINUSE		48
#define	OT_EADDRNOTAVAIL	49
#define	OT_ENETUNREACH		51
#define	OT_ECONNABORTED		53
#define	OT_ECONNRESET		54
#define	OT_ENOBUFS		55
#define	OT_ETIMEDOUT		60
#define	OT_ECONNREFUSED		61
#define	OT_EHOSTUNREACH		65
#define	OT_ENOSR		72

/* ---- records ---- */

/*
 * One TPI message per record, both directions:
 *	u_char type; u_char flags; u_short ctllen; u_long datalen;
 *	ctl[ctllen]; data[datalen]
 * Down, one write() per record.  Up, one read() per record (the stream
 * is in RMSGN mode); a message longer than OTB_MAXREC continues in
 * records with ctllen 0, and every record but its last has OTB_RF_MORE.
 */
#define	OTB_RHDR		8
#define	OTB_R_DATA		0x00	/* M_DATA */
#define	OTB_R_PROTO		0x01	/* M_PROTO */
#define	OTB_R_PCPROTO		0x83	/* M_PCPROTO */
#define	OTB_RF_MORE		0x01
#define	OTB_MAXCTL		1024
#define	OTB_MAXDATA		65536	/* down */
#define	OTB_MAXREC		8192	/* up, header included */

/* ---- ioctls ---- */

#define	OTB_IOC(n)		(('o' << 8) | (n))

/* endpoint stream, I_STR */
#define	OTX_ATTACH		OTB_IOC(1)	/* struct otx_attach */
#define	OTX_TCP			1
#define	OTX_UDP			2

struct otx_attach {
	long		at_session;	/* OTB_GETID */
	unsigned long	at_cookie;	/* unique in the session */
	long		at_proto;	/* OTX_TCP, OTX_UDP */
};

/* session (/dev/otbridge), ioctl(2) */
#define	OTB_GETID		OTB_IOC(0x10)	/* returns the session id */
#define	OTB_SETRING		OTB_IOC(0x11)	/* struct otb_setring; addr 0 removes */
#define	OTB_STATS		OTB_IOC(0x12)	/* struct otb_stats */
#define	OTB_GETRING		OTB_IOC(0x13)	/* returns 1 while the ring is live */

struct otb_setring {
	unsigned long	sr_addr;	/* OTB_RINGSZ bytes, 8-aligned */
	long		sr_sig;		/* 0: SIGPOLL */
};

struct otb_stats {
	long	st_sessions;		/* open sessions */
	long	st_endpoints;		/* otxti instances */
	long	st_kmem;		/* bytes allocated now */
	long	st_posts;		/* ring entries written */
	long	st_merged;		/* events merged into a pending entry */
	long	st_signals;
	long	st_overflows;
	long	st_ringlost;		/* rings the owner stopped mapping */
	long	st_ringleak;		/* rings whose pages stayed locked */
};

/*
 * Station (minor 1, clone): a raw Ethernet port with its own address.
 * One frame (dst, src, type, payload; no CRC) per read and per write;
 * frames from other source addresses are refused.
 */

#define	OTB_STATION		OTB_IOC(0x20)	/* struct otb_station, in and out */
#define	OTB_STSIG		OTB_IOC(0x21)	/* arg: signal on queue empty -> not, 0 none */
#define	OTB_STMULTI		OTB_IOC(0x22)	/* struct otb_stmulti */
#define	OTB_STSTATS		OTB_IOC(0x23)	/* struct otb_ststats */
#define	OTB_ST_BRIDGE		1		/* frames to and from the LAN */

struct otb_station {
	unsigned char	st_mac[6];	/* 0: derived from the host's and the uid */
	unsigned short	st_mode;
};

struct otb_stmulti {
	unsigned char	sm_addr[6];
	unsigned short	sm_on;
};

struct otb_ststats {
	unsigned long	ss_rx;		/* frames queued for reading */
	unsigned long	ss_tx;
	unsigned long	ss_drop;	/* queue full or no memory */
	unsigned long	ss_queued;	/* waiting now */
};

/* ---- event ring ---- */

/*
 * OTB_RINGSZ bytes in the Mac process, written by the kernel:
 *	0  u_long  magic	OTB_RMAGIC
 *	4  u_short nent		entries
 *	6  u_short head		next entry the kernel fills
 *	8  u_short tail		next entry the Mac drains (Mac writes)
 *	10 u_short flags	OTB_RING_OVERFLOW (kernel sets, Mac clears)
 *	12 u_long  reserved
 *	16 entries { u_long cookie; u_short events; u_short 0 }
 * An endpoint has at most one entry with nonzero events; new events are
 * or'ed into it.  The Mac drains: take events with a compare-and-swap
 * to 0, advance tail, reread head, repeat.  The signal comes only when
 * the ring goes from empty to non-empty.
 */
#define	OTB_RINGSZ		4096
#define	OTB_RMAGIC		0x6f747231	/* "otr1" */
#define	OTB_RING_HDR		16
#define	OTB_RING_ENT		8
#define	OTB_RING_N		((OTB_RINGSZ - OTB_RING_HDR) / OTB_RING_ENT)
#define	OTB_RING_OVERFLOW	0x0001

#define	OTB_EV_READ		0x0001
#define	OTB_EV_WRITE		0x0002	/* write side drained after a full queue */
#define	OTB_EV_HUP		0x0004
#define	OTB_EV_ERR		0x0008

#endif	/* OTWIRE_H */
