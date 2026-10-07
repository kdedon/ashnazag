/*
 * amxplat.c -- amilib's platform part for the AMIX kernel on an Amiga.
 * Part of the amilib module; opci (and any later library wrapper) use
 * amilib through it.
 *
 * The Amiga-specific services come from the host kernel's platform
 * layer through these weak hooks (absent: the module still loads into
 * a kernel that lacks them, finds no boards and fails with ENXIO):
 *
 *   int  plat_zorro(i, struct amx_zboard *)   board i of the autoconfig
 *                                             list; nonzero past the end
 *   char *plat_iomap(pa, size)                supervisor, cache-inhibited
 *                                             (serialized) kernel mapping
 *   void plat_iounmap(va, size)
 *   unsigned long plat_vtop(va)               physical address of a
 *                                             kernel address
 *   unsigned long plat_pagesize()             the MMU page size (4096)
 *
 * and, for mmu.library's context windows (all three, or none):
 *
 *   int  plat_winrange(&lo, &hi)              a kernel VA range kept
 *                                             unmapped for windows, in
 *                                             [16 MB, 2 GB) and inside one
 *                                             512 MB-aligned block; 0 = ok
 *   int  plat_remap(va, pa, len, mode)        map [va, va+len) to pa
 *                                             (AMX_MAP_IO), or unmap it
 *                                             (AMX_MAP_INVALID); page
 *                                             multiples; any IPL; flushes
 *                                             the ATC; 0 = done
 *   int  plat_faulthook(lo, hi, fn)           a supervisor access fault at
 *                                             va in [lo, hi] calls
 *                                             fn(va, len, write); fn 0 =
 *                                             repaired, retry the access;
 *                                             else the kernel's own fault
 *   void plat_unfaulthook(lo, hi)
 *   int  plat_zintr(intnum, fn)               call fn(intnum) from the
 *                                             level 2 (INTB_PORTS, 3) or
 *                                             level 6 (INTB_EXTER, 13)
 *                                             handler; 0 = done
 *   void plat_zunintr(intnum)
 *
 * From the kernel: kmem_alloc/kmem_free, printf, dlm_cacheflush (DLM),
 * sleep/wakeup, vn_open/vn_rdwr; delayus and hrestime when the kernel
 * has them.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/systm.h"
#include "sys/errno.h"
#include "sys/immu.h"
#include "sys/signal.h"
#include "sys/fs/s5dir.h"
#include "sys/psw.h"
#include "sys/pcb.h"
#include "sys/user.h"
#include "sys/cred.h"
#include "sys/vnode.h"
#include "sys/vfs.h"
#include "sys/uio.h"
#include "sys/file.h"
#include "sys/kmem.h"
#include "amilib.h"

asm(".weak plat_zorro");
asm(".weak plat_iomap");
asm(".weak plat_iounmap");
asm(".weak plat_vtop");
asm(".weak plat_pagesize");
asm(".weak plat_winrange");
asm(".weak plat_remap");
asm(".weak plat_faulthook");
asm(".weak plat_unfaulthook");
asm(".weak plat_zintr");
asm(".weak plat_zunintr");
asm(".weak delayus");
asm(".weak hrestime");
asm(".weak cputype");

extern int plat_zorro(), plat_zintr();
extern char *plat_iomap();
extern void plat_iounmap(), plat_zunintr();
extern unsigned long plat_vtop(), plat_pagesize();
extern int plat_winrange(), plat_remap(), plat_faulthook();
extern void plat_unfaulthook();
extern void delayus();
extern long hrestime[2];		/* timestruc_t: seconds, nanoseconds */
extern long cputype;			/* 30, 40, 60: the 040/060 port */
extern void dlm_cacheflush();

/* through volatile pointers, so no compiler takes a weak symbol for set */
static int (*volatile p_zorro)() = plat_zorro;
static int (*volatile p_zintr)() = plat_zintr;
static char *(*volatile p_iomap)() = plat_iomap;
static void (*volatile p_iounmap)() = plat_iounmap;
static void (*volatile p_zunintr)() = plat_zunintr;
static unsigned long (*volatile p_vtop)() = plat_vtop;
static unsigned long (*volatile p_pagesize)() = plat_pagesize;
static int (*volatile p_winrange)() = plat_winrange;
static int (*volatile p_remap)() = plat_remap;
static int (*volatile p_faulthook)() = plat_faulthook;
static void (*volatile p_unfaulthook)() = plat_unfaulthook;
static void (*volatile p_delayus)() = delayus;
static long *volatile p_hrestime = hrestime;
static long *volatile p_cputype = &cputype;

extern long amilib_loops_us;		/* Space.c: busy loops per microsecond */

static int amx_locked, amx_wanted;
static char amx_noboards;

char *
amx_alloc(size, cansleep)
	unsigned long size;
	int cansleep;
{
	return (char *)kmem_alloc((size_t)size, cansleep ? KM_SLEEP : KM_NOSLEEP);
}

void
amx_free(p, size)
	char *p;
	unsigned long size;
{
	kmem_free((_VOID *)p, (size_t)size);
}

int
amx_spl7()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s & 0xffff;
}

void
amx_splx(s)
	int s;
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}

int
amx_ipl()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s));
	return (s >> 8) & 7;
}

void
amx_delayus(n)
	long n;
{
	volatile long i;

	if (p_delayus) {
		(*p_delayus)((int)n);
		return;
	}
	while (n-- > 0)
		for (i = amilib_loops_us; i > 0; i--)
			;
}

