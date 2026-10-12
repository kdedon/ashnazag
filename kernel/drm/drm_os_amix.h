/*
 * The DRM templates' OS layer on AMIX (SVR4, single CPU, no kernel
 * preemption).  A DRM driver module is one template instance over a
 * card a display backend already set up (struct drm_amixcard).
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/systm.h"
#include "sys/errno.h"
#include "sys/signal.h"
#include "sys/cred.h"
#include "sys/proc.h"
#include "sys/user.h"
#include "sys/file.h"
#include "sys/fcntl.h"
#include "sys/vnode.h"
#include "sys/uio.h"
#include "sys/kmem.h"
#include "sys/mman.h"
#include "sys/poll.h"
#include "sys/ioccom.h"
#include "ds.h"
#include "sys/opci.h"

#define __REALLY_HAVE_AGP	0
#define __REALLY_HAVE_MTRR	0
#define __REALLY_HAVE_SG	(__HAVE_SG)

#undef DRM_DEBUG
#define DRM_TIME_SLICE		(HZ / 20)
#define DRM_DEV_MODE		0660
#define DRM_DEV_UID		0
#define DRM_DEV_GID		0

#define DRM_CURPROC		(u.u_procp)
#define DRM_STRUCTPROC		struct proc
#define DRM_CURRENTPID		(u.u_procp->p_pid)
#define DRM_SPINTYPE		int
#define DRM_SPININIT(l, name)	((l) = 0)
#define DRM_SPINUNINIT(l)
#define DRM_SPINLOCK(l)
#define DRM_SPINUNLOCK(u)
/* nothing between these sleeps, and interrupts leave the lists alone */
#define DRM_LOCK
#define DRM_UNLOCK

#define DRM_IOCTL_ARGS		dev_t kdev, u_long cmd, caddr_t data, int flags, DRM_STRUCTPROC *p
#define DRM_SUSER(p)		drv_priv((p)->p_cred)
#define DRM_IRQ_ARGS		char *arg
#define DRM_DEVICE		drm_device_t *dev = &DRM(softc)[getminor(kdev)]
#define DRM_MALLOC(size)	DRM(amix_malloc)(size)
#define DRM_FREE(pt)		DRM(amix_mfree)(pt)
#define DRM_VTOPHYS(addr)	vtophys(addr)
#define DRM_READ8(addr)		(*(volatile unsigned char *)(addr))
#define DRM_WRITE8(addr, val)	(*(volatile unsigned char *)(addr) = (val))
/* the bridges pass PCI bytes address-invariant: a long is byte reversed */
#define DRM_READ32(addr)	DRM(swab32)(*(volatile u32 *)(addr))
#define DRM_WRITE32(addr, val)	(*(volatile u32 *)(addr) = DRM(swab32)(val))
#define DRM_ERR(v)		v
#define DRM_PRIV							\
	drm_file_t *priv = (drm_file_t *)DRM(find_file_by_proc)(dev, p); \
	if (!priv) {							\
		DRM_DEBUG("can't find authenticator\n");		\
		return EINVAL;						\
	}
#define DRM_UDELAY(us)		drv_usecwait(us)
#define DRM_GETSAREA()							\
do {									\
	drm_map_list_entry_t *listentry;				\
	TAILQ_FOREACH(listentry, dev->maplist, link) {			\
		drm_map_t *map = listentry->map;			\
		if (map->type == _DRM_SHM &&				\
		    map->flags & _DRM_CONTAINS_LOCK) {			\
			dev_priv->sarea = map;				\
			break;						\
		}							\
	}								\
} while (0)
#define DRM_HZ			HZ
#define DRM_WAIT_ON(ret, queue, timeout, condition)			\
while (!(condition)) {							\
	ret = DRM(tsleep)((caddr_t)&(queue), (timeout));		\
	if (ret)							\
		return ret;						\
}
#define DRM_WAKEUP(queue)	wakeup((caddr_t)(queue))
#define DRM_WAKEUP_INT(queue)	wakeup((caddr_t)(queue))
#define DRM_INIT_WAITQUEUE(queue) do {} while (0)

#define IOCPARM_LEN(cmd)	((int)((cmd) >> 16) & IOCPARM_MASK)
#define tsleep(chan, pri, wmesg, timo)	DRM(tsleep)((caddr_t)(chan), timo)
#define vtophys(va)		vtop((caddr_t)(va), (proc_t *)0)
#define DRM_BUSADDR(va)		opci_busaddr(DRM(card).c_dev, (char *)(va))
#define pmap_mapdev(pa, size)	DRM(amix_mapdev)(pa, size)
#define pmap_unmapdev(va, size)	do {} while (0)
#define p_svuid			cr_suid

