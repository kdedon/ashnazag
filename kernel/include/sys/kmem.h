/* Kernel memory allocator. */
#ifndef _SYS_KMEM_H
#define _SYS_KMEM_H

#define KM_SLEEP	0
#define KM_NOSLEEP	1

#ifdef __STDC__
extern _VOID *kmem_alloc(size_t, int);
extern _VOID *kmem_zalloc(size_t, int);
extern void kmem_free(_VOID *, size_t);
#else
extern _VOID *kmem_alloc();
extern _VOID *kmem_zalloc();
extern void kmem_free();
#endif

#endif
