#ifndef AUX_OT_MPS_H
#define AUX_OT_MPS_H
#include "bootstrap.h"
#include <stddef.h>

struct otr_queue_prefix {
	void *info;
	struct otr_msg *first, *last;
	void *next, *link, *private;
};
#if defined(__m68k__)
#pragma pack(2)
#endif
struct otr_module_info {
	unsigned short number;
	char *name;
	long minimum, maximum;
	unsigned long high, low;
};
#if defined(__m68k__)
#pragma pack()
typedef char otr_queue_private[(offsetof(struct otr_queue_prefix, private) == 20) ? 1 : -1];
typedef char otr_module_info_size[(sizeof(struct otr_module_info) == 22) ? 1 : -1];
#endif
#if defined(__m68k__)
extern unsigned long OTAllocMem_D0(unsigned long);
#define OTAllocMem(n) ((void *)OTAllocMem_D0((n)))
#else
extern void *OTAllocMem(unsigned long);
#endif
extern void OTFreeMem(void *);
#if defined(__m68k__)
extern unsigned long allocb_D0(unsigned long, long);
#define allocb(n,p) ((struct otr_msg *)allocb_D0((n),(p)))
#else
extern struct otr_msg *allocb(unsigned long, long);
#endif
extern void freemsg(struct otr_msg *);
extern void freeb(struct otr_msg *);
extern int canputnext(void *);
extern int putnext(void *, struct otr_msg *);
extern int putq(void *, struct otr_msg *);
extern int putbq(void *, struct otr_msg *);
#if defined(__m68k__)
extern unsigned long getq_D0(void *);
#define getq(q) ((struct otr_msg *)getq_D0((q)))
#else
extern struct otr_msg *getq(void *);
#endif
extern void qenable(void *);
extern void qprocson(void *);
extern void qprocsoff(void *);
extern void flushq(void *, long);
extern int putnextctl(void *, long);
extern int putnextctl1(void *, long, long);
extern int qreply(void *, struct otr_msg *);
#if defined(__m68k__)
extern unsigned long RD_D0(void *);
#define RD(q) ((void *)RD_D0((q)))
#else
extern void *RD(void *);
#endif
#if defined(__m68k__)
extern unsigned long WR_D0(void *);
#define WR(q) ((void *)WR_D0((q)))
#else
extern void *WR(void *);
#endif
#if defined(__m68k__)
extern unsigned long mi_timer_alloc_D0(void *, unsigned long);
#define mi_timer_alloc(q,n) ((struct otr_msg *)mi_timer_alloc_D0((q),(n)))
#else
extern struct otr_msg *mi_timer_alloc(void *, unsigned long);
#endif
extern void mi_timer(void *, unsigned long);
extern int mi_timer_cancel(void *);
extern void mi_timer_free(void *);
extern int mi_timer_valid(void *);
extern struct otr_install *GetOTauxtcpInstallInfo(void);
extern struct otr_install *GetOTauxudpInstallInfo(void);
#endif
