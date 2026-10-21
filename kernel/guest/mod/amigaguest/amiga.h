#ifndef _AMIGA_H
#define _AMIGA_H
#include "sys/types.h"
#include "sys/conf.h"
#include "kinc.h"
#include "sys/sysm68k.h"
#include "amigaio.h"
#include "guest_rpage.h"
#include "guest_env.h"
#define AMIGA_NCPAGE 13
#define AMIGA_SIG 17
#define AMIGA_NOTHER 32
/* with AMIGAF_CENSUS, logged at exit */
struct amigacensus {
	unsigned long priv[AMIGA_NOTHER][3];	/* PC, count, opcode of privileged traps */
	unsigned long site[64][3];	/* PC, address, count of hardware accesses */
};
struct amigactr {
	/* the host's interrupt words: INTENA, INTREQ, the CIAs' ICR bits */
	unsigned short ac_intena, ac_intreq;
	unsigned char ac_icr[2];
	unsigned char ac_icrmask[2];	/* the guest's ICR masks: which bits interrupt */
	unsigned long ac_vbl;	/* E-clock ticks into the frame, times the frame rate */
	struct amigaenter ac_config;
	struct amigastat ac_stat;
	struct guest_proc *ac_gp;
	unsigned long ac_mmu[10];
	unsigned long ac_fraction, ac_epoch;
	unsigned long ac_pvtb;	/* timer B as last copied to guest memory */
	/* the interrupt words as guest memory held them when last read or written; 0x10000 unknown */
	unsigned long ac_pvreq, ac_pvena, ac_pvicr[2];
	/* the host clock: the tick last seen, E-clock ticks since enter */
	long ac_bolt;
	unsigned long ac_eclk;
	/* timer.device's interval timer: E-clock due, raised and not yet seen; its CIA and ICR bit */
	unsigned long ac_tdue;
	unsigned char ac_tarmed, ac_tfired, ac_tcia, ac_tbit;
	struct amigacensus ac_census;
	/* register and absent pages whose reads are constants */
	struct guest_cpage ac_cpage[AMIGA_NCPAGE];
	/* the custom chips' page; INTENAR and INTREQR as last written there */
	unsigned char ac_custom[0x200];
	unsigned long ac_cpreq, ac_cpena;
};
#define AMIGAP(gp) ((struct amigactr *)GUEST_PRIV(gp))
extern int amiga_fault(), amiga_access(), amiga_cpages();
extern void amiga_ring();
/* host calls in the boot extension: SYS: request, ring, sound ring */
#define AMIGA_HOSTVA 0xf7f080UL
extern int amiga_pvget();
extern void amiga_pvput(), amiga_repost(), amiga_pvclock(), amiga_await();
extern void amiga_pvpull(), amiga_pvpush(), amiga_pvreset();
#endif
