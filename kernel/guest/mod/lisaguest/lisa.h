/*
 * lisa.h -- the Lisa profile's container state.  One container.
 */

#include "sys/types.h"
#include "sys/conf.h"		/* before moddefs.h: struct mod_drv_data */
#include "kinc.h"
#include "sys/sysm68k.h"
#include "lisaio.h"

struct lisactr {
	int		l_state;	/* 0 free, 1 running */
	struct proc	*l_proc;
	struct guest_proc *l_gp;
	dev_t		l_dev;
	unsigned long	l_phys, l_rom;	/* launcher's mappings */
	unsigned long	l_ramsize;
	unsigned short	l_slr[4][128], l_sor[4][128];	/* per context */
	int		l_seg;
	int		l_ctx;		/* the context mapped */
	unsigned long	l_map[4096];	/* what each 4 KB page of 0-16 MB maps */
	struct { unsigned long b, m; int c; } l_alias[64];	/* 128 KB windows above 16 MB: pages mapped, context */
	int		l_nalias;
	int		l_irqon;	/* level 1 sources running */
	int		l_vpend, l_tpend;	/* retrace, timer periods pending */
	long		l_tacc;		/* microseconds toward the next timer period */
	unsigned long	l_in[64];	/* input events for level 2 */
	unsigned	l_inh, l_int;	/* their head and tail */
	int		l_timer;
	long		l_tzoff;	/* host local time - UTC, seconds */
	struct lisastat	l_st;
};

extern struct lisactr lisac;
extern int lisa_trace;
