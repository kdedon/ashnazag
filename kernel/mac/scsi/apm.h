/*
 * Apple Partition Map to AMIX slice mapping.
 *
 * Slice 0 is the whole disk; slices 1..7 come from map entries:
 *	1 root, 2 swap, 3 usr (A/UX slice + 1), 4..7 the rest in map order.
 * Portable: no kernel headers, big-endian fields read bytewise.
 */

#define APM_BSIZE	512
#define APM_NSLICE	8
#define APM_MAXENT	64		/* map entries examined */

#define APM_DDM_SIG	0x4552		/* 'ER', block 0 */
#define APM_PM_SIG	0x504D		/* 'PM', blocks 1..n */
#define APM_BZB_MAGIC	0xABADBABEL

/* entry byte offsets */
#define PM_SIG		0x00
#define PM_MAPCNT	0x04
#define PM_START	0x08
#define PM_COUNT	0x0C
#define PM_NAME		0x10
#define PM_TYPE		0x30
#define PM_STATUS	0x58
#define PM_BZB		0x88		/* pmBootArgs: A/UX block zero block */

/* bzb byte offsets (within PM_BZB) */
#define BZB_MAGIC	0x00
#define BZB_CLUSTER	0x04
#define BZB_TYPE	0x05
#define BZB_FLAGS	0x08

#define BZB_ROOT	0x8000
#define BZB_USR		0x4000
#define BZB_CRIT	0x2000
#define BZB_SLICE	0x001F		/* A/UX slice + 1, 0 = none */
#define BZB_FSTSFS	3		/* swap */

/* why a slice was chosen (apm_slice.how) */
#define APM_NONE	0
#define APM_WHOLE	1
#define APM_BZBSLICE	2		/* explicit bzb slice number */
#define APM_BZBROLE	3		/* bzb root/swap/usr */
#define APM_NAMEROLE	4		/* SVR2 entry without bzb: name */
#define APM_UNIX	5		/* other Apple_UNIX_SVR2 */
#define APM_OTHER	6		/* other data partition */

struct apm_slice {
	unsigned long	base;		/* first 512-byte block */
	unsigned long	len;		/* blocks */
	short		entry;		/* map entry (block) number */
	short		how;
	char		name[32];
	char		type[32];
};

/*
 * rd(arg, blkno, buf) reads one 512-byte block, returns 0 or an errno.
 * Returns 0 and fills sl[0..7]; ENXIO-like -1 if the disk has no map.
 */
int	apm_scan();
