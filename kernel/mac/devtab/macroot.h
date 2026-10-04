/* Disk root policy: shared by config(), the root probe and host tests. */

#define DDMINOR(t, s)	((long)(s) << 4 | (t))
#define SLICE_ROOT	1
#define SLICE_SWAP	2

/* mac_diskpick() results, weakest first */
#define PICK_DEFAULT	0	/* c0d0s1, swap c0d0s2 */
#define PICK_KIROOT	1	/* A/UX Startup's root disk */
#define PICK_KIEXPLICIT	2	/* launch -e/-p partitions */
#define PICK_ROOTARG	3	/* root= on the command line */

/* Superblocks; offsets checked against the kernel headers at build time */
#define S5_SBBLK	1	/* SUPERBOFF / 512 */
#define S5_MAGOFF	504	/* struct filsys s_magic */
#define S5_MAGIC	0xfd187e20
#define UFS_SBBLK	16	/* SBLOCK */
#define UFS_MAGOFF	1372	/* struct fs fs_magic */
#define UFS_MAGIC	0x011954

int	mac_diskpick();
void	mac_dskname();
char	*mac_fsprobe();
