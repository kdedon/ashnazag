/* Stream head. */
#ifndef _SYS_STRSUBR_H
#define _SYS_STRSUBR_H

#include <sys/poll.h>

typedef struct stdata {
	struct queue	*sd_wrq;	/* write queue of the head */
	struct msgb	*sd_iocblk;
	struct vnode	*sd_vnode;
	struct streamtab *sd_strtab;
	long		sd_flag;
	long		sd_iocid;
	ushort		sd_iocwait;
	struct pid	*sd_sidp;
	struct pid	*sd_pgidp;
	ushort		sd_wroff;
	int		sd_rerror;
	int		sd_werror;
	int		sd_pushcnt;
	int		sd_sigflags;
	struct strevent	*sd_siglist;
	int		sd_eventflags;
	struct strevent	*sd_eventlist;
	struct pollhead	sd_pollist;
	struct msgb	*sd_mark;
	int		sd_closetime;
	clock_t		sd_rtime;
} stdata_t;

/* sd_flag */
#define IOCWAIT		0x00000001
#define RSLEEP		0x00000002
#define WSLEEP		0x00000004
#define STRPRI		0x00000008
#define STRHUP		0x00000010
#define STWOPEN		0x00000020
#define STPLEX		0x00000040	/* linked under a multiplexor */
#define STRISTTY	0x00000080
#define STRDERR		0x00000400
#define STRTIME		0x00000800
#define STRCLOSE	0x00004000
#define STRSNDZERO	0x00040000
#define STRTOSTOP	0x00080000
#define STRMOUNT	0x00400000
#define STRSIGPIPE	0x00800000
#define STRDELIM	0x01000000
#define STWRERR		0x02000000
#define STRHOLD		0x04000000

/* strdoioctl copy mode */
#define U_TO_K		01
#define K_TO_K		02

#endif