void
amx_time(tv)
	unsigned long *tv;
{
	if (p_hrestime) {
		tv[0] = p_hrestime[0];
		tv[1] = p_hrestime[1] / 1000;
	} else
		tv[0] = tv[1] = 0;
}

void
amx_cacheflush()
{
	dlm_cacheflush();
}

int
amx_attnflags()
{
	long c = p_cputype ? *p_cputype : 30;

	if (c >= 60)
		return AFF_68010 | AFF_68020 | AFF_68030 | AFF_68040 | AFF_68060 |
		    AFF_FPU40;
	if (c >= 40)
		return AFF_68010 | AFF_68020 | AFF_68030 | AFF_68040 | AFF_FPU40;
	return AFF_68010 | AFF_68020 | AFF_68030 | AFF_68881 | AFF_68882;
}

int
amx_zorro(i, zb)
	int i;
	struct amx_zboard *zb;
{
	if (p_zorro == 0 || p_iomap == 0) {
		if (!amx_noboards)
			printf("amilib: this kernel lists no Zorro boards (plat_zorro)\n");
		amx_noboards = 1;
		return -1;
	}
	return (*p_zorro)(i, zb);
}

char *
amx_iomap(pa, size)
	unsigned long pa, size;
{
	return (*p_iomap)(pa, size);
}

void
amx_iounmap(va, size)
	char *va;
	unsigned long size;
{
	if (p_iounmap)
		(*p_iounmap)(va, size);
}

unsigned long
amx_vtop(va)
	char *va;
{
	return p_vtop ? (*p_vtop)(va) : (unsigned long)va;
}

unsigned long
amx_pagesize()
{
	return p_pagesize ? (*p_pagesize)() : 4096;
}

/* the window hooks come as a set */
int
amx_winrange(lop, hip)
	unsigned long *lop, *hip;
{
	if (!p_winrange || !p_remap || !p_faulthook || !p_unfaulthook)
		return -1;
	return (*p_winrange)(lop, hip);
}

int
amx_remap(va, pa, len, mode)
	char *va;
	unsigned long pa, len;
	int mode;
{
	return p_remap ? (*p_remap)(va, pa, len, mode) : -1;
}

int
amx_faulthook(lo, hi, fn)
	unsigned long lo, hi;
	int (*fn)();
{
	return p_faulthook ? (*p_faulthook)(lo, hi, fn) : -1;
}

void
amx_unfaulthook(lo, hi)
	unsigned long lo, hi;
{
	if (p_unfaulthook)
		(*p_unfaulthook)(lo, hi);
}

static void
amx_zint(n)
	int n;
{
	(void)am_intrun(n);
}

int
amx_intattach(n)
	int n;
{
	if (p_zintr == 0)
		return -1;
	return (*p_zintr)(n, amx_zint);
}

void
amx_intdetach(n)
	int n;
{
	if (p_zunintr)
		(*p_zunintr)(n);
}

/* one caller inside amilib's libraries at a time */
int
amx_lock()
{
	int s = amx_spl7();

	while (amx_locked) {
		amx_wanted = 1;
		(void)sleep((caddr_t)&amx_locked, PZERO);
	}
	amx_locked = 1;
	amx_splx(s);
	return 0;
}

int
amx_trylock()
{
	int s = amx_spl7(), e = -1;

	if (!amx_locked) {
		amx_locked = 1;
		e = 0;
	}
	amx_splx(s);
	return e;
}

void
amx_unlock()
{
	int s = amx_spl7();

	amx_locked = 0;
	if (amx_wanted) {
		amx_wanted = 0;
		wakeup((caddr_t)&amx_locked);
	}
	amx_splx(s);
}

void
amx_log(f, a, b, c, d)
	char *f;
	long a, b, c, d;
{
	printf(f, a, b, c, d);
}

/* a whole regular file into kernel memory: process context only */
int
amx_readfile(path, bufp, lenp, max)
	char *path, **bufp;
	unsigned long *lenp, max;
{
	struct vnode *vp;
	struct vattr va;
	char *b;
	unsigned long size;
	int e, resid = 0;

	e = vn_open(path, UIO_SYSSPACE, FREAD, 0, &vp, (enum create)0);
	if (e)
		return e;
	va.va_mask = AT_SIZE;
	e = VOP_GETATTR(vp, &va, 0, u.u_cred);
	if (e == 0 && vp->v_type == VDIR)
		e = AMX_EISDIR;
	else if (e == 0 && (vp->v_type != VREG || va.va_size <= 0 ||
	    va.va_size > max))
		e = EFBIG;
	size = va.va_size;
	if (e == 0) {
		b = (char *)kmem_alloc((size_t)size, KM_SLEEP);
		e = vn_rdwr(UIO_READ, vp, (caddr_t)b, (int)size, (off_t)0,
		    UIO_SYSSPACE, 0, 0x7fffffffL, u.u_cred, &resid);
		if (e == 0 && resid)
			e = EIO;
		if (e)
			kmem_free((_VOID *)b, (size_t)size);
	}
	(void)VOP_CLOSE(vp, FREAD, 1, (off_t)0, u.u_cred);
	VN_RELE(vp);
	if (e)
		return e;
	*bufp = b;
	*lenp = size;
	return 0;
}

void
amx_freefile(b, len)
	char *b;
	unsigned long len;
{
	kmem_free((_VOID *)b, (size_t)len);
}
