/* STREAMS queues, messages and driver tables. */
#ifndef _SYS_STREAM_H
#define _SYS_STREAM_H

typedef struct queue {
	struct qinit	*q_qinfo;
	struct msgb	*q_first;
	struct msgb	*q_last;
	struct queue	*q_next;
	struct queue	*q_link;
	_VOID		*q_ptr;		/* driver private */
	ulong		q_count;
	ulong		q_flag;
	long		q_minpsz;
	long		q_maxpsz;
	ulong		q_hiwat;
	ulong		q_lowat;
	struct qband	*q_bandp;
	unsigned char	q_nband;
	char		_q0[11];
} queue_t;

/* q_flag */
#define QENAB		0x001
#define QWANTR		0x002
#define QWANTW		0x004
#define QFULL		0x008
#define QREADR		0x010
#define QUSE		0x020
#define QNOENB		0x040
#define QOLD		0x080
#define QBACK		0x100
#define QHLIST		0x200

struct module_info {
	ushort	mi_idnum;
	char	*mi_idname;
	long	mi_minpsz;
	long	mi_maxpsz;
	ulong	mi_hiwat;
	ulong	mi_lowat;
};

struct qinit {
	int	(*qi_putp)();
	int	(*qi_srvp)();
	int	(*qi_qopen)();
	int	(*qi_qclose)();
	int	(*qi_qadmin)();
	struct module_info *qi_minfo;
	struct module_stat *qi_mstat;
};

struct streamtab {
	struct qinit	*st_rdinit;
	struct qinit	*st_wrinit;
	struct qinit	*st_muxrinit;
	struct qinit	*st_muxwinit;
};

typedef struct free_rtn {
	void	(*free_func)();
	char	*free_arg;
} frtn_t;

typedef struct datab {
	union {
		struct datab	*freep;
		struct free_rtn	*frtnp;
	} db_f;
	unsigned char	*db_base;
	unsigned char	*db_lim;
	unsigned char	db_ref;
	unsigned char	db_type;
	char		_db0[2];
	unsigned int	db_size;
	char		_db1[8];
} dblk_t;

typedef struct msgb {
	struct msgb	*b_next;
	struct msgb	*b_prev;
	struct msgb	*b_cont;
	unsigned char	*b_rptr;
	unsigned char	*b_wptr;
	struct datab	*b_datap;
	unsigned char	b_band;
	char		_b0;
	unsigned short	b_flag;
	char		_b1[4];
} mblk_t;

/* db_type: QPCTL and up are high priority */
#define M_DATA		0x00
#define M_PROTO		0x01
#define M_BREAK		0x08
#define M_PASSFP	0x09
#define M_SIG		0x0b
#define M_DELAY		0x0c
#define M_CTL		0x0d
#define M_IOCTL		0x0e
#define M_SETOPTS	0x10
#define M_IOCACK	0x81
#define M_IOCNAK	0x82
#define M_PCPROTO	0x83
#define M_PCSIG		0x84
#define M_READ		0x85
#define M_FLUSH		0x86
#define M_STOP		0x87
#define M_START		0x88
#define M_HANGUP	0x89
#define M_ERROR		0x8a
#define M_COPYIN	0x8b
#define M_COPYOUT	0x8c
#define M_IOCDATA	0x8d
#define M_STOPI		0x8f
#define M_STARTI	0x90

#define QNORM		0x00
#define QPCTL		0x80

struct iocblk {
	int		ioc_cmd;
	struct cred	*ioc_cr;
	uint		ioc_id;
	uint		ioc_count;
	int		ioc_error;
	int		ioc_rval;
	long		_ioc0[4];
};

/* M_COPYIN/M_COPYOUT request and its M_IOCDATA answer */
struct copyreq {
	int		cq_cmd;
	struct cred	*cq_cr;
	uint		cq_id;
	caddr_t		cq_addr;
	uint		cq_size;
	int		cq_flag;
	mblk_t		*cq_private;
	long		_cq0[4];
};

struct copyresp {
	int		cp_cmd;
	struct cred	*cp_cr;
	uint		cp_id;
	caddr_t		cp_rval;	/* 0 on success */
	char		_cp0[8];
	mblk_t		*cp_private;
	long		_cp1[4];
};

struct stroptions {
	ulong		so_flags;
	short		so_readopt;
	ushort		so_wroff;
	long		so_minpsz;
	long		so_maxpsz;
	ulong		so_hiwat;
	ulong		so_lowat;
	unsigned char	so_band;
};

/* so_flags */
#define SO_READOPT	0x0001
#define SO_WROFF	0x0002
#define SO_MINPSZ	0x0004
#define SO_MAXPSZ	0x0008
#define SO_HIWAT	0x0010
#define SO_LOWAT	0x0020
#define SO_MREADON	0x0040
#define SO_MREADOFF	0x0080
#define SO_NDELON	0x0100
#define SO_NDELOFF	0x0200
#define SO_ISTTY	0x0400
#define SO_ISNTTY	0x0800
#define SO_TOSTOP	0x1000
#define SO_TONSTOP	0x2000

/* open sflag */
#define MODOPEN		0x1
#define CLONEOPEN	0x2
#define OPENFAIL	-1

#define BPRI_LO		1
#define BPRI_MED	2
#define BPRI_HI		3

#define INFPSZ		-1

#define FLUSHALL	1
#define FLUSHDATA	0

#define TRANSPARENT	(unsigned int)(-1)

#define splstr()	spltty()

#define OTHERQ(q)	((q)->q_flag&QREADR? (q)+1: (q)-1)
#define WR(q)		((q)+1)
#define RD(q)		((q)-1)
#define putnext(q, mp)	((*(q)->q_next->q_qinfo->qi_putp)((q)->q_next, (mp)))
#define datamsg(type)	((type) == M_DATA || (type) == M_PROTO || (type) == M_PCPROTO || (type) == M_DELAY)
#define queclass(bp)	(((bp)->b_datap->db_type >= QPCTL) ? QPCTL : QNORM)

extern mblk_t *allocb();
extern mblk_t *esballoc();
extern int bufcall();
extern void unbufcall();
extern int testb();
extern void freeb();
extern void freemsg();
extern mblk_t *dupb();
extern mblk_t *dupmsg();
extern mblk_t *copyb();
extern mblk_t *copymsg();
extern void linkb();
extern mblk_t *unlinkb();
extern mblk_t *rmvb();
extern int pullupmsg();
extern int adjmsg();
extern int msgdsize();
extern mblk_t *getq();
extern void rmvq();
extern void flushq();
extern int canput();
extern int putq();
extern int putbq();
extern int insq();
extern int putctl();
extern int putctl1();
extern queue_t *backq();
extern void qreply();
extern void qenable();
extern void noenable();
extern void enableok();

#endif
