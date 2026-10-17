/* Vnodes: the file system independent file interface. */
#ifndef _SYS_VNODE_H
#define _SYS_VNODE_H

#include <sys/time.h>

typedef enum vtype {
	VNON, VREG, VDIR, VBLK, VCHR, VLNK, VFIFO, VXNAM, VBAD
} vtype_t;

typedef struct vnode {
	u_short		v_flag;
	u_short		v_count;	/* references */
	struct vfs	*v_vfsmountedhere;
	struct vnodeops	*v_op;
	struct vfs	*v_vfsp;	/* file system holding it */
	struct stdata	*v_stream;
	struct page	*v_pages;
	enum vtype	v_type;
	dev_t		v_rdev;		/* device, for VCHR and VBLK */
	caddr_t		v_data;
	char		_v0[36];
} vnode_t;

/* v_flag */
#define VROOT		0x01

typedef struct vnodeops {
	int	(*vop_open)();
	int	(*vop_close)();
	int	(*vop_read)();
	int	(*vop_write)();
	int	(*vop_ioctl)();
	int	(*vop_setfl)();
	int	(*vop_getattr)();
	int	(*vop_setattr)();
	int	(*vop_access)();
	int	(*vop_lookup)();
	int	(*vop_create)();
	int	(*vop_remove)();
	int	(*vop_link)();
	int	(*vop_rename)();
	int	(*vop_mkdir)();
	int	(*vop_rmdir)();
	int	(*vop_readdir)();
	int	(*vop_symlink)();
	int	(*vop_readlink)();
	int	(*vop_fsync)();
	void	(*vop_inactive)();
	int	(*vop_fid)();
	void	(*vop_rwlock)();
	void	(*vop_rwunlock)();
	int	(*vop_seek)();
	int	(*vop_cmp)();
	int	(*vop_frlock)();
	int	(*vop_space)();
	int	(*vop_realvp)();
	int	(*vop_getpage)();
	int	(*vop_putpage)();
	int	(*vop_map)();
	int	(*vop_addmap)();
	int	(*vop_delmap)();
	int	(*vop_poll)();
	int	(*vop_dump)();
	int	(*vop_pathconf)();
	int	(*_vop0[32])();
} vnodeops_t;

#define VOP_OPEN(vpp, m, cr)		(*(*(vpp))->v_op->vop_open)(vpp, m, cr)
#define VOP_CLOSE(vp, f, c, o, cr)	(*(vp)->v_op->vop_close)(vp, f, c, o, cr)
#define VOP_READ(vp, uio, f, cr)	(*(vp)->v_op->vop_read)(vp, uio, f, cr)
#define VOP_WRITE(vp, uio, f, cr)	(*(vp)->v_op->vop_write)(vp, uio, f, cr)
#define VOP_IOCTL(vp, c, a, f, cr, rv)	(*(vp)->v_op->vop_ioctl)(vp, c, a, f, cr, rv)
#define VOP_GETATTR(vp, va, f, cr)	(*(vp)->v_op->vop_getattr)(vp, va, f, cr)
#define VOP_SETATTR(vp, va, f, cr)	(*(vp)->v_op->vop_setattr)(vp, va, f, cr)
#define VOP_ACCESS(vp, m, f, cr)	(*(vp)->v_op->vop_access)(vp, m, f, cr)
#define VOP_LOOKUP(vp, nm, vpp, pn, f, rdir, cr) \
	(*(vp)->v_op->vop_lookup)(vp, nm, vpp, pn, f, rdir, cr)
#define VOP_READDIR(vp, uio, cr, eof)	(*(vp)->v_op->vop_readdir)(vp, uio, cr, eof)
#define VOP_FSYNC(vp, cr)		(*(vp)->v_op->vop_fsync)(vp, cr)
#define VOP_RWLOCK(vp)			(*(vp)->v_op->vop_rwlock)(vp)
#define VOP_RWUNLOCK(vp)		(*(vp)->v_op->vop_rwunlock)(vp)
#define VOP_SEEK(vp, o, no)		(*(vp)->v_op->vop_seek)(vp, o, no)
#define VOP_POLL(vp, ev, any, rev, php)	(*(vp)->v_op->vop_poll)(vp, ev, any, rev, php)

typedef struct vattr {
	long		va_mask;	/* AT_* fields wanted or set */
	vtype_t		va_type;
	mode_t		va_mode;
	uid_t		va_uid;
	gid_t		va_gid;
	dev_t		va_fsid;
	ino_t		va_nodeid;
	nlink_t		va_nlink;
	u_long		va_size0;	/* high word of the size */
	u_long		va_size;
	timestruc_t	va_atime;
	timestruc_t	va_mtime;
	timestruc_t	va_ctime;
	dev_t		va_rdev;
	u_long		va_blksize;
	u_long		va_nblocks;
	u_long		va_vcode;
	char		_va0[32];
} vattr_t;

#define AT_TYPE		0x0001
#define AT_MODE		0x0002
#define AT_UID		0x0004
#define AT_GID		0x0008
#define AT_FSID		0x0010
#define AT_NODEID	0x0020
#define AT_NLINK	0x0040
#define AT_SIZE		0x0080
#define AT_ATIME	0x0100
#define AT_MTIME	0x0200
#define AT_CTIME	0x0400
#define AT_RDEV		0x0800
#define AT_BLKSIZE	0x1000
#define AT_NBLOCKS	0x2000
#define AT_VCODE	0x4000
#define AT_ALL		0x7fff
#define AT_STAT		(AT_MODE|AT_UID|AT_GID|AT_FSID|AT_NODEID|AT_NLINK|\
			 AT_SIZE|AT_ATIME|AT_MTIME|AT_CTIME|AT_RDEV)

#define VREAD		00400
#define VWRITE		00200
#define VEXEC		00100

enum symfollow { NO_FOLLOW, FOLLOW };
enum vcexcl { NONEXCL, EXCL };
enum create { CRCREAT, CRMKNOD, CRMKDIR, CRCORE };
typedef enum symfollow symfollow_t;

#define NULLVP		((struct vnode *)0)
#define NULLVPP		((struct vnode **)0)

#define VN_HOLD(vp)	{ (vp)->v_count++; }
#define VN_RELE(vp)	{ vn_rele(vp); }

extern int vn_open();
extern int vn_rdwr();
extern int vn_close();
extern void vn_rele();
extern vnode_t *specvp();
extern vnode_t *makespecvp();
extern int lookupname();

#endif
