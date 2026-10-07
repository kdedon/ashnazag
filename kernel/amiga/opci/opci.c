/*
 * opci.c -- the PCI bus for Unix modules (<sys/opci.h>), through
 * openpci.library running on amilib.
 *
 * opci_init reads the library's load file from the configuration
 * directory (/etc/conf/pci/openpci.library; it is not ours to ship),
 * loads it as LoadSeg would, initialises its Resident (which finds the bridges and
 * configures the bus), opens it, and lists what it found.  Every call
 * below is one LVO of the library, made under one lock: the library
 * expects AmigaOS's tasks and semaphores, and here only one caller at a
 * time is inside it.
 *
 * Interrupts: the library hooks its bridge's Zorro interrupt chain with
 * AddIntServer; amilib routes that chain to the host (amx_intattach),
 * and the host's level 2 / level 6 handler runs it with am_intrun.
 */

#include "amilib.h"
#include "sys/opci.h"

#define	OPCI_ENOEXEC	8
#define	OPCI_ENXIO	6
#define	OPCI_ENOMEM	12
#define	OPCI_EBUSY	16

/* openpci.library LVOs (pragmas/openpci_pragmas.h) */
#define	PCI_BUS			(-30)
#define	PCI_FIND_CLASS		(-96)
#define	PCI_READ_BYTE		(-108)
#define	PCI_READ_WORD		(-114)
#define	PCI_READ_LONG		(-120)
#define	PCI_WRITE_BYTE		(-126)
#define	PCI_WRITE_WORD		(-132)
#define	PCI_WRITE_LONG		(-138)
#define	PCI_SET_MASTER		(-144)
#define	PCI_ADD_INTSERVER	(-150)
#define	PCI_REM_INTSERVER	(-156)
#define	PCI_LOGIC_TO_PHYSIC	(-174)
#define	PCI_OBTAIN_CARD		(-186)
#define	PCI_RELEASE_CARD	(-192)
#define	FINDBOARDA		(-204)
#define	GETBOARDATTRSA		(-210)
#define	ALLOCDMAFORBOARD	(-222)
#define	RELEASEDMAFORBOARD	(-228)
#define	OBTAINPCIREGION		(-246)
#define	RELEASEPCIREGION	(-252)

static char *opci_base;			/* the open library */
static char *opci_lib;			/* its base from InitResident */
static unsigned long opci_seg;
static int opci_flags;

static void
rclear(r)
	unsigned long *r;
{
	int i;

	for (i = 0; i < 16; i++)
		r[i] = 0;
}

static long
lcall(lvo, r)
	int lvo;
	unsigned long *r;
{
	return am_lvo(opci_base, lvo, r);
}

static int opci_start();
static void opci_list();

static int
opci_isr(oi)
	struct opci_intr *oi;
{
	return (*oi->oi_fn)(oi->oi_arg);
}

/* ------------------------------------------------------- the calls */

int
opci_bus()
{
	return opci_flags;
}

struct opci_dev *
opci_find(prev, vendor, device)
	struct opci_dev *prev;
	int vendor, device;
{
	unsigned long r[16], tags[6];
	int n = 0;

	if (vendor != OPCI_ANY) {
		tags[n++] = OPCI_VENDOR;
		tags[n++] = vendor & 0xffff;
	}
	if (device != OPCI_ANY) {
		tags[n++] = OPCI_DEVICE;
		tags[n++] = device & 0xffff;
	}
	tags[n] = TAG_DONE;
	rclear(r);
	r[A0] = (unsigned long)prev;
	r[A1] = (unsigned long)tags;
	if (amx_lock())
		return 0;
	(void)lcall(FINDBOARDA, r);
	amx_unlock();
	return (struct opci_dev *)r[D0];
}

