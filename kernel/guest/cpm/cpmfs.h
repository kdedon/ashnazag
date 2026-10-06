/*
 * cpmfs.h -- CP/M 2.2 file systems in disk images (cpmtools layout).
 *
 * Two formats, told apart by size:
 *	256256 bytes	ibm-3740: 8" SSSD, 77 tracks of 26, skew 6, 2 boot tracks
 *	16 KB * n	hard disk, 256 KB to 8 MB: 128 sectors of 128 bytes a
 *			track, 4 KB blocks, 1024 directory entries, no boot track
 */

#define	CPM_HDMIN	(256L << 10)
#define	CPM_HDMAX	(8L << 20)

struct cpmfmt {
	int		spt;		/* 128-byte records a track */
	int		bsh;		/* block of 128 << bsh bytes */
	int		exm;
	int		dsm;		/* blocks - 1 */
	int		drm;		/* directory entries - 1 */
	int		al;		/* directory blocks: AL0 << 8 | AL1 */
	int		off;		/* boot tracks */
	unsigned char	*skew;		/* physical sector of each logical one, or 0 */
	long		size;
};

struct cpmimg {
	struct cpmfmt	f;
	char		*mem;		/* the image in memory, or */
	int		fd;		/* its file */
	int		ro;
};

extern int cpm_format();
extern long cpm_secoff();
extern int cpm_io();
extern int cpm_mkfs();
extern int cpm_name();
extern int cpm_put();
