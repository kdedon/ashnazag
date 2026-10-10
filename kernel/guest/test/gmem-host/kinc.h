#ifndef GMEM_HOST_H
#define GMEM_HOST_H
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#define KM_NOSLEEP 1
typedef unsigned int u_int;
#define KM_SLEEP 0
struct modwrapper { int refs; };
struct page { struct page *p_next; int p_keepcnt, used; unsigned char data[4096]; };
typedef struct page page_t;
extern page_t *pages;
extern u_int pages_base;
extern int page_fail, test_kmem_fail;
extern page_t *page_get();
extern void page_abort();
#define P_NOSLEEP 0
#define P_CANWAIT 1
#define P_PHYSCONTIG 2
#define kmem_alloc(n,f) (test_kmem_fail ? 0 : malloc(n))
#define kmem_zalloc(n,f) (test_kmem_fail ? 0 : calloc(1,n))
#define kmem_free(p,n) free(p)
#define mod_hold(p) ((p)->refs++)
#define mod_rele(p) ((p)->refs--)
struct tune { int t_minarmem; };
#endif
