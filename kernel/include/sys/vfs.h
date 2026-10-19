/* File system types. */
#ifndef _SYS_VFS_H
#define _SYS_VFS_H

typedef struct {
	long	val[2];
} fsid_t;

#define MAXFIDSZ	16

typedef struct fid {
	u_short	fid_len;
	char	fid_data[MAXFIDSZ];
} fid_t;

typedef struct vfs {
	struct vfs	*vfs_next;
	struct vfsops	*vfs_op;
	struct vnode	*vfs_vnodecovered;	/* mount point */
	u_long		vfs_flag;
	u_long		vfs_bsize;
	int		vfs_fstype;
	fsid_t		vfs_fsid;
	caddr_t		vfs_data;
	dev_t		vfs_dev;
	u_long		vfs_bcount;
	u_short		vfs_nsubmounts;
} vfs_t;

/* vfs_flag */
#define VFS_RDONLY	0x01
#define VFS_MLOCK	0x02
#define VFS_MWAIT	0x04
#define VFS_NOSUID	0x08
#define VFS_REMOUNT	0x10
#define VFS_NOTRUNC	0x20

typedef struct vfsops {
	int	(*vfs_mount)();
	int	(*vfs_unmount)();
	int	(*vfs_root)();
	int	(*vfs_statvfs)();
	int	(*vfs_sync)();
	int	(*vfs_vget)();
	int	(*vfs_mountroot)();
	int	(*vfs_swapvp)();
	int	(*_vfs0[8])();
} vfsops_t;

#define VFS_MOUNT(vfsp, mvp, uap, cr)	(*(vfsp)->vfs_op->vfs_mount)(vfsp, mvp, uap, cr)
#define VFS_UNMOUNT(vfsp, cr)		(*(vfsp)->vfs_op->vfs_unmount)(vfsp, cr)
#define VFS_ROOT(vfsp, vpp)		(*(vfsp)->vfs_op->vfs_root)(vfsp, vpp)
#define VFS_STATVFS(vfsp, sp)		(*(vfsp)->vfs_op->vfs_statvfs)(vfsp, sp)
#define VFS_SYNC(vfsp, flag, cr)	(*(vfsp)->vfs_op->vfs_sync)(vfsp, flag, cr)
#define VFS_VGET(vfsp, vpp, fidp)	(*(vfsp)->vfs_op->vfs_vget)(vfsp, vpp, fidp)

/* mount(2) arguments */
struct mounta {
	char	*spec;
	char	*dir;
	int	flags;
	char	*fstype;
	char	*dataptr;
	int	datalen;
};

typedef struct vfssw {
	char		*vsw_name;
	int		(*vsw_init)();
	struct vfsops	*vsw_vfsops;
	long		vsw_flag;
} vfssw_t;

/* why vfs_mountroot runs */
enum whymountroot { ROOT_INIT, ROOT_REMOUNT, ROOT_UNMOUNT };
typedef enum whymountroot whymountroot_t;

#define VFS_MOUNTROOT(vfsp, init)	(*(vfsp)->vfs_op->vfs_mountroot)(vfsp, init)
#define VFS_SWAPVP(vfsp, vpp, nm)	(*(vfsp)->vfs_op->vfs_swapvp)(vfsp, vpp, nm)

/* VFS_SYNC flags */
#define SYNC_ATTR	0x01
#define SYNC_CLOSE	0x02

extern vfs_t	*getvfs();
extern vfs_t	*vfs_devsearch();
extern vfssw_t	*vfs_getvfssw();
extern void	vfs_add();
extern void	vfs_remove();
extern int	vfs_lock();
extern void	vfs_unlock();
extern struct vfs *rootvfs;
extern struct vfssw vfssw[];
extern int	nfstype;

#endif
