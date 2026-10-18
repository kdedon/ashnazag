/* Socket types, options and the generic address. */
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#define SOCK_DGRAM	1
#define SOCK_STREAM	2
#define SOCK_RAW	4
#define SOCK_RDM	5
#define SOCK_SEQPACKET	6

/* SOL_SOCKET options */
#define SO_DEBUG	0x0001
#define SO_ACCEPTCONN	0x0002
#define SO_REUSEADDR	0x0004
#define SO_KEEPALIVE	0x0008
#define SO_DONTROUTE	0x0010
#define SO_BROADCAST	0x0020
#define SO_USELOOPBACK	0x0040
#define SO_LINGER	0x0080
#define SO_OOBINLINE	0x0100
#define SO_SNDBUF	0x1001
#define SO_RCVBUF	0x1002
#define SO_SNDLOWAT	0x1003
#define SO_RCVLOWAT	0x1004
#define SO_SNDTIMEO	0x1005
#define SO_RCVTIMEO	0x1006
#define SO_ERROR	0x1007
#define SO_TYPE		0x1008
#define SO_PROTOTYPE	0x1009

#define SOL_SOCKET	0xffff

#define AF_UNSPEC	0
#define AF_UNIX		1
#define AF_INET		2

#define MSG_OOB		0x1
#define MSG_PEEK	0x2
#define MSG_DONTROUTE	0x4

struct sockaddr {
	u_short	sa_family;
	char	sa_data[14];
};

#endif