struct opci_dev *
opci_findclass(prev, class)
	struct opci_dev *prev;
	unsigned long class;
{
	unsigned long r[16];

	rclear(r);
	r[D0] = class & 0xffffff;
	r[A0] = (unsigned long)prev;
	if (amx_lock())
		return 0;
	(void)lcall(PCI_FIND_CLASS, r);
	amx_unlock();
	return (struct opci_dev *)r[D0];
}

/* the two calls an interrupt handler may make: no sleeping for the lock */
static int
cfglock()
{
	return amx_ipl() ? amx_trylock() : amx_lock();
}

unsigned long
opci_cfgread(dev, reg, width)
	struct opci_dev *dev;
	int reg, width;
{
	unsigned long r[16];

	rclear(r);
	r[D0] = reg & 0xff;
	r[A0] = (unsigned long)dev;
	if (cfglock())
		return ~0UL;
	(void)lcall(width == 1 ? PCI_READ_BYTE : width == 2 ? PCI_READ_WORD :
	    PCI_READ_LONG, r);
	amx_unlock();
	return width == 1 ? r[D0] & 0xff : width == 2 ? r[D0] & 0xffff : r[D0];
}

void
opci_cfgwrite(dev, reg, width, v)
	struct opci_dev *dev;
	int reg, width;
	unsigned long v;
{
	unsigned long r[16];

	rclear(r);
	r[D0] = reg & 0xff;
	r[D1] = v;
	r[A0] = (unsigned long)dev;
	if (cfglock())
		return;
	(void)lcall(width == 1 ? PCI_WRITE_BYTE : width == 2 ? PCI_WRITE_WORD :
	    PCI_WRITE_LONG, r);
	amx_unlock();
}

unsigned long
opci_attr(dev, tag)
	struct opci_dev *dev;
	unsigned long tag;
{
	unsigned long r[16], tags[3], v = 0;

	tags[0] = tag;
	tags[1] = (unsigned long)&v;
	tags[2] = TAG_DONE;
	rclear(r);
	r[A0] = (unsigned long)dev;
	r[A1] = (unsigned long)tags;
	if (amx_lock())
		return 0;
	(void)lcall(GETBOARDATTRSA, r);
	amx_unlock();
	return v;
}

int
opci_obtain(dev)
	struct opci_dev *dev;
{
	unsigned long r[16];

	rclear(r);
	r[A0] = (unsigned long)dev;
	if (amx_lock())
		return OPCI_EBUSY;
	(void)lcall(PCI_OBTAIN_CARD, r);
	amx_unlock();
	return (r[D0] & 0xffff) ? 0 : OPCI_EBUSY;
}

void
opci_release(dev)
	struct opci_dev *dev;
{
	unsigned long r[16];

	rclear(r);
	r[A0] = (unsigned long)dev;
	if (amx_lock())
		return;
	(void)lcall(PCI_RELEASE_CARD, r);
	amx_unlock();
}

int
opci_master(dev)
	struct opci_dev *dev;
{
	unsigned long r[16];

	rclear(r);
	r[A0] = (unsigned long)dev;
	if (amx_lock())
		return 0;
	(void)lcall(PCI_SET_MASTER, r);
	amx_unlock();
	return (r[D0] & 0xffff) != 0;
}

char *
opci_region(dev, pciaddr, size)
	struct opci_dev *dev;
	unsigned long pciaddr, size;
{
	unsigned long r[16];

	rclear(r);
	r[A0] = (unsigned long)dev;
	r[A1] = pciaddr;
	r[D0] = size;
	if (amx_lock())
		return 0;
	(void)lcall(OBTAINPCIREGION, r);
	amx_unlock();
	return (char *)r[D0];
}

void
opci_unregion(dev, va)
	struct opci_dev *dev;
	char *va;
{
	unsigned long r[16];

	rclear(r);
	r[A0] = (unsigned long)dev;
	r[A1] = (unsigned long)va;
	if (amx_lock())
		return;
	(void)lcall(RELEASEPCIREGION, r);
	amx_unlock();
}

