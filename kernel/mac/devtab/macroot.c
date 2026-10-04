/*
 * Disk root policy for the Quadra.  No kernel headers, so it also builds
 * on the host.  mac_diskpick() runs from config() (MMU off, BSS not yet
 * cleared): nothing here may live in BSS.
 *
 * Minors are dd minors: bits 0-2 SCSI target, bits 4-6 slice.
 */

#include "macroot.h"

/* A/UX kernel_info, the 'Pigs' block A/UX Startup's launch passes in a0 */
#define KI_MAGIC	0x50696773
#define KI_ROOTCTRL	0x88	/* short: SCSI ID */
#define KI_ROOTDRIVE	0x8A	/* LUN */
#define KI_FLAGS	0xB6	/* ushort */
#define KI_ROOTPART	0xB9	/* A/UX partition */
#define KI_SWAPCTRL	0xBA
#define KI_SWAPDRIVE	0xBC
#define KI_SWAPPART	0xBD
#define KI_SIZE		0xBE
#define KI_PARTVALID	0x0008	/* launch -e/-p: the partition fields count */

#define MAXTARGET	6
#define MAXSLICE	7

static unsigned long
be32(p)
unsigned char *p;
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16
	    | p[2] << 8 | p[3];
}

static int
be16s(p)
unsigned char *p;
{
	int v = p[0] << 8 | p[1];

	return v & 0x8000 ? v - 0x10000 : v;
}

/* A/UX partition N is our slice N+1 (see the partition-map scan). */
static long
ki_minor(ctrl, drive, part)
int ctrl, drive, part;
{
	if (ctrl < 0 || ctrl > MAXTARGET || drive != 0 || part + 1 > MAXSLICE)
		return -1;
	return DDMINOR(ctrl, part + 1);
}

/*
 * Root and swap from the hand-off block ki (ending no later than lim),
 * else c0d0s1 and c0d0s2.  Like A/UX: without KI_PARTVALID the root is
 * partition 0 and swap partition 1 of the root disk.
 */
int
mac_diskpick(ki, lim, rootm, swapm)
unsigned char *ki, *lim;
long *rootm, *swapm;
{
	long r, s;
	int valid;

	*rootm = DDMINOR(0, SLICE_ROOT);
	*swapm = DDMINOR(0, SLICE_SWAP);
	if (ki == 0 || lim == 0 || ki + KI_SIZE > lim || be32(ki) != KI_MAGIC)
		return PICK_DEFAULT;
	valid = (be16s(ki + KI_FLAGS) & KI_PARTVALID) != 0;
	r = ki_minor(be16s(ki + KI_ROOTCTRL), ki[KI_ROOTDRIVE],
	    valid ? ki[KI_ROOTPART] : 0);
	if (r < 0)
		return PICK_DEFAULT;
	s = valid ? ki_minor(be16s(ki + KI_SWAPCTRL), ki[KI_SWAPDRIVE],
	    ki[KI_SWAPPART]) : -1;
	if (s < 0)
		s = DDMINOR(r & 7, SLICE_SWAP);
	*rootm = r;
	*swapm = s;
	return valid ? PICK_KIEXPLICIT : PICK_KIROOT;
}

/* "/dev/dsk/cNd0sM" for dd minor m */
void
mac_dskname(d, m)
char *d;
long m;
{
	register char *s;

	for (s = "/dev/dsk/c0d0s0"; (*d++ = *s++) != 0; )
		;
	d[-6] = '0' + (m & 7);
	d[-2] = '0' + (m >> 4 & 7);
}

/*
 * File-system type of a slice from its superblock: rd(arg, blk, n, buf)
 * reads n bytes from 512-byte block blk.  "" if neither magic is found.
 */
char *
mac_fsprobe(rd, arg, buf)
int (*rd)();
char *arg;
unsigned char *buf;
{
	if ((*rd)(arg, S5_SBBLK, 512, buf) == 0
	&& be32(buf + S5_MAGOFF) == (unsigned long)S5_MAGIC)
		return "s5";
	if ((*rd)(arg, UFS_SBBLK, 2048, buf) == 0
	&& be32(buf + UFS_MAGOFF) == (unsigned long)UFS_MAGIC)
		return "ufs";
	return "";
}
