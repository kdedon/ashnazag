/*
 * hsock.h -- BSD socket calls on host TLI endpoints with sockmod, made
 * in the calling process.  SVR4 numbering and errnos.  Small structures
 * (addresses, option values) are kernel buffers, data stays in user
 * space; each call needs HS_GAP bytes of user scratch.
 */

#ifndef _HSOCK_H
#define _HSOCK_H

#define	HS_GAP		576
#define	HS_ADDR		112	/* longest address */

struct hs {
	int	h_fd;
	caddr_t	h_g;		/* user scratch */
	file_t	*h_fp;
	long	h_ud[7];	/* si_udata */
};
#define	h_tidu		h_ud[0]
#define	h_serv		h_ud[4]
#define	h_state		h_ud[5]

extern int hs_socket();		/* (g, af, type, proto, &fd) */
extern int hs_attach();		/* (h, fd, g): ENOTSOCK unless sockmod */
extern int hs_issock();		/* (fp) */
extern int hs_bind();		/* (h, addr, len) */
extern int hs_listen();		/* (h, backlog) */
extern int hs_connect();	/* (h, addr, len) */
extern int hs_accept();		/* (h, &fd, addr, &len) */
extern int hs_send();		/* (h, ubuf, len, flags, to, tolen, &n) */
extern int hs_recv();		/* (h, ubuf, len, flags, from, &fromlen, &n) */
extern int hs_name();		/* (h, peer, addr, &len) */
extern int hs_getopt();		/* (h, level, name, val, &len) */
extern int hs_setopt();		/* (h, level, name, val, len) */
extern int hs_shutdown();	/* (h, how) */
extern int hs_ioctl();		/* (h, cmd, uarg, &rval) */
extern int hs_connecting();	/* (fp) */
extern int hs_connwait();	/* (h): -1 pending, else error */

#endif	/* _HSOCK_H */
