/* Per address space MMU state: the user segment tables. */
#ifndef _VM_VM_HAT_H
#define _VM_VM_HAT_H

#include "sys/immu.h"
#include "vm/page.h"

typedef struct hat {
	sde_t	*hat_sram;
	sde_t	hat_top[7];
} hat_t;

#define HAT_NOSLEEP	0
#define HAT_CANWAIT	1
#define HAT_NOSTEAL	2

#endif