char *
opci_dmaalloc(dev, size, flags)
	struct opci_dev *dev;
	unsigned long size, flags;
{
	unsigned long r[16];

	rclear(r);
	r[A0] = (unsigned long)dev;
	r[D0] = size;
	r[D1] = flags | 1;			/* MEM_PCI, always implied */
	if (amx_lock())
		return 0;
	(void)lcall(ALLOCDMAFORBOARD, r);
	amx_unlock();
	return (char *)r[D0];
}

void
opci_dmafree(dev, va, size)
	struct opci_dev *dev;
	char *va;
	unsigned long size;
{
	unsigned long r[16];

	rclear(r);
	r[A0] = (unsigned long)dev;
	r[A1] = (unsigned long)va;
	r[D0] = size;
	if (amx_lock())
		return;
	(void)lcall(RELEASEDMAFORBOARD, r);
	amx_unlock();
}

/*
 * The library knows two kinds of 68K address: inside a bridge's window,
 * where it gave out the address itself (our kernel va), and main memory,
 * which it takes as physical (no mmu.library here).  Give it each in
 * the form it expects.
 */
unsigned long
opci_busaddr(dev, va)
	struct opci_dev *dev;
	char *va;
{
	unsigned long r[16];

	rclear(r);
	r[A0] = am_inboard(va) ? (unsigned long)va : amx_vtop(va);
	r[A1] = (unsigned long)dev;
	if (amx_lock())
		return 0;
	(void)lcall(PCI_LOGIC_TO_PHYSIC, r);
	amx_unlock();
	return r[D0];
}

int
opci_intr(dev, oi)
	struct opci_dev *dev;
	struct opci_intr *oi;
{
	unsigned long r[16];
	char *is = oi->oi_is;
	int i;

	for (i = 0; i < sizeof oi->oi_is; i++)
		is[i] = 0;
	oi->oi_call = opci_isr;
	oi->oi_dev = dev;
	AB(is, LN_TYPE) = NT_INTERRUPT;
	AP(is, LN_NAME) = "opci";
	AP(is, IS_DATA) = (char *)oi;
	AP(is, IS_CODE) = (char *)am_isr;
	rclear(r);
	r[A0] = (unsigned long)is;
	r[A1] = (unsigned long)dev;
	if (amx_lock())
		return OPCI_EBUSY;
	(void)lcall(PCI_ADD_INTSERVER, r);
	amx_unlock();
	return (r[D0] & 0xffff) ? 0 : OPCI_ENXIO;
}

void
opci_unintr(oi)
	struct opci_intr *oi;
{
	unsigned long r[16];

	rclear(r);
	r[A0] = (unsigned long)oi->oi_is;
	r[A1] = (unsigned long)oi->oi_dev;
	if (amx_lock())
		return;
	(void)lcall(PCI_REM_INTSERVER, r);
	amx_unlock();
}

/* ------------------------------------------------- start and stop */

static void
opci_list()
{
	struct opci_dev *d = 0;
	unsigned long v;

	while ((d = opci_find(d, OPCI_ANY, OPCI_ANY)) != 0) {
		v = opci_cfgread(d, OPCI_CFG_CLASSREV, 4);
		amx_log("opci: %d:%d.%d %x", (long)opci_attr(d, OPCI_BUSNO),
		    (long)opci_attr(d, OPCI_SLOT), (long)opci_attr(d, OPCI_FUNCTION),
		    (long)(v >> 8));
		amx_log(" %x:%x irq %d bar0 %x\n", (long)opci_attr(d, OPCI_VENDOR),
		    (long)opci_attr(d, OPCI_DEVICE), (long)opci_attr(d, OPCI_INTLINE),
		    (long)opci_attr(d, OPCI_BARADDR(0)));
	}
}

