/*
 * dlmtf -- FS module that t_dlm mounts: one empty, read-only
 * directory, mountable once.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/errno.h"
#include "sys/time.h"
#include "sys/cred.h"
#include "sys/uio.h"
#include "sys/vfs.h"
#include "sys/vnode.h"
#include "sys/statvfs.h"
#include "sys/moddefs.h"

extern int fs_nosys(), fs_cmp(), fs_frlock(), fs_poll(), fs_pathconf(), fs_setfl();
extern void fs_rwlock(), fs_rwunlock();
extern int getudev();
extern void bzero();
extern char *strcpy();

static int tf_fstype;
static dev_t tf_dev;
static int tf_mounted;
static struct vnode tf_root;
extern struct vnodeops tf_vnodeops;

static int
tf_zero()
{
	return 0;
}

static void
tf_void()
{
}

static int
tf_mount(vfsp, mvp, uap, cr)
	struct vfs *vfsp;
	struct vnode *mvp;
	char *uap;
	struct cred *cr;
{
	if (mvp->v_type != VDIR)
		return ENOTDIR;
	if (tf_mounted)
		return EBUSY;
	bzero((caddr_t)&tf_root, sizeof tf_root);
	tf_root.v_flag = VROOT;
	tf_root.v_count = 1;
	tf_root.v_op = &tf_vnodeops;
	tf_root.v_vfsp = vfsp;
	tf_root.v_type = VDIR;
	vfsp->vfs_flag |= VFS_RDONLY;
	vfsp->vfs_bsize = 1024;
	vfsp->vfs_fstype = tf_fstype;
	vfsp->vfs_dev = tf_dev;
	vfsp->vfs_fsid.val[0] = tf_dev;
	vfsp->vfs_fsid.val[1] = tf_fstype;
	vfsp->vfs_data = (caddr_t)&tf_root;
	tf_mounted = 1;
	return 0;
}

static int
tf_unmount(vfsp, cr)
	struct vfs *vfsp;
	struct cred *cr;
{
	if (tf_root.v_count > 1)
		return EBUSY;
	tf_mounted = 0;
	return 0;
}

static int
tf_vroot(vfsp, vpp)
	struct vfs *vfsp;
	struct vnode **vpp;
{
	tf_root.v_count++;
	*vpp = &tf_root;
	return 0;
}

static int
tf_statvfs(vfsp, sp)
	struct vfs *vfsp;
	struct statvfs *sp;
{
	bzero((caddr_t)sp, sizeof *sp);
	sp->f_bsize = sp->f_frsize = 1024;
	sp->f_fsid = tf_dev;
	strcpy(sp->f_basetype, "dlmtf");
	sp->f_flag = ST_RDONLY;
	sp->f_namemax = 14;
	return 0;
}

static struct vfsops tf_vfsops = {
	tf_mount, tf_unmount, tf_vroot, tf_statvfs,
	tf_zero, fs_nosys, fs_nosys, fs_nosys
};

static int
tf_getattr(vp, vap, flags, cr)
	struct vnode *vp;
	struct vattr *vap;
	int flags;
	struct cred *cr;
{
	bzero((caddr_t)vap, sizeof *vap);
	vap->va_mask = AT_ALL;
	vap->va_type = VDIR;
	vap->va_mode = 0555;
	vap->va_fsid = tf_dev;
	vap->va_nodeid = 2;
	vap->va_nlink = 2;
	vap->va_blksize = 1024;
	return 0;
}

static int
tf_access(vp, mode, flags, cr)
	struct vnode *vp;
	int mode, flags;
	struct cred *cr;
{
	return mode & VWRITE ? EROFS : 0;
}

static int
tf_lookup(dvp, nm, vpp, pnp, flags, rdir, cr)
	struct vnode *dvp;
	char *nm;
	struct vnode **vpp;
	char *pnp, *rdir;
	int flags;
	struct cred *cr;
{
	if (nm[0] == 0 || (nm[0] == '.' && nm[1] == 0)) {
		tf_root.v_count++;
		*vpp = &tf_root;
		return 0;
	}
	return ENOENT;
}

static int
tf_readdir(vp, uiop, cr, eofp)
	struct vnode *vp;
	struct uio *uiop;
	struct cred *cr;
	int *eofp;
{
	if (eofp)
		*eofp = 1;
	return 0;
}

struct vnodeops tf_vnodeops = {
	tf_zero,	/* open */
	tf_zero,	/* close */
	fs_nosys,	/* read */
	fs_nosys,	/* write */
	fs_nosys,	/* ioctl */
	fs_setfl,
	tf_getattr,
	fs_nosys,	/* setattr */
	tf_access,
	tf_lookup,
	fs_nosys,	/* create */
	fs_nosys,	/* remove */
	fs_nosys,	/* link */
	fs_nosys,	/* rename */
	fs_nosys,	/* mkdir */
	fs_nosys,	/* rmdir */
	tf_readdir,
	fs_nosys,	/* symlink */
	fs_nosys,	/* readlink */
	tf_zero,	/* fsync */
	tf_void,	/* inactive: the root is static */
	fs_nosys,	/* fid */
	fs_rwlock,
	fs_rwunlock,
	tf_zero,	/* seek */
	fs_cmp,
	fs_frlock,
	fs_nosys,	/* space */
	fs_nosys,	/* realvp */
	fs_nosys,	/* getpage */
	fs_nosys,	/* putpage */
	fs_nosys,	/* map */
	fs_nosys,	/* addmap */
	fs_nosys,	/* delmap */
	fs_poll,
	fs_nosys,	/* dump */
	fs_pathconf,
};

static int
tf_init(vswp, fstype)
	struct vfssw *vswp;
	int fstype;
{
	tf_fstype = fstype;
	if (tf_dev == 0)
		tf_dev = makedevice(getudev(), 0);
	vswp->vsw_vfsops = &tf_vfsops;
	return 0;
}

struct mod_fs_data dlmtf_fsdata = { "dlmtf", tf_init, 0 };

static int
dlmtf_load()
{
	return 0;
}

static int
dlmtf_unload()
{
	return 0;
}

MOD_FS_WRAPPER(dlmtf, dlmtf_load, dlmtf_unload, "t_dlm filesystem");