#define DRM_COPY_TO_USER_IOCTL(user, kern, size)	\
	if (IOCPARM_LEN(cmd) != size)			\
		return EINVAL;				\
	*user = kern;
#define DRM_COPY_FROM_USER_IOCTL(kern, user, size)	\
	if (IOCPARM_LEN(cmd) != size)			\
		return EINVAL;				\
	kern = *user;
#define DRM_COPY_TO_USER(user, kern, size)	copyout((caddr_t)(kern), (caddr_t)(user), size)
#define DRM_COPY_FROM_USER(kern, user, size)	copyin((caddr_t)(user), (caddr_t)(kern), size)
#define DRM_VERIFYAREA_READ(uaddr, size)	0
#define DRM_COPY_FROM_USER_UNCHECKED(kern, user, size) \
	copyin((caddr_t)(user), (caddr_t)(kern), size)
#define DRM_GET_USER_UNCHECKED(val, uaddr)	DRM(fuword)(&(val), (caddr_t)(uaddr))
#define DRM_READMEMORYBARRIER()		__asm__ __volatile__("nop" : : : "memory")
#define DRM_WRITEMEMORYBARRIER()	__asm__ __volatile__("nop" : : : "memory")

/* the MMU's page, 4 KB on the 68040 and 68060 whatever the headers say */
#undef PAGE_SIZE
#undef PAGE_SHIFT
#undef PAGE_MASK
#define PAGE_SHIFT		DRM(pgshift)
#define PAGE_SIZE		(1 << PAGE_SHIFT)
#define PAGE_MASK		(PAGE_SIZE - 1)
#define PAGE_ALIGN(a)		(((a) + PAGE_MASK) & ~PAGE_MASK)
#define atop(a)			((unsigned long)(a) >> PAGE_SHIFT)
#define ptoa(n)			((unsigned long)(n) << PAGE_SHIFT)
#define round_page(a)		PAGE_ALIGN(a)

#define FREAD_OK		FREAD
#ifndef FASYNC
#define FASYNC			0x1000
#endif
#define FIONBIO_DRM		0x7f7f0001	/* never matches */
#define FIOASYNC_DRM		0x7f7f0002

typedef struct drm_chipinfo {
	int	vendor;
	int	device;
	int	supported;
	char	*name;
} drm_chipinfo_t;

typedef unsigned long	u32;
typedef unsigned short	u16;
typedef unsigned char	u8;
typedef u32		dma_addr_t;
typedef u32		atomic_t;
typedef u32		cycles_t;
typedef u32		spinlock_t;
typedef int		d_ioctl_t(DRM_IOCTL_ARGS);

#define atomic_set(p, v)	(*(p) = (v))
#define atomic_read(p)		(*(p))
#define atomic_inc(p)		(++*(p))
#define atomic_dec(p)		(--*(p))
#define atomic_add(n, p)	(*(p) += (n))
#define atomic_sub(n, p)	(*(p) -= (n))

static __inline__ int
DRM(splhi)(void)
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0\n\tor.w &0x700,%%sr" : "=d" (s) : : "memory");
	return s;
}

static __inline__ void
DRM(splx)(int s)
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}

#define spldrm()		DRM(splhi)()
#define splx(s)			DRM(splx)(s)

static __inline__ atomic_t
test_and_set_bit(int b, volatile void *p)
{
	int s = DRM(splhi)();
	volatile int *w = (volatile int *)p + (b >> 5);
	unsigned int m = 1 << (b & 0x1f), r = *w & m;

	*w |= m;
	DRM(splx)(s);
	return r;
}

static __inline__ void
clear_bit(int b, volatile void *p)
{
	int s = DRM(splhi)();

	((volatile int *)p)[b >> 5] &= ~(1 << (b & 0x1f));
	DRM(splx)(s);
}

static __inline__ void
set_bit(int b, volatile void *p)
{
	int s = DRM(splhi)();

	((volatile int *)p)[b >> 5] |= 1 << (b & 0x1f);
	DRM(splx)(s);
}

static __inline__ int
test_bit(int b, volatile void *p)
{
	return ((volatile int *)p)[b >> 5] & (1 << (b & 0x1f));
}

