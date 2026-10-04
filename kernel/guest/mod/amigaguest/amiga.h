#ifndef _AMIGA_H
#define _AMIGA_H
#include "sys/types.h"
#include "sys/conf.h"
#include "kinc.h"
#include "sys/sysm68k.h"
#include "amigaio.h"
#include "amigadev.h"
#define AMIGA_SIG 17
struct amigactr {
	struct amigadev ac_dev;
	struct amigaenter ac_config;
	struct amigastat ac_stat;
	struct guest_proc *ac_gp;
	unsigned long ac_mmu[10];
	unsigned long ac_fraction, ac_epoch;
	unsigned char ac_gary[4];
	int ac_timer, ac_sleeping;
	struct amigacensus ac_census;
};
#define AMIGAP(gp) ((struct amigactr *)GUEST_PRIV(gp))
extern int amiga_spl();
extern void amiga_splx();
extern int amiga_fault();
#endif
