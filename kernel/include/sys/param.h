/* Kernel constants: clock, sleep priorities, pages. */
#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H

#include <sys/types.h>

#define HZ		60
#define NULL		0
#define MAXUID		60002
#define MAXPATHLEN	1024
#define MAXNAMELEN	256
#define NBPC		2048		/* bytes per click */
#define BPCSHIFT	11
#define NCPS		64		/* clicks per segment */
#define CPSSHIFT	6
#define NBPS		0x20000		/* bytes per segment */
#define NBPW		sizeof(int)
#define NBPSCTR		512		/* bytes per disk sector */
#define SCTRSHFT	9
#define DEV_BSIZE	512
#define DEV_BSHIFT	9
#define NOFILE		20
#define USIZE		4		/* u-area pages */
#define MAXSYMLINKS	20

/* sleep() priorities; above PZERO a signal wakes the sleeper */
#define PSWP		0
#define PMEM		0
#define PINOD		10
#define PRIBIO		20
#define PZERO		25
#define PWAIT		30
#define PSLEP		39
#define PUSER		60
#define PIDLE		127
#define PMASK		0177
#define PCATCH		0400
#define PNOSTOP		01000

/* pages are 2 KB, as in the kernel the headers describe */
#define MMU_PAGESIZE	0x800
#define MMU_PAGESHIFT	11
#define MMU_PAGEOFFSET	(MMU_PAGESIZE-1)
#define MMU_PAGEMASK	(~MMU_PAGEOFFSET)
#define PAGESIZE	0x800
#define PAGESHIFT	11
#define PAGEOFFSET	(PAGESIZE - 1)
#define PAGEMASK	(~PAGEOFFSET)
#define ptob(x)		((x) << PAGESHIFT)
#define btop(x)		(((unsigned)(x)) >> PAGESHIFT)
#define btopr(x)	((((unsigned)(x) + PAGEOFFSET) >> PAGESHIFT))

#endif
