/*
 * next.h -- the NeXTstation Turbo profile: one machine at a time.
 */

#include "sys/types.h"
#include "sys/conf.h"
#include "kinc.h"
#include "sys/sysm68k.h"
#include "sys/time.h"
#include "guest_mem.h"
#include "../../next/profile.h"

#define	SR_S	0x2000
#define	SR_IPL	0x0700
#define	NX_SIG	17		/* SIGUSR2: carries the machine's interrupts */

/* interrupt status bits */
#define	INT_SOFT1	0x00000001
#define	INT_SOFT2	0x00000002
#define	INT_TIMER	0x20000000

extern int nx_dev(), nx_fault(), nx_service(), nx_census();
extern void nx_board(), nx_post();

struct nx {
	int		state;		/* 0 free, 2 claimed, 1 running */
	struct proc	*proc;
	struct guest_proc *gp;
	int		tid, sleeping, flags, opened;
	unsigned long	ramsize;
	unsigned long	stat;		/* interrupts asked for, INT_* */
	unsigned long	istat;		/* delivered and not yet acknowledged, INT_TIMER */
	unsigned long	mmu[10];	/* movec control registers kept for the guest */
	struct guest_ram *ram;
	pid_t		rampid;
	struct nx_info	st;
};
extern struct nx nx;
extern int nx_level();
extern void nx_kick();
extern unsigned long nx_us();
