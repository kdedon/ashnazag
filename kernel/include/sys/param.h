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

/* sleep() priorities; above PZERO a signal wakes the sleeper */
#define PRIBIO		20
#define PZERO		25
#define PWAIT		30
#define PSLEP		39
#define PMASK		0177
#define PCATCH		0400
#define PNOSTOP		01000

/* pages are 2 KB, as in the kernel the headers describe */
#define PAGESIZE	0x800
#define PAGESHIFT	11
#define PAGEOFFSET	(PAGESIZE - 1)
#define PAGEMASK	(~PAGEOFFSET)
#define ptob(x)		((x) << PAGESHIFT)
#define btop(x)		(((unsigned)(x)) >> PAGESHIFT)
#define btopr(x)	((((unsigned)(x) + PAGEOFFSET) >> PAGESHIFT))

#endif
