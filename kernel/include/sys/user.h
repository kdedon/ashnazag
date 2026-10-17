/* Per-process kernel data (the u area): the fields modules use, the
 * rest kept as padding. */
#ifndef _SYS_USER_H
#define _SYS_USER_H

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/resource.h>

#define MAXSYSARGS	8
#define PSCOMSIZ	14
#define MAXBUSERR	23	/* words in a 68020/030 long bus-error frame */

typedef struct user {
	char		_u0[484];	/* pcb, fpu state */
	int		u_sigsave[MAXBUSERR];
	char		_u1[352];
	int		u_syscall;	/* current system call number */
	char		_u2[544];
	int		u_sigflag;
	char		_u3[4];
	stack_t		u_sigaltstack;
	char		_u4[12];
	k_sigset_t	u_sigrestart;
	k_sigset_t	u_sigoldmask;
	k_sigset_t	u_sigmask[MAXSIG];
	void		(*u_signal[MAXSIG])();
	char		_u5[4];
	label_t		u_qsav;		/* longjmp target when a sleep is interrupted */
	char		_u6[2];
	char		u_error;
	char		_u7[9];
	proc_t		*u_procp;
	int		*u_ap;		/* system call arguments */
	union {
		struct {
			int	r_v1;
			int	r_v2;
		} r_reg;
		off_t	r_off;
		time_t	r_time;
	} u_r;
	char		_u8[28];
	struct vnode	*u_cdir;
	struct vnode	*u_rdir;
	char		_u9[32];
	int		u_arg[MAXSYSARGS];
	struct rlimit	u_rlimit[RLIM_NLIMITS];
	char		_u10[136];
	struct pcb	*u_ar0;		/* saved user registers */
	char		_u11[136];
	char		u_comm[PSCOMSIZ];
	char		_u12[170];
} user_t;

extern struct user u;

#define u_rval1		u_r.r_reg.r_v1
#define u_rval2		u_r.r_reg.r_v2
#define u_roff		u_r.r_off
#define u_cred		u_procp->p_cred

/* u_sigflag */
#define SOMASK		0x001

#endif
