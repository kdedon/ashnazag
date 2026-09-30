/*
 * RAM disk, block (major 20) and raw (major RD_CMAJ) interfaces.
 *
 *	minor 0	root image: boot-record BI_RAMDISK, else the linked-in image
 *	minor 1	swap, carved from the top of the kernel's memory chunk
 *		when the RAM disk is root
 *
 * mac_rd_config() runs from config(): MMU off, BSS not yet cleared,
 * so everything it touches is initialised data.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/errno.h"
#include "sys/buf.h"
#include "sys/uio.h"
#include "sys/open.h"
#include "sys/cred.h"
#include "sys/conf.h"
#include "vm/bootconf.h"

#define RD_BMAJ		20
#ifndef RD_CMAJ
#define RD_CMAJ		42
#endif
#ifndef RD_SWAPKB
#define RD_SWAPKB	2048
#endif
#ifndef RD_SWAPMAXKB
#define RD_SWAPMAXKB	32768
#endif
#define RD_NUNIT	2
#define RD_ROOT		0
#define RD_SWAP		1

#define RD_BSIZE	512
#define RD_BSHIFT	9
#define RD_CHUNK	2048		/* no page is smaller */
#define RD_ALIGN	0x40000		/* keep VSIZOFMEM on 256 KB */
#define RD_MINMEM	0x400000	/* memory left to VM above end */
#define RD_MAXPA	0x40000000	/* identity-mapped limit */
#define RD_MAGIC	0x52614D44	/* 'RaMD' */

#define BI_LAST		0x0000
#define BI_RAMDISK	0x0006

struct rdunit {
	caddr_t		base;
	unsigned long	nblk;
};

/* nonzero initialisers keep these out of BSS */
unsigned long rd_magic = 1;
struct rdunit rd_unit[RD_NUNIT] = { { (caddr_t)1, 1 }, { (caddr_t)1, 1 } };
char rd_swapname[] = "/dev/swap";

extern char rd_image[], rd_image_end[];
extern char end[];
extern unsigned char mac_bi[];
extern unsigned long mac_bilen;
extern unsigned long MAINSTORE, VSIZOFMEM;
extern dev_t rootdev, dumpdev;
extern char rootfstype[];
extern struct bootobj swapfile;
extern int cdevcnt;
extern int nodev();
extern paddr_t vtop();
extern void mac_puts(), mac_puthex();

int ramopen(), ramclose(), ramread(), ramwrite();
void ramstrategy();

static void
rd_default()
{
	rd_unit[RD_ROOT].base = rd_image;
	rd_unit[RD_ROOT].nblk = (rd_image_end - rd_image) >> RD_BSHIFT;
	rd_unit[RD_SWAP].base = 0;
	rd_unit[RD_SWAP].nblk = 0;
	rd_magic = RD_MAGIC;
}

/* BI_RAMDISK {addr, size} from the boot record copy */
static int
rd_bootinfo(basep, sizep)
unsigned long *basep, *sizep;
{
	register unsigned long n;
	unsigned short tag, size;
	unsigned long *p;

	for (n = 0; n + 4 <= mac_bilen; n += size) {
		tag = *(unsigned short *)(mac_bi + n);
		size = *(unsigned short *)(mac_bi + n + 2);
		if (tag == BI_LAST || size < 4)
			break;
		if (tag == BI_RAMDISK && size >= 12) {
			p = (unsigned long *)(mac_bi + n + 4);
			*basep = p[0];
			*sizep = p[1];
			return 1;
		}
	}
	return 0;
}

static void
rd_strcpy(d, s)
register char *d, *s;
{
	while ((*d++ = *s++) != 0)
		;
}

static void
rd_putkv(k, v)
char *k;
unsigned long v;
{
	mac_puts(k);
	mac_puthex(v);
}

/*
 * Pick the root image, reserve its memory, install the raw entry.  With
 * wantroot and a nonempty image, also carve the swap unit and point
 * rootdev, dumpdev, rootfstype and swapfile at the RAM disk.  Returns 1
 * when the RAM disk is root.
 */