static __inline__ int
find_first_zero_bit(volatile void *p, int max)
{
	int b;

	for (b = 0; b < max; b++)
		if (!test_bit(b, p))
			return b;
	return max;
}

static __inline__ u32
DRM(swab32)(u32 v)
{
	return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24;
}

#define cpu_to_le32(x)		DRM(swab32)(x)
#define le32_to_cpu(x)		DRM(swab32)(x)

/*
 * Clients map shared memory uncached; the kernel's view is copyback.
 * Push and invalidate the data cache around the kernel's accesses.
 */
#define DRM_SAREA_SYNC()	__asm__ __volatile__(".word 0xf478" : : : "memory")

static __inline__ unsigned int
DRM(cmpxchg)(volatile unsigned int *p, unsigned int old, unsigned int new)
{
	int s = DRM(splhi)();
	unsigned int prev;

	DRM_SAREA_SYNC();
	prev = *p;
	if (prev == old)
		*p = new;
	DRM_SAREA_SYNC();
	DRM(splx)(s);
	return prev;
}
#define cmpxchg(p, o, n)	DRM(cmpxchg)((volatile unsigned int *)(p), o, n)
/* __ret: nonzero when *lock was not old */
#define _DRM_CAS(lock, old, new, __ret) \
	((__ret) = DRM(cmpxchg)((volatile unsigned int *)(lock), old, new) != (old))

static __inline__ int
atomic_cmpset_int(volatile unsigned int *p, unsigned int old, unsigned int new)
{
	int s = DRM(splhi)(), r = *p == old;

	if (r)
		*p = new;
	DRM(splx)(s);
	return r;
}

/* the system's prototypes take caddr_t */
#define bzero(p, n)		bzero((caddr_t)(p), n)
#define wakeup(c)		wakeup((caddr_t)(c))
#define strncpy(d, s, n)	strncpy(d, (char *)(s), n)
#define memset(p, v, s)		bzero((caddr_t)(p), s)
#define memcpy(d, s, n)		bcopy((caddr_t)(s), (caddr_t)(d), n)
#define sprintf			DRM(sprintf)

/* BSD tail queues */
#define TAILQ_HEAD(name, type)						\
struct name {								\
	struct type *tqh_first;						\
	struct type **tqh_last;						\
}
#define TAILQ_ENTRY(type)						\
struct {								\
	struct type *tqe_next;						\
	struct type **tqe_prev;						\
}
#define TAILQ_FIRST(head)	((head)->tqh_first)
#define TAILQ_NEXT(elm, field)	((elm)->field.tqe_next)
#define TAILQ_INIT(head) do {						\
	(head)->tqh_first = NULL;					\
	(head)->tqh_last = &(head)->tqh_first;				\
} while (0)
#define TAILQ_FOREACH(var, head, field)					\
	for ((var) = TAILQ_FIRST(head); (var); (var) = TAILQ_NEXT(var, field))
#define TAILQ_INSERT_TAIL(head, elm, field) do {			\
	(elm)->field.tqe_next = NULL;					\
	(elm)->field.tqe_prev = (head)->tqh_last;			\
	*(head)->tqh_last = (elm);					\
	(head)->tqh_last = &(elm)->field.tqe_next;			\
} while (0)
#define TAILQ_REMOVE(head, elm, field) do {				\
	if (((elm)->field.tqe_next) != NULL)				\
		(elm)->field.tqe_next->field.tqe_prev =			\
		    (elm)->field.tqe_prev;				\
	else								\
		(head)->tqh_last = (elm)->field.tqe_prev;		\
	*(elm)->field.tqe_prev = (elm)->field.tqe_next;			\
} while (0)

struct callout { int c_unused; };
#define callout_init(c)		do {} while (0)
#define callout_stop(c)		do {} while (0)

#define wait_queue_head_t	int
#define agp_memory		void
#define jiffies			lbolt

