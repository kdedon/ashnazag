/* Signal information and the per-process signal queue. */
#ifndef _SYS_SIGINFO_H
#define _SYS_SIGINFO_H

/* si_code > 0: raised by the kernel */
#define SI_FROMUSER(sip)	((sip)->si_code <= 0)
#define SI_FROMKERNEL(sip)	((sip)->si_code > 0)
#define SI_USER		0

#define ILL_ILLOPC	1
#define ILL_ILLOPN	2
#define ILL_ILLADR	3
#define ILL_ILLTRP	4
#define ILL_PRVOPC	5
#define ILL_PRVREG	6
#define ILL_COPROC	7
#define ILL_BADSTK	8

#define FPE_INTDIV	1
#define FPE_INTOVF	2
#define FPE_FLTDIV	3
#define FPE_FLTOVF	4
#define FPE_FLTUND	5
#define FPE_FLTRES	6
#define FPE_FLTINV	7
#define FPE_FLTSUB	8

#define SEGV_MAPERR	1
#define SEGV_ACCERR	2

#define BUS_ADRALN	1
#define BUS_ADRERR	2
#define BUS_OBJERR	3

#define TRAP_BRKPT	1
#define TRAP_TRACE	2

#define CLD_EXITED	1
#define CLD_KILLED	2
#define CLD_DUMPED	3
#define CLD_TRAPPED	4
#define CLD_STOPPED	5
#define CLD_CONTINUED	6

#define POLL_IN		1
#define POLL_OUT	2
#define POLL_MSG	3
#define POLL_ERR	4
#define POLL_PRI	5
#define POLL_HUP	6

#define SI_MAXSZ	128
#define SI_PAD		((SI_MAXSZ / sizeof(int)) - 3)

/* the process part of both forms */
#define _SI_PROC \
	struct { \
		pid_t	_pid; \
		union { \
			struct { uid_t _uid; } _kill; \
			struct { clock_t _utime; int _status; clock_t _stime; } _cld; \
		} _pdata; \
	} _proc

typedef struct siginfo {
	int	si_signo;
	int	si_code;
	int	si_errno;
	union {
		int	_pad[SI_PAD];
		_SI_PROC;
		struct {
			caddr_t	_addr;
			caddr_t	_pc;
			long	_exop[3];
		} _fault;
		struct {
			int	_fd;
			long	_band;
		} _file;
	} _data;
} siginfo_t;

/* the kernel's copy: no padding, no fault pc */
typedef struct k_siginfo {
	int	si_signo;
	int	si_code;
	int	si_errno;
	union {
		_SI_PROC;
		struct {
			caddr_t	_addr;
		} _fault;
		struct {
			int	_fd;
			long	_band;
		} _file;
	} _data;
} k_siginfo_t;

#define si_pid		_data._proc._pid
#define si_status	_data._proc._pdata._cld._status
#define si_stime	_data._proc._pdata._cld._stime
#define si_utime	_data._proc._pdata._cld._utime
#define si_uid		_data._proc._pdata._kill._uid
#define si_addr		_data._fault._addr
#define si_fd		_data._file._fd
#define si_band		_data._file._band

typedef struct sigqueue {
	struct sigqueue		*sq_next;
	struct k_siginfo	sq_info;
} sigqueue_t;

#ifdef _KERNEL
extern void sigdeq();
extern void sigdelq();
extern void sigaddq();
extern void sigdupq();
extern void winfo();
#endif

#endif
