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
#define	TGF_SOLO	0x10000		/* gp_flags: TEF_NOMACH guest */
#define	SR_IPL		0x0700

/* MFP 68901: registers at the odd addresses $FFFA01 + 2n */
#define	MFP_GPIP	0
#define	MFP_AER		1
#define	MFP_DDR		2
#define	MFP_IERA	3
#define	MFP_IERB	4
#define	MFP_IPRA	5
#define	MFP_IPRB	6
#define	MFP_ISRA	7
#define	MFP_ISRB	8
#define	MFP_IMRA	9
#define	MFP_IMRB	10
#define	MFP_VR		11
#define	MFP_TACR	12
#define	MFP_TBCR	13
#define	MFP_TCDCR	14
#define	MFP_TADR	15		/* ... TDDR 18 */
#define	MFP_NREG	24

/* channels: bit n of the A:B register pairs */
#define	CH_TIMERD	4
#define	CH_TIMERC	5
#define	CH_ACIA		6		/* GPIP4 */
#define	CH_FDC		7		/* GPIP5 */
#define	CH_TIMERB	8
#define	CH_TIMERA	13

struct tosmfp {
	unsigned char	m_r[MFP_NREG];
	unsigned short	m_ier, m_ipr, m_isr, m_imr;
	unsigned char	m_cnt[4];	/* timer counters, A-D */
	unsigned long	m_acc[4];	/* MFP clocks towards the next period */
	unsigned char	m_owed[4];	/* periods not yet signalled */
};

#define	ACIA_FIFO	256

struct tosctr {
	int		t_state;	/* 0 free, 1 running */
	struct proc	*t_proc;
	struct guest_proc *t_gp;
	uid_t		t_uid;
	int		t_tid;		/* tick callout */
	unsigned long	t_ramsize;
	unsigned long	t_flags;	/* TEF_* */

	struct tosmfp	t_mfp;		/* ST MFP */
	struct tosmfp	t_mfp2;		/* TT MFP: timers polled, no interrupts */
	int		t_vblpend;
	int		t_vblowed;	/* ticks that came while a held VBL waited */
	int		t_sleeping;	/* in stop, waiting for an interrupt */
	int		t_paused;	/* in the background: sleeps, no interrupts */
	int		t_held;		/* requests held at an emulation tail */
	long		t_heldsince;	/* lbolt when the first was held */
	pid_t		t_ppid;		/* who paused it */
	struct proc	*t_pproc;

	/* IKBD ACIA */
	unsigned char	t_kctl, t_kdata;
	unsigned char	t_kfifo[ACIA_FIFO];
	int		t_khead, t_kcount;
	unsigned char	t_kcmd[8];	/* command being received */
	int		t_kcmdn, t_kcmdlen;
	int		t_mdx, t_mdy;	/* mouse motion not yet queued */
	int		t_mbtn;
	unsigned char	t_bq[8];	/* button changes waiting for FIFO room */
	int		t_bqn;
	int		t_qbtn;		/* buttons of the last packet queued */
	int		t_bsent;	/* a button change went out this tick */

	/* video: $FF8200-$FF82FF and the TT palette $FF8400-$FF85FF */
	unsigned char	t_vid[0x100];
	unsigned char	t_ttpal[0x200];
	unsigned long	t_vgen, t_vcnt;

	unsigned char	t_rtc[64], t_rtcidx;
	unsigned char	t_scu[16];	/* $FF8E00 */
	unsigned char	t_ym[16], t_ymsel;
	unsigned char	t_dma[16];	/* $FF8600 */
	int		t_fdcirq;
	int		t_fdcbusy;	/* the next status read sees the command running */
	unsigned char	t_scsi[32];	/* $FF8700-$FF878F, folded */
	unsigned char	t_scc[16], t_sccptr[2];
	unsigned char	t_misc[16];	/* memory controller and such */
	unsigned char	t_fpal[0x400];	/* Falcon: palette $FF9800 */
	unsigned char	t_snd[0x44];	/* sound and GPIO $FF8900 */
	unsigned char	t_blt[0x40];	/* blitter $FF8A00 */
	unsigned char	t_dsp[8];	/* DSP host port $FFA200 */
	pid_t		t_spid;		/* the sound pump */
	struct proc	*t_sproc;
	unsigned long	t_sgen;		/* $FF8901 writes */

	/* 68030 MMU registers, recorded only */
	unsigned long	t_tc, t_tt0, t_tt1, t_crp[2], t_srp[2];
	unsigned short	t_mmusr;

	struct tosstat	t_st;
};

extern struct tosctr tosc;
extern int tos_trace;

/* tosguest.c */
extern int splhi_();
extern void splx_();
extern void tos_kick();
extern void mfp_pend();
extern int tos_berr();

/* tosdev.c */
extern int tos_fault();
extern void tos_devinit();
extern void tos_nvinit();
extern void tos_input();
extern void tos_timers();
extern int mfp_level();
extern int mfp_ack();
extern void mfp_unack();
extern void tos_snd(), tos_sndend();
