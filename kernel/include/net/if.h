/* Network interface flags, requests and statistics. */
#ifndef _NET_IF_H
#define _NET_IF_H

#define IFF_UP		0x1
#define IFF_BROADCAST	0x2
#define IFF_DEBUG	0x4
#define IFF_LOOPBACK	0x8
#define IFF_POINTOPOINT	0x10
#define IFF_NOTRAILERS	0x20
#define IFF_RUNNING	0x40
#define IFF_NOARP	0x80
#define IFF_PROMISC	0x100
#define IFF_ALLMULTI	0x200

/* one per interface unit, chained from ifstats for netstat */
struct ifstats {
	struct ifstats	*ifs_next;
	char		*ifs_name;
	short		ifs_unit;
	short		ifs_active;
	struct ifaddr	*ifs_addrs;
	short		ifs_mtu;
	int		ifs_ipackets;
	int		ifs_ierrors;
	int		ifs_opackets;
	int		ifs_oerrors;
	int		ifs_collisions;
};

struct ifqueue {
	struct mbuf	*ifq_head;
	struct mbuf	*ifq_tail;
	int		ifq_len;
	int		ifq_maxlen;
	int		ifq_drops;
};

#define IFNAMSIZ	16

struct ifreq {
	char	ifr_name[IFNAMSIZ];
	union {
		struct sockaddr	ifru_addr;
		short		ifru_flags;
		int		ifru_metric;
		char		ifru_data[1];
		char		ifru_enaddr[6];
	} ifr_ifru;
};
#define ifr_addr	ifr_ifru.ifru_addr
#define ifr_flags	ifr_ifru.ifru_flags
#define ifr_metric	ifr_ifru.ifru_metric
#define ifr_data	ifr_ifru.ifru_data
#define ifr_enaddr	ifr_ifru.ifru_enaddr

struct ifconf {
	int	ifc_len;
	union {
		caddr_t		ifcu_buf;
		struct ifreq	*ifcu_req;
	} ifc_ifcu;
};
#define ifc_buf		ifc_ifcu.ifcu_buf
#define ifc_req		ifc_ifcu.ifcu_req

#ifdef _KERNEL
/* tentative definitions: every includer gets these as commons */
struct ifqueue rawintrq;
struct ifnet *ifnet;
#endif

#endif
