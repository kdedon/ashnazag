/* Process address spaces. */
#ifndef _VM_AS_H
#define _VM_AS_H

#include "vm/faultcode.h"
#include "vm/vm_hat.h"

struct as {
	u_int		a_lock: 1;
	u_int		a_want: 1;
	u_int		a_paglck: 1;
	u_int		: 13;
	u_short		a_keepcnt;
	struct seg	*a_segs;
	struct seg	*a_seglast;	/* last hit */
	size_t		a_size;
	size_t		a_rss;
	struct hat	a_hat;
};

#ifdef _KERNEL
/* as_gap direction */
#define AH_DIR		0x1
#define AH_LO		0x0
#define AH_HI		0x1
#define AH_CONTAIN	0x2

struct seg *as_segat();
struct as *as_alloc();
void	as_free();
struct as *as_dup();
int	as_addseg();
faultcode_t as_fault();
faultcode_t as_faulta();
int	as_setprot();
int	as_checkprot();
int	as_unmap();
int	as_map();
int	as_gap();
int	as_memory();
int	as_incore();
int	as_ctl();
u_int	as_getprot();
#endif

#endif
