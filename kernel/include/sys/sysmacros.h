/* Device numbers and unit conversions. */
#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

/* 14-bit major (8 used), 18-bit minor */
#define L_BITSMINOR	18
#define L_MAXMAJ	0xff
#define L_MAXMIN	0x3ffff
#define major(x)	(int)((unsigned)((x) >> L_BITSMINOR) & L_MAXMAJ)
#define minor(x)	(int)((x) & L_MAXMIN)
#define getmajor(x)	major(x)
#define getminor(x)	minor(x)
#define makedevice(x, y)	(unsigned long)(((x) << L_BITSMINOR) | ((y) & L_MAXMIN))

#define ctob(x)		((x) << BPCSHIFT)
#define btoc(x)		(((unsigned)(x) + (NBPC - 1)) >> BPCSHIFT)
#define btoct(x)	((unsigned)(x) >> BPCSHIFT)

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) < (b) ? (b) : (a))

#endif
