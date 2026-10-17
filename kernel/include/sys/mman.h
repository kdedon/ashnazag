/* Memory mapping protections and flags. */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define PROT_EXEC	0x4
#define PROT_USER	0x8
#define PROT_NONE	0x0
#define PROT_ALL	(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_USER)

#define MAP_SHARED	1
#define MAP_PRIVATE	2
#define MAP_FIXED	0x10

#endif