#define DRM_ERROR(fmt, arg...) \
	printf("[" DRM_NAME ":%s] *ERROR* " fmt, __FUNCTION__ , ## arg)
#define DRM_MEM_ERROR(area, fmt, arg...) \
	printf("[" DRM_NAME ":%s:%s] *ERROR* " fmt, \
	    __FUNCTION__, DRM(mem_stats)[area].name , ## arg)
#define DRM_INFO(fmt, arg...)	printf("[" DRM_NAME "] " fmt , ## arg)
#define DRM_DEBUG(fmt, arg...)	do { } while (0)

#define DRM_FIND_MAP(dest, o)						\
	do {								\
		drm_map_list_entry_t *listentry;			\
		TAILQ_FOREACH(listentry, dev->maplist, link) {		\
			if (listentry->map->offset == o) {		\
				dest = listentry->map;			\
				break;					\
			}						\
		}							\
	} while (0)

#include "drmcard.h"

extern int printf(), drv_priv(), copyin(), copyout(), sleep(), timeout(), untimeout();
extern int uiomove(), getf();
extern void bcopy(), drv_usecwait();
extern int strlen();
extern char *strcpy();
extern time_t lbolt;
extern struct dssess *ds_front;

/* drm_drv.h and the glue */
extern d_ioctl_t	DRM(ioctl);
extern d_ioctl_t	DRM(lock);
extern d_ioctl_t	DRM(unlock);
extern d_ioctl_t	DRM(dsbind);
extern int		DRM(open)(dev_t kdev, int flags, int fmt, DRM_STRUCTPROC *p);
extern int		DRM(close)(dev_t kdev, int flags, int fmt, DRM_STRUCTPROC *p);
extern int		DRM(mmap)(dev_t kdev, off_t offset, int prot);
extern int		DRM(open_helper)(dev_t kdev, int flags, int fmt,
			    DRM_STRUCTPROC *p, drm_device_t *dev);
extern drm_file_t	*DRM(find_file_by_proc)(drm_device_t *dev, DRM_STRUCTPROC *p);
extern int		DRM(tsleep)(caddr_t chan, int timo);
extern void		*DRM(amix_malloc)(unsigned long size);
extern void		DRM(amix_mfree)(void *pt);
extern void		*DRM(alloc_pages)(unsigned long size);
extern void		DRM(free_pages)(void *pt, unsigned long size);
extern int		DRM(fuword)(void *val, caddr_t uaddr);
extern int		DRM(sprintf)(char *buf, const char *fmt, ...);
extern void		*DRM(amix_mapdev)(unsigned long pa, unsigned long size);
extern int		DRM(pgshift);
extern drm_device_t	DRM(softc)[];
extern int		DRM(sysctl_init)(drm_device_t *dev);
extern int		DRM(sysctl_cleanup)(drm_device_t *dev);
extern struct drm_amixcard DRM(card);

/* drm_ioctl.h */
extern d_ioctl_t	DRM(irq_busid);
extern d_ioctl_t	DRM(getunique);
extern d_ioctl_t	DRM(setunique);
extern d_ioctl_t	DRM(getmap);
extern d_ioctl_t	DRM(getclient);
extern d_ioctl_t	DRM(getstats);
/* drm_context.h */
extern d_ioctl_t	DRM(resctx);
extern d_ioctl_t	DRM(addctx);
extern d_ioctl_t	DRM(modctx);
extern d_ioctl_t	DRM(getctx);
extern d_ioctl_t	DRM(switchctx);
extern d_ioctl_t	DRM(newctx);
extern d_ioctl_t	DRM(rmctx);
extern d_ioctl_t	DRM(setsareactx);
extern d_ioctl_t	DRM(getsareactx);
/* drm_drawable.h */
extern d_ioctl_t	DRM(adddraw);
extern d_ioctl_t	DRM(rmdraw);
/* drm_auth.h */
extern d_ioctl_t	DRM(getmagic);
extern d_ioctl_t	DRM(authmagic);
/* drm_lock.h */
extern d_ioctl_t	DRM(block);
extern d_ioctl_t	DRM(unblock);
extern d_ioctl_t	DRM(finish);
/* drm_bufs.h */
extern d_ioctl_t	DRM(addmap);
extern d_ioctl_t	DRM(rmmap);
#if __HAVE_DMA
extern d_ioctl_t	DRM(addbufs_agp);
extern d_ioctl_t	DRM(addbufs_pci);
extern d_ioctl_t	DRM(addbufs_sg);
extern d_ioctl_t	DRM(addbufs);
extern d_ioctl_t	DRM(infobufs);
extern d_ioctl_t	DRM(markbufs);
extern d_ioctl_t	DRM(freebufs);
extern d_ioctl_t	DRM(mapbufs);
extern d_ioctl_t	DRM(control);
#endif
#if __HAVE_VBL_IRQ
extern d_ioctl_t	DRM(wait_vblank);
#endif
#if __HAVE_SG
extern d_ioctl_t	DRM(sg_alloc);
extern d_ioctl_t	DRM(sg_free);
#endif
