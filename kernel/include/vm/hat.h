/* Hardware address translation: loading and unloading mappings. */
#ifndef _VM_HAT_H
#define _VM_HAT_H

#include "vm/vm_hat.h"

#ifdef _KERNEL
void	hat_alloc();
void	hat_free();
int	hat_dup();
void	hat_memload();
void	hat_devload();
void	hat_unlock();
void	hat_chgprot();
void	hat_unload();
void	hat_pageunload();
u_int	hat_getkpfnum();

/* hat_*load flags */
#define HAT_NOFLAGS	0x0
#define HAT_LOCK	0x1
#define HAT_UNLOCK	0x2
#define HAT_FREEPP	0x4
#define HAT_RELEPP	0x8
#define HAT_PRELOAD	0x10
#endif

#endif
