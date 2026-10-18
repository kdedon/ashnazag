/* Physical page frames. */
#ifndef _VM_PAGE_H
#define _VM_PAGE_H

typedef struct page {
	u_int		p_lock: 1,
			p_want: 1,
			p_free: 1,
			p_intrans: 1,
			p_gone: 1,
			p_mod: 1,
			p_ref: 1,
			p_pagein: 1,
			p_nc: 1,
			p_age: 1;
	u_int		p_nio: 6;
	u_short		p_keepcnt;
	struct vnode	*p_vnode;
	u_int		p_offset;
	struct page	*p_hash;
	struct page	*p_next;
	struct page	*p_prev;
	struct page	*p_vpnext;
	struct page	*p_vpprev;
	caddr_t		p_mapping;
	u_short		p_lckcnt;
	u_short		p_cowcnt;
	daddr_t		p_dblist[PAGESIZE/NBPSCTR];
	struct proc	*p_uown;
} page_t;

#ifdef _KERNEL
#define PAGE_HOLD(pp)	(pp)->p_keepcnt++
#define PAGE_RELE(pp)	page_rele(pp)

/* page_get flags */
#define P_NOSLEEP	0x0000
#define P_CANWAIT	0x0001
#define P_PHYSCONTIG	0x0002

extern page_t	*pages;
extern page_t	*epages;
extern u_int	pages_base;
extern uint	pages_end;
extern u_int	pages_pp_locked;
extern u_int	pages_pp_claimed;
extern u_int	pages_pp_kernel;
extern u_int	pages_pp_maximum;

page_t	*page_exists();
page_t	*page_find();
page_t	*page_lookup();
int	page_enter();
void	page_abort();
void	page_free();
page_t	*page_get();
int	page_pp_lock();
void	page_pp_unlock();
void	page_wait();
page_t	*page_numtookpp();

#define page_pptonum(pp) \
	(((uint)((pp) - pages) * (PAGESIZE/MMU_PAGESIZE)) + pages_base)
#define page_numtopp(pfnum) \
	((pfnum) < pages_base || (pfnum) >= pages_end) ? \
		((struct page *) NULL) : \
		((struct page *) (&pages[(uint) ((pfnum) - pages_base) / \
			(PAGESIZE/MMU_PAGESIZE)]))

#include "vm/mp.h"
extern mon_t	page_mplock;

/* drop a hold; abort the page once unheld and orphaned */
#define page_rele(pp) { \
	mon_enter(&page_mplock); \
	if (--((struct page *)(pp))->p_keepcnt == 0) { \
		while (((struct page *)(pp))->p_want) { \
			cv_broadcast(&page_mplock, (char *)(pp)); \
			((struct page *)(pp))->p_want = 0; \
		} \
	} \
	mon_exit(&page_mplock); \
	if (((struct page *)(pp))->p_keepcnt == 0 \
		&& (((struct page *)(pp))->p_gone \
		|| ((struct page *)(pp))->p_vnode == NULL)) \
		page_abort(pp); \
}
#endif

#ifdef __STDC__
extern void page_cv_wait(page_t *);
extern void ppcopy(page_t *, page_t *);
#else
extern void page_cv_wait();
extern void ppcopy();
#endif

#endif
