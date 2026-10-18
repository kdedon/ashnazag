/* Device memory segments. */
#ifndef _VM_SEG_DEV_H
#define _VM_SEG_DEV_H

struct segdev_crargs {
	int	(*mapfunc)();
	u_int	offset;
	dev_t	dev;
	u_char	prot;
	u_char	maxprot;
};

struct segdev_data {
	int		(*mapfunc)();
	u_int		offset;
	struct vnode	*vp;
	u_char		pageprot;
	u_char		prot;
	u_char		maxprot;
	struct vpage	*vpage;
};

#ifdef _KERNEL
#ifdef __STDC__
extern int segdev_create(struct seg *, void *);
#else
extern int segdev_create();
#endif
#endif

#endif