int
mac_rd_config(wantroot)
int wantroot;
{
	register struct cdevsw *cp;
	unsigned long base, size, kend, lim, sw;

	rd_default();
	kend = ((unsigned long)end + RD_ALIGN - 1) & ~(RD_ALIGN - 1);
	lim = MAINSTORE + VSIZOFMEM;

	if (rd_bootinfo(&base, &size)) {
		rd_putkv("ramdisk: boot record ", base);
		rd_putkv(" size ", size);
		/* above the kernel, below 1 GB, and clear of the boot-time tables */
		if (base >= kend && size >= RD_BSIZE && size <= RD_MAXPA &&
		    base <= RD_MAXPA - size &&
		    (base >= lim || base - kend >= RD_MINMEM)) {
			if (base < lim)
				VSIZOFMEM = (base & ~(RD_ALIGN - 1)) - MAINSTORE;
			rd_unit[RD_ROOT].base = (caddr_t)base;
			rd_unit[RD_ROOT].nblk = size >> RD_BSHIFT;
		} else
			mac_puts(" (unusable)");
		mac_puts("\n");
	}

	cp = &cdevsw[RD_CMAJ];
	if (RD_CMAJ < cdevcnt && cp->d_open == nodev && cp->d_str == 0) {
		cp->d_open = ramopen;
		cp->d_close = ramclose;
		cp->d_read = ramread;
		cp->d_write = ramwrite;
	}

	rd_putkv("ramdisk: image ", (unsigned long)rd_unit[RD_ROOT].base);
	rd_putkv(" blocks ", rd_unit[RD_ROOT].nblk);
	if (!wantroot || rd_unit[RD_ROOT].nblk == 0) {
		mac_puts(" (not root)\n");
		return 0;
	}

	/*
	 * swap: half the memory above RD_MINMEM, within RD_SWAPKB..RD_SWAPMAXKB,
	 * else what is left (at least RD_ALIGN).  Anonymous memory must fit in
	 * swap as well as in memory, so swap caps heaps and stacks; half keeps
	 * the two limits about equal.
	 */
	lim = MAINSTORE + VSIZOFMEM;
	sw = lim > kend + RD_MINMEM ? ((lim - kend - RD_MINMEM) / 2) & ~(RD_ALIGN - 1) : 0;
	if (sw > RD_SWAPMAXKB * 1024)
		sw = RD_SWAPMAXKB * 1024;
	if (sw < RD_SWAPKB * 1024)
		sw = RD_SWAPKB * 1024;
	if (lim < kend + RD_MINMEM + sw)
		sw = lim > kend + RD_MINMEM ?
		    (lim - kend - RD_MINMEM) & ~(RD_ALIGN - 1) : 0;
	if (sw >= RD_ALIGN) {
		VSIZOFMEM -= sw;
		rd_unit[RD_SWAP].base = (caddr_t)(MAINSTORE + VSIZOFMEM);
		rd_unit[RD_SWAP].nblk = sw >> RD_BSHIFT;
	} else
		mac_puts(" (no memory for swap)");

	rootdev = makedevice(RD_BMAJ, RD_ROOT);
	dumpdev = makedevice(RD_BMAJ, RD_SWAP);
	rd_strcpy(rootfstype, "s5");
	rd_strcpy(swapfile.bo_name, rd_swapname);
	swapfile.bo_offset = 0;
	swapfile.bo_size = rd_unit[RD_SWAP].nblk;

	rd_putkv(" swap ", (unsigned long)rd_unit[RD_SWAP].base);
	rd_putkv(" blocks ", rd_unit[RD_SWAP].nblk);
	mac_puts("\n");
	return 1;
}

int
ramopen(devp, flag, otyp, cr)
dev_t *devp;
int flag, otyp;
struct cred *cr;
{
	unsigned int m = getminor(*devp);

	if (rd_magic != RD_MAGIC)
		rd_default();
	if (m >= RD_NUNIT || rd_unit[m].nblk == 0)
		return ENXIO;
	return 0;
}

int
ramclose(dev, flag, otyp, cr)
dev_t dev;
int flag, otyp;
struct cred *cr;
{
	return 0;
}

/*
 * Copy one request.  The buffer address is virtual (kernel or b_proc);
 * translate it a chunk at a time since pages need not be contiguous.
 */
void
ramstrategy(bp)
register struct buf *bp;
{
	register struct rdunit *up;
	register unsigned long n, cc;
	unsigned int m = getminor(bp->b_edev);
	caddr_t va, disk;
	paddr_t pa;

	bp->b_resid = bp->b_bcount;
	if (m >= RD_NUNIT || (up = &rd_unit[m])->nblk == 0 ||
	    bp->b_blkno < 0 || bp->b_blkno > up->nblk) {
		bp->b_error = ENXIO;
		bp->b_flags |= B_ERROR;
		iodone(bp);
		return;
	}
	n = (up->nblk - bp->b_blkno) << RD_BSHIFT;
	if (n == 0 && (bp->b_flags & B_READ) == 0) {
		bp->b_error = ENXIO;
		bp->b_flags |= B_ERROR;
		iodone(bp);
		return;
	}
	if (n > bp->b_bcount)
		n = bp->b_bcount;

	disk = up->base + (bp->b_blkno << RD_BSHIFT);
	va = bp->b_un.b_addr;
	while (n > 0) {
		cc = RD_CHUNK - ((unsigned long)va & (RD_CHUNK - 1));
		if (cc > n)
			cc = n;
		pa = vtop(va, bp->b_proc);
		if (pa == 0) {
			bp->b_error = EFAULT;
			bp->b_flags |= B_ERROR;
			break;
		}
		if (bp->b_flags & B_READ)
			bcopy(disk, (caddr_t)pa, cc);
		else
			bcopy((caddr_t)pa, disk, cc);
		va += cc;
		disk += cc;
		n -= cc;
		bp->b_resid -= cc;
	}
	iodone(bp);
}

int
ramread(dev, uiop, cr)
dev_t dev;
struct uio *uiop;
struct cred *cr;
{
	return uiophysio(ramstrategy, (struct buf *)0, dev, B_READ, uiop);
}

int
ramwrite(dev, uiop, cr)
dev_t dev;
struct uio *uiop;
struct cred *cr;
{
	return uiophysio(ramstrategy, (struct buf *)0, dev, B_WRITE, uiop);
}

void
ramprint(dev, s)
dev_t dev;
char *s;
{
	printf("rd%d: %s\n", getminor(dev), s);
}

int
ramsize(dev)
dev_t dev;
{
	unsigned int m = getminor(dev);

	if (rd_magic != RD_MAGIC)
		rd_default();
	if (m >= RD_NUNIT)
		return -1;
	return rd_unit[m].nblk;
}