int
opci_init(dir)
	char *dir;
{
	char *buf, path[96];
	unsigned long len;
	int e, i, j;

	if (opci_base)
		return OPCI_EBUSY;
	for (i = 0; dir[i] && i < sizeof am_confdir - 1 &&
	    i < sizeof path - 20; i++)
		am_confdir[i] = path[i] = dir[i];
	am_confdir[i] = 0;
	for (j = 0; "/openpci.library"[j]; j++)
		path[i + j] = "/openpci.library"[j];
	path[i + j] = 0;
	if ((e = amx_readfile(path, &buf, &len, 1024L * 1024)) != 0) {
		amx_log("opci: %s: error %d\n", (long)path, (long)e, 0L, 0L);
		return e;
	}
	if (amx_lock()) {
		amx_freefile(buf, len);
		return OPCI_EBUSY;
	}
	e = opci_start(buf, len);
	amx_unlock();
	amx_freefile(buf, len);		/* the hunks are copied out */
	if (e == 0)
		opci_list();
	return e;
}

static int
opci_start(buf, len)
	char *buf;
	unsigned long len;
{
	char *res;
	unsigned long r[16];
	int e;

	if (am_init(amx_attnflags()) != 0)
		return OPCI_ENOMEM;
	opci_seg = am_loadseg(buf, len, &e);
	if (opci_seg == 0) {
		amx_log("opci: openpci.library: %s\n", (long)(e == AMH_ENOMEM ?
		    "out of memory" : "not an AmigaOS load file"), 0L, 0L, 0L);
		am_fini();
		return e == AMH_ENOMEM ? OPCI_ENOMEM : OPCI_ENOEXEC;
	}
	res = am_findres(opci_seg);
	if (res == 0 || AB(res, RT_TYPE) != NT_LIBRARY ||
	    am_stricmp(AP(res, RT_NAME), "openpci.library", -1L) != 0) {
		amx_log("opci: openpci.library has no library Resident\n", 0L, 0L,
		    0L, 0L);
		goto fail;
	}
	amx_log("opci: %s", (long)AP(res, RT_IDSTRING), 0L, 0L, 0L);
	opci_lib = am_initres(res, opci_seg);
	if (opci_lib == 0) {
		amx_log("opci: openpci.library did not initialise\n", 0L, 0L, 0L,
		    0L);
		goto fail;
	}
	opci_base = am_openlib("openpci.library", 0L);
	if (opci_base == 0) {
		amx_log("opci: openpci.library did not open\n", 0L, 0L, 0L, 0L);
		goto fail;
	}
	rclear(r);
	(void)lcall(PCI_BUS, r);
	opci_flags = r[D0] & 0xffff;
	amx_log("opci: bridges %x\n", (long)opci_flags, 0L, 0L, 0L);
	return 0;
fail:
	if (opci_lib) {			/* let it release what it can */
		rclear(r);
		(void)am_lvo(opci_lib, LVO_EXPUNGE, r);
	}
	am_fini();			/* its interrupt chains go here */
	am_unloadseg(opci_seg);
	opci_base = opci_lib = 0;
	opci_seg = 0;
	return OPCI_ENXIO;
}

/*
 * Close and expunge the library; it gives back its segment list, then
 * amilib goes.  An expunge it refuses keeps everything (EBUSY).
 */
int
opci_fini()
{
	unsigned long r[16], seg;

	if (opci_base == 0)
		return 0;
	if (amx_lock())
		return OPCI_EBUSY;
	am_closelib(opci_base);
	rclear(r);
	seg = am_lvo(opci_lib, LVO_EXPUNGE, r);
	if (seg == 0) {
		opci_base = am_openlib("openpci.library", 0L);
		amx_unlock();
		amx_log("opci: openpci.library refused to expunge\n", 0L, 0L, 0L,
		    0L);
		return OPCI_EBUSY;
	}
	am_unloadseg(seg);
	opci_base = opci_lib = 0;
	opci_seg = 0;
	opci_flags = 0;
	am_fini();
	amx_unlock();
	return 0;
}
