/* Dispatcher: run queue state and preemption. */
#ifndef _SYS_DISP_H
#define _SYS_DISP_H

typedef struct dispq {
	struct proc	*dq_first;
	struct proc	*dq_last;
	int		dq_sruncnt;
} dispq_t;

#ifdef _KERNEL
extern int	runrun;
extern int	kprunrun;
extern struct proc *curproc;
extern int	curpri;
extern int	maxrunpri;

extern void	preempt();
extern void	setbackdq();
extern void	setfrontdq();
extern void	swtch();

#define PREEMPT()	if (kprunrun != 0) { preempt(); }
#endif

#endif
