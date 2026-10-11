#ifndef _FLIO_H
#define _FLIO_H
/*
 * The floppy service: raw track access to the host's floppy drives.
 * /dev/floppyN is drive N.  Opening it claims the drive for the caller;
 * the last close, or the caller's exit, releases it.  One claimant a
 * drive: EBUSY otherwise.  While any drive is claimed the host's own
 * floppy driver is refused, and while that driver has a drive open no
 * claim is granted.  Encoding and decoding sectors is the claimant's.
 */
#define FL_MAJOR 62
#define FL_NDRIVE 4
#define FL_MAXRAW 32766		/* bytes a transfer */

struct flinfo {
	unsigned long fi_flags;		/* FLI_* */
	unsigned long fi_enc;		/* FLE_*: what the raw bytes hold */
	unsigned long fi_tracks;	/* cylinders * heads */
	unsigned long fi_rawlen;	/* bytes in one revolution */
	unsigned long fi_changes;	/* disks inserted or removed since the claim */
};
#define FLI_DISK 1
#define FLI_WPROT 2
#define FLI_HD 4
/* MFM cells, 2 us each, as the disk DMA reads them: no sync, any start */
#define FLE_MFM 1

struct flraw {
	unsigned long fr_track;		/* cylinder * 2 + head */
	unsigned long fr_len;		/* bytes, even, up to FL_MAXRAW */
	unsigned long fr_flags;		/* FLR_* */
	char *fr_buf;
};
/* a write starts at the index pulse */
#define FLR_INDEX 1

#define FLIOC(n) (('F' << 8) | (n))
#define FLIOC_INFO FLIOC(1)	/* out struct flinfo; looks for a disk */
#define FLIOC_READ FLIOC(2)	/* struct flraw */
#define FLIOC_WRITE FLIOC(3)	/* struct flraw */
#define FLIOC_MOTOR FLIOC(4)	/* int: 0 off, 1 on */
#endif
