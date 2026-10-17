/* Signals (SVR4 numbering) and the kernel's one-word signal sets. */
#ifndef _SYS_SIGNAL_H
#define _SYS_SIGNAL_H

#define SIGHUP		1
#define SIGINT		2
#define SIGQUIT		3
#define SIGILL		4
#define SIGTRAP		5
#define SIGIOT		6
#define SIGABRT		6
#define SIGEMT		7
#define SIGFPE		8
#define SIGKILL		9
#define SIGBUS		10
#define SIGSEGV		11
#define SIGSYS		12
#define SIGPIPE		13
#define SIGALRM		14
#define SIGTERM		15
#define SIGUSR1		16
#define SIGUSR2		17
#define SIGCLD		18
#define SIGCHLD		18
#define SIGPWR		19
#define SIGWINCH	20
#define SIGURG		21
#define SIGPOLL		22
#define SIGIO		22
#define SIGSTOP		23
#define SIGTSTP		24
#define SIGCONT		25
#define SIGTTIN		26
#define SIGTTOU		27
#define SIGVTALRM	28
#define SIGPROF		29
#define SIGXCPU		30
#define SIGXFSZ		31
#define NSIG		32
#define MAXSIG		32

#define SIG_DFL		(void (*)())0
#define SIG_IGN		(void (*)())1
#define SIG_HOLD	(void (*)())2

#define SIG_BLOCK	1
#define SIG_UNBLOCK	2
#define SIG_SETMASK	3

typedef struct {
	unsigned long	sigbits[4];
} sigset_t;

struct sigaction {
	int	sa_flags;
	void	(*sa_handler)();
	sigset_t sa_mask;
	int	sa_resv[2];
};

#define SA_ONSTACK	0x00000001
#define SA_RESETHAND	0x00000002
#define SA_RESTART	0x00000004
#define SA_SIGINFO	0x00000008
#define SA_NODEFER	0x00000010
#define SA_NOCLDWAIT	0x00010000
#define SA_NOCLDSTOP	0x00020000

typedef struct sigaltstack {
	char	*ss_sp;
	int	ss_size;
	int	ss_flags;
} stack_t;

#define SS_ONSTACK	0x00000001
#define SS_DISABLE	0x00000002

/* k_sigset_t: bit n-1 is signal n */
extern k_sigset_t fillset, cantmask;
#define sigmask(n)		((unsigned long)1 << ((n) - 1))
#define sigemptyset(s)		(*(s) = 0)
#define sigfillset(s)		(*(s) = fillset)
#define sigaddset(s, n)		(*(s) |= sigmask(n))
#define sigdelset(s, n)		(*(s) &= ~sigmask(n))
#define sigismember(s, n)	(sigmask(n) & *(s))
#define sigisempty(s)		(*(s) == 0)
#define sigorset(s1, s2)	(*(s1) |= *(s2))
#define sigdiffset(s1, s2)	(*(s1) &= ~(*(s2)))
#define sigutok(us, ks)		(*(ks) = (us)->sigbits[0])

#endif
