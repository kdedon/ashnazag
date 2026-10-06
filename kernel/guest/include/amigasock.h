/*
 * amigasock.h -- bsdsocket.library's host calls: BSD socket calls of the
 * Amiga environment's own process on its own descriptors, through
 * AMIGAIOC_SOCK on /dev/amiga.  AF_INET with SVR4 types, address layout
 * and errnos.  Host sockets are always nonblocking and raise SIGPOLL,
 * which the guest takes as a PORTS interrupt.
 */

#ifndef _AMIGASOCK_H
#define _AMIGASOCK_H

#define	AMIGAIOC_SOCK	(('A' << 8) | 32)	/* in/out struct amigasock */
#define	BS_GAP		576

struct amigasock {
	long	bs_op;		/* BSO_* */
	long	bs_fd;
	long	bs_arg;		/* type, backlog, flags or peer */
	char	*bs_buf;
	long	bs_len;
	long	bs_rv;		/* descriptor, count or connect state */
	char	bs_addr[16];	/* sockaddr_in, in or out */
	long	bs_alen;	/* in: 0 no address; out: its length */
	char	*bs_gap;	/* BS_GAP bytes of scratch */
};

#define	BSO_SOCKET	1	/* bs_arg type, bs_len protocol -> bs_rv descriptor */
#define	BSO_BIND	2
#define	BSO_CONNECT	3
#define	BSO_LISTEN	4	/* bs_arg backlog */
#define	BSO_ACCEPT	5	/* -> bs_rv descriptor, bs_addr */
#define	BSO_SEND	6	/* bs_buf, bs_len, bs_arg flags -> bs_rv */
#define	BSO_RECV	7	/* bs_buf, bs_len, bs_arg flags -> bs_rv, bs_addr */
#define	BSO_CONNWAIT	8	/* -> bs_rv: -1 pending, else the connect's errno */
#define	BSO_NAME	9	/* bs_arg 1 peer, 0 local -> bs_addr */
#define	BSO_GETOPT	10	/* bs_arg level << 16 | name, bs_buf, bs_len -> bs_rv length */
#define	BSO_SETOPT	11	/* bs_arg level << 16 | name, bs_buf, bs_len */
#define	BSO_SHUTDOWN	12	/* bs_arg how */
#define	BSO_NREAD	13	/* -> bs_rv bytes readable */
#define	BSO_POLL	14	/* bs_buf pollfds, bs_len count -> bs_rv ready; never waits */

#define	BS_NPOLL	256

#endif	/* _AMIGASOCK_H */
