/* Process table entry: the fields modules use, the rest kept as padding. */
#ifndef _SYS_PROC_H
#define _SYS_PROC_H

#include <sys/immu.h>

typedef struct proc {
	char		p_stat;
	char		_p0[3];
	u_int		p_flag;
	char		_p1[8];
	struct cred	*p_cred;
	char		_p2[16];
	struct proc	*p_parent;
	char		_p3[20];
	caddr_t		p_stkbase;
	int		p_stksize;
	char		_p4[56];
	struct as	*p_as;
	char		_p5[6];
	u_char		p_cursig;	/* signal being delivered */
	char		_p6[21];
	k_sigset_t	p_sig;		/* pending */
	k_sigset_t	p_sigmask;	/* traced */
	k_sigset_t	p_hold;		/* blocked */
	char		_p7[32];
	struct evpd	*p_evpdp;
	char		_p8[36];
	struct sigqueue	*p_sigqueue;
	struct sigqueue	*p_curinfo;
	char		_p9[16];
	struct pid	*p_pidp;
	struct pid	*p_pgidp;
	char		_p10[4];
	struct proc	*p_next;
	char		_p11[16];
} proc_t;

struct pid {
	unsigned int	pid_prinactive:1;
	unsigned int	pid_pgorphaned:1;
	unsigned int	pid_ref:6;
	unsigned int	pid_prslot:24;
	pid_t		pid_id;
	struct proc	*pid_pglink;
	struct pid	*pid_link;
};

#define p_pid		p_pidp->pid_id
#define p_pgrp		p_pgidp->pid_id

#define PID_HOLD(pidp)	(++(pidp)->pid_ref)
#define PID_RELE(pidp)	((pidp)->pid_ref>1 ? --(pidp)->pid_ref : pid_rele(pidp))

/* p_stat */
#define SSLEEP		1
#define SRUN		2
#define SZOMB		3
#define SSTOP		4
#define SIDL		5
#define SONPROC		6

/* p_flag */
#define SSYS		0x00000001
#define STRC		0x00000002
#define SLOAD		0x00000010
#define SPRSTOP		0x00000100
#define SNOWAIT		0x02000000

extern proc_t *practive;

#ifdef __STDC__
extern void psignal(proc_t *, int);
extern int issig(int);
extern void psig(void);
extern proc_t *prfind(pid_t);
extern int pid_rele(struct pid *);
#else
extern void psignal();
extern int issig();
extern void psig();
extern proc_t *prfind();
extern int pid_rele();
#endif

#endif
