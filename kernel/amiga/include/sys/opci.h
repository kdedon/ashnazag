/*
 * The PCI bus on an Amiga host, for loadable modules: the opci module
 * runs openpci.library (Thomas Richter's bridge driver for Mediator,
 * Prometheus, G-REX and Firestorm boards) and exports these calls.
 * A PCI driver module names it in its Master file: "$depend opci".
 *
 * A device is the library's struct pci_dev, opaque here.  Addresses the
 * calls return (BARs, regions, DMA memory) are kernel virtual addresses
 * inside the bridge's mapped Zorro space.
 *
 * Every call except opci_cfgread/opci_cfgwrite may sleep: process
 * context only.  The two config calls also work from an interrupt
 * handler, but return ~0 / do nothing if the library is busy there.
 */

#ifndef _SYS_OPCI_H
#define _SYS_OPCI_H

/* opci_bus(): the bridges found */
#define	OPCI_MEDIATOR1200	0x01
#define	OPCI_MEDIATORZ4		0x02
#define	OPCI_PROMETHEUS		0x04
#define	OPCI_GREX1200		0x08
#define	OPCI_GREX4000		0x10

#define	OPCI_ANY		0xffff	/* vendor or device wildcard */

/* opci_attr() tags, the prometheus.library ones openpci keeps */
#define	OPCI_VENDOR		0x6eda0000
#define	OPCI_DEVICE		0x6eda0001
#define	OPCI_REVISION		0x6eda0002
#define	OPCI_CLASS		0x6eda0003
#define	OPCI_SUBCLASS		0x6eda0004
#define	OPCI_SLOT		0x6eda0006
#define	OPCI_FUNCTION		0x6eda0007
#define	OPCI_BUSNO		0x6eda0008
#define	OPCI_HEADERTYPE		0x6eda0009
#define	OPCI_SUBSYSVENDOR	0x6eda000a
#define	OPCI_SUBSYSID		0x6eda000b
#define	OPCI_INTERFACE		0x6eda000c
#define	OPCI_INTPIN		0x6eda000d
#define	OPCI_LEGACYIO		0x6eda000e	/* kernel va of IO space */
#define	OPCI_PCITOHOST		0x6eda000f	/* PCI -> kernel va offset */
#define	OPCI_INTLINE		0x6eda001d
#define	OPCI_BARADDR(n)		(0x6eda0010 + (n))	/* n = 0..5 */
#define	OPCI_ROMADDR		0x6eda0016
#define	OPCI_BARSIZE(n)		(0x6eda0020 + (n))
#define	OPCI_ROMSIZE		0x6eda0026
#define	OPCI_BARFLAGS(n)	(0x6eda0030 + (n))

/* opci_dmaalloc() flags */
#define	OPCI_DMA_NOCACHE	0x2

/* config space registers used by most drivers */
#define	OPCI_CFG_VENDOR		0x00
#define	OPCI_CFG_COMMAND	0x04
#define	OPCI_CFG_STATUS		0x06
#define	OPCI_CFG_CLASSREV	0x08
#define	OPCI_CFG_BAR0		0x10
#define	OPCI_CFG_INTLINE	0x3c
#define	OPCI_CMD_IO		0x0001
#define	OPCI_CMD_MEMORY		0x0002
#define	OPCI_CMD_MASTER		0x0004

struct opci_dev;

/*
 * An interrupt handler.  oi_fn(oi_arg) returns nonzero when its device
 * raised the interrupt; it runs at the bridge's Zorro level (2 or 6).
 * The rest is the module's: leave it alone between opci_intr and
 * opci_unintr.
 */
struct opci_intr {
	int		(*oi_call)();	/* first: am_isr calls it */
	int		(*oi_fn)();
	char		*oi_arg;
	struct opci_dev	*oi_dev;
	char		oi_is[24];	/* exec struct Interrupt */
};

extern int		opci_bus();		/* () */
extern struct opci_dev	*opci_find();		/* (prev, vendor, device) */
extern struct opci_dev	*opci_findclass();	/* (prev, class) 24 bits */
extern unsigned long	opci_cfgread();		/* (dev, reg, width 1/2/4) */
extern void		opci_cfgwrite();	/* (dev, reg, width, value) */
extern unsigned long	opci_attr();		/* (dev, tag) */
extern int		opci_obtain();		/* (dev) 0 = yours */
extern void		opci_release();		/* (dev) */
extern int		opci_master();		/* (dev) bus mastering on */
extern char		*opci_region();		/* (dev, pciaddr, size) */
extern void		opci_unregion();	/* (dev, va) */
extern char		*opci_dmaalloc();	/* (dev, size, flags) */
extern void		opci_dmafree();		/* (dev, va, size) */
extern unsigned long	opci_busaddr();		/* (dev, va) as the device sees it */
extern int		opci_intr();		/* (dev, struct opci_intr *) 0 = ok */
extern void		opci_unintr();		/* (struct opci_intr *) */

#endif
