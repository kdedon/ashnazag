/* Vnode-backed and anonymous memory segments. */
#ifndef _VM_SEG_VN_H
#define _VM_SEG_VN_H

#include "vm/mp.h"

struct segvn_crargs {
	struct vnode	*vp;
	u_int		offset;
	struct cred	*cred;
	u_char		type;		/* MAP_SHARED or MAP_PRIVATE */
	u_char		prot;
	u_char		maxprot;
	struct anon_map	*amp;
};

struct anon_map {
	u_int		refcnt;
	u_int		size;
	struct anon	**anon;
	u_int		swresv;
};

struct segvn_data {
	mon_t		lock;
	u_char		pageprot;
	u_char		prot;
	u_char		maxprot;
	u_char		type;
	struct vnode	*vp;
	u_int		offset;
	u_int		anon_index;
	struct anon_map	*amp;
	struct vpage	*vpage;
	struct cred	*cred;
	u_int		swresv;
};

#ifdef _KERNEL
extern int	segvn_create();
extern struct seg_ops segvn_ops;
extern caddr_t	zfod_argsp;
extern caddr_t	kzfod_argsp;
#endif

#endif
