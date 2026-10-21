/* host stand-ins for the kernel the session layer uses */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

typedef char *caddr_t;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned long k_sigset_t;
typedef unsigned long dev_t;
typedef void _VOID;
struct cred { int dummy; };
struct modwrapper { int dummy; };
struct evpd;
struct proc { struct evpd *p_evpdp; k_sigset_t p_sig, p_hold; };
struct guest_ram { int dummy; };
#define	sigdelset(s, n)	(*(s) &= ~(1UL << ((n) - 1)))
#define	sigaddset(s, n)	(*(s) |= 1UL << ((n) - 1))
#define	getmajor(d)	((d) >> 8)
#define	getminor(d)	((d) & 0xff)
#define	PZERO	25
#define	PCATCH	0x100
#define	USTKCLEAR	1
#define	KM_SLEEP	0
#define	bzero(p, n)	memset(p, 0, n)
#define	kmem_zalloc(n, f)	calloc(1, n)
#define	kmem_free(p, n)	free(p)

struct t_user { int u_sigflag; char *u_ar0; };
extern struct t_user u;
#define	psignal	t_psignal
extern struct proc *curproc;
extern int wakeup(), sleep(), psignal(), __amix_fsig(), __amix_sendsig();
extern int guest_attach(), guest_detach(), guest_reflect(), guest_sigtail(), guest_onstack();
extern int guest_profile_add();
extern void guest_profile_del(), guest_trapret();

/* the CPU's status register is the test's */
#define	__volatile__(...)
#define	__asm__
#include "guest.h"
#undef	__asm__
#undef	__volatile__
