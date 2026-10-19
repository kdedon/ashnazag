/* Address space segments and their driver operations. */
#ifndef _VM_SEG_H
#define _VM_SEG_H

#include "vm/faultcode.h"
#include "vm/mp.h"

enum fault_type { F_INVAL, F_PROT, F_SOFTLOCK, F_SOFTUNLOCK };
enum seg_rw { S_OTHER, S_READ, S_WRITE, S_EXEC };

struct seg {
	mon_t		s_lock;
	addr_t		s_base;
	u_int		s_size;
	struct as	*s_as;
	struct seg	*s_next;
	struct seg	*s_prev;
	struct seg_ops {
#ifdef __STDC__
		int		(*dup)(struct seg *, struct seg *);
		int		(*unmap)(struct seg *, addr_t, u_int);
		void		(*free)(struct seg *);
		faultcode_t	(*fault)(struct seg *, addr_t, u_int, enum fault_type, enum seg_rw);
		faultcode_t	(*faulta)(struct seg *, addr_t);
		void		(*unload)(struct seg *, addr_t, u_int, u_int);
		int		(*setprot)(struct seg *, addr_t, u_int, u_int);
		int		(*checkprot)(struct seg *, addr_t, u_int, u_int);
		int		(*kluster)(struct seg *, addr_t, int);
		u_int		(*swapout)(struct seg *);
		int		(*sync)(struct seg *, addr_t, u_int, int, u_int);
		int		(*incore)(struct seg *, addr_t, u_int, char *);
		int		(*lockop)(struct seg *, addr_t, u_int, int, int, ulong *, u_int);
		int		(*getprot)(struct seg *, addr_t, u_int, u_int *);
		off_t		(*getoffset)(struct seg *, addr_t);
		int		(*gettype)(struct seg *, addr_t);
		int		(*getvp)(struct seg *, addr_t, struct vnode **);
#else
		int		(*dup)();
		int		(*unmap)();
		void		(*free)();
		faultcode_t	(*fault)();
		faultcode_t	(*faulta)();
		void		(*unload)();
		int		(*setprot)();
		int		(*checkprot)();
		int		(*kluster)();
		u_int		(*swapout)();
		int		(*sync)();
		int		(*incore)();
		int		(*lockop)();
		int		(*getprot)();
		off_t		(*getoffset)();
		int		(*gettype)();
		int		(*getvp)();
#endif
	} *s_ops;
	_VOID		*s_data;
};

#ifdef _KERNEL
struct seg *seg_alloc();
int	seg_attach();
void	seg_free();
#define seg_page(seg, addr)	((u_int)(((addr) - (seg)->s_base) >> PAGESHIFT))
#define seg_pages(seg)		((u_int)(((seg)->s_size + PAGEOFFSET) >> PAGESHIFT))
#ifdef __STDC__
extern void seg_unmap(struct seg *);
#else
extern void seg_unmap();
#endif
#endif

#endif
