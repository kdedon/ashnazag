/*
 * tos.h -- the TOS profile's container state.  One container.
 */

#include "sys/types.h"
#include "sys/conf.h"		/* before moddefs.h: struct mod_drv_data */
#include "kinc.h"
#include "sys/sysm68k.h"
#include "sys/time.h"
#include "tosio.h"

#define	SR_S		0x2000
#define	TGF_SOLO	0x20000		/* gp_flags: TEF_NOMACH guest */
typedef char tgf_profile[(TGF_SOLO & GPF_PROFILE) == TGF_SOLO ? 1 : -1];
#define	SR_IPL		0x0700

/* MFP channels: vector $40 + channel */
#define	CH_TIMERC	5
#define	CH_TIMERB	8
#define	CH_TIMERA	13
#define	CH_GPIP7	15

struct tostimer {
	unsigned long	tm_per;		/* MFP clocks per period, 0 stopped */
	unsigned long	tm_acc;
	int		tm_ev, tm_left;	/* event mode: events per interrupt, events left */
};

struct tosctr {
	int		t_state;	/* 0 free, 1 running */
	struct proc	*t_proc;
	struct guest_proc *t_gp;
	uid_t		t_uid;
	int		t_tid;		/* tick callout */
	long		t_tickbolt;	/* lbolt at its last run */
	unsigned long	t_ramsize;
	unsigned long	t_flags;	/* TEF_* */

	unsigned char	t_ipend[16];	/* MFP channels' interrupts owed */
	unsigned short	t_ipmask;	/* channels owing any */
	unsigned short	t_ien;		/* MFP channels enabled */
	struct tostimer	t_tm[2];	/* Timer A, B */
	unsigned long	t_tcacc;
	int		t_vblpend;
	int		t_vblowed;	/* ticks that came while a held VBL waited */
	int		t_sleeping;	/* in stop, waiting for an interrupt */
	int		t_paused;	/* in the background: sleeps, no interrupts */
	int		t_held;		/* requests held at an emulation tail */
	long		t_heldsince;	/* lbolt when the first was held */
	pid_t		t_ppid;		/* who paused it */
	struct proc	*t_pproc;
	unsigned long	t_sinkva;	/* the writable I/O page, saved in the sink; 0 none */
	unsigned long	t_sinkoff;	/* that I/O page's offset in the RAM object */
	int		t_tcsvc;	/* Timer C in service */
	long		t_tcsince;	/* lbolt at its delivery */

	/* 68030 MMU registers, recorded only */

	struct tosstat	t_st;
};

extern struct tosctr tosc;
extern int tos_trace;

/* tosguest.c */
extern void tos_kick();
extern void mfp_pend();
extern int tos_berr();
extern int tos_toppage();
extern int tos_iowrite();
extern void tos_unsink();

/* tosdev.c */
extern int tos_fault();
extern int tos_access();
extern void tos_devinit();
extern void tos_timers();
extern int mfp_level();
extern int mfp_ready();
extern int mfp_ack();
extern void mfp_unack();
extern void tos_xbtimer();
extern void tos_inten();
extern void tos_snd();
