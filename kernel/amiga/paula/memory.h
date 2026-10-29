#ifndef _MEMORY_H
#define _MEMORY_H

#define MEMF_CHIP	0x00002
#define MEMF_CLEAR	0x10000

/* the kernel's AllocMem leaves chip memory uncleared */
#define AllocMem	paula_allocmem
extern char *AllocMem();
extern void FreeMem();

#endif
