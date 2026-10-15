/*
 * next.h -- the NeXTstation Turbo profile: one machine at a time.
 */

#include "sys/types.h"
#include "sys/conf.h"
#include "kinc.h"
#include "sys/sysm68k.h"
#include "sys/time.h"
#include "../../next/profile.h"

#define	SR_S	0x2000
#define	SR_IPL	0x0700
#define	NX_SIG	17		/* SIGUSR2: carries the machine's interrupts */

/* interrupt status bits */
#define	INT_SOFT1	0x00000001
#define	INT_SOFT2	0x00000002
#define	INT_POWER	0x00000004
#define	INT_TIMER	0x20000000
#define	INT_NMI		0x80000000

extern int nx_dev(), nx_read(), nx_write(), nx_badaccess(), nx_fault();
extern void nx_after();

struct nx {
	int		state;		/* 0 free, 2 claimed, 1 running */
	struct proc	*proc;
	struct guest_proc *gp;
	int		tid, sleeping, flags, opened, nnote;
	unsigned long	ramsize;
	unsigned long	stat, mask;	/* interrupt status and mask */
	unsigned char	scr2[4];
	unsigned char	hc_csr;
	unsigned long	hc_latch, hc_due, evoff, evlast;
	/* clock chip behind the SCR2 bit lines */
	int		rtc_phase;
	unsigned char	rtc_addr, rtc_val, rtc_data, rtc_ctl, rtc_stat;
	unsigned long	rtc_time, rtc_alarm;
	unsigned long	mmu[10];	/* movec control registers kept for the guest */
	struct nx_info	st;
};
extern struct nx nx;
extern int nx_splhi(), nx_level();
extern void nx_splx(), nx_kick(), nx_setint(), nx_clrint(), nx_reset();
extern unsigned long nx_us();
