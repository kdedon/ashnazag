/*
 * AHDI root sector to AMIX slice mapping.
 *
 * Slice 0 is the whole disk; 1 AXR (root), 2 AXS (swap), 3 AXU, 4..7 the
 * GEM/BGM partitions in table order, extended (XGM) ones included.
 * Portable: no kernel headers, big-endian fields read bytewise.
 */

#define AHDI_BSIZE	512
#define AHDI_NSLICE	8

/* root sector byte offsets */
#define AHDI_HDSIZ	0x1C2		/* disk size in blocks */
#define AHDI_PART	0x1C6		/* 4 entries of 12 bytes */
#define AHDI_BSLST	0x1F6		/* bad sector list: first block */
#define AHDI_BSLCNT	0x1FA		/* and blocks */
#define AHDI_PSIZE	12

/* entry byte offsets */
#define AP_FLG		0		/* bit 0: in use */
#define AP_ID		1		/* 3 characters */
#define AP_ST		4		/* first block */
#define AP_SIZ		8		/* blocks */

struct ahdi_slice {
	unsigned long	base;		/* first 512-byte block */
	unsigned long	len;		/* blocks */
	char		id[4];
};

/*
 * rd(arg, blkno, buf) reads one 512-byte block, returns 0 or an errno.
 * ahdi_scan returns 0 and fills sl[0..7], -1 if block 0 holds no AHDI
 * table, or an errno from rd.
 */
int	ahdi_scan();
