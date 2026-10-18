/* STREAMS ioctls and their arguments. */
#ifndef _SYS_STROPTS_H
#define _SYS_STROPTS_H

/* write options */
#define SNDZERO		0x001

/* read options */
#define RNORM		0x000
#define RMSGD		0x001
#define RMSGN		0x002
#define RPROTDAT	0x004
#define RPROTDIS	0x008
#define RPROTNORM	0x010

/* M_FLUSH */
#define FLUSHR		0x01
#define FLUSHW		0x02
#define FLUSHRW		0x03

/* I_SETSIG events */
#define S_INPUT		0x0001
#define S_HIPRI		0x0002
#define S_OUTPUT	0x0004
#define S_MSG		0x0008
#define S_ERROR		0x0010
#define S_HANGUP	0x0020
#define S_RDNORM	0x0040
#define S_WRNORM	S_OUTPUT
#define S_RDBAND	0x0080
#define S_WRBAND	0x0100
#define S_BANDURG	0x0200

#define RS_HIPRI	0x01

/* getmsg returns */
#define MORECTL		1
#define MOREDATA	2

#define STR		('S'<<8)
#define I_NREAD		(STR|01)
#define I_PUSH		(STR|02)
#define I_POP		(STR|03)
#define I_LOOK		(STR|04)
#define I_FLUSH		(STR|05)
#define I_SRDOPT	(STR|06)
#define I_GRDOPT	(STR|07)
#define I_STR		(STR|010)
#define I_SETSIG	(STR|011)
#define I_GETSIG	(STR|012)
#define I_FIND		(STR|013)
#define I_LINK		(STR|014)
#define I_UNLINK	(STR|015)
#define I_RECVFD	(STR|016)
#define I_PEEK		(STR|017)
#define I_FDINSERT	(STR|020)
#define I_SENDFD	(STR|021)
#define I_SWROPT	(STR|023)
#define I_GWROPT	(STR|024)
#define I_LIST		(STR|025)
#define I_PLINK		(STR|026)
#define I_PUNLINK	(STR|027)
#define I_FLUSHBAND	(STR|034)
#define I_CKBAND	(STR|035)
#define I_GETBAND	(STR|036)
#define I_ATMARK	(STR|037)
#define I_SETCLTIME	(STR|040)
#define I_GETCLTIME	(STR|041)
#define I_CANPUT	(STR|042)

#define INFTIM		-1

struct strioctl {
	int	ic_cmd;
	int	ic_timout;
	int	ic_len;
	char	*ic_dp;
};

struct strbuf {
	int	maxlen;
	int	len;
	char	*buf;
};

struct strpeek {
	struct strbuf	ctlbuf;
	struct strbuf	databuf;
	long		flags;
};

struct strfdinsert {
	struct strbuf	ctlbuf;
	struct strbuf	databuf;
	long		flags;
	int		fildes;
	int		offset;
};

#endif
