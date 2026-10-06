/*
 * uirom.c -- the ROM object: a Mac host's own ROM, else the user's Mac
 * ROM image file, checked once, mapped into a Mac task by UI_ROM as a
 * private copy, and the kernel
 * low-memory values the Mac side copies with UI_COPY_OUT, derived from
 * the ROM's ProductInfo table.
 *
 * The machine's ROM is used unless uinter_rom names a file or the ROM
 * fails the checks; then the file, by default /etc/aux/rom.
 * Accepted: a universal ROM (version word at +8 >= $067C), size from
 * the header long at +$40 matching the image, checksum (the first long:
 * the sum of the words from offset 4) correct.
 *
 * ProductInfo: the record for the model is chosen by its id.  The
 * record's high id byte is the box flag.  The model: uinter_boxflag,
 * else the Gestalt machine ID in uinter_model's file, else pickbox()
 * from the host and the ROM; failing all, the Quadra 700 (16) if the
 * ROM has it, else the first record.  A model without a record
 * of its own (the Quadra 800, 29, found by machine ID on real
 * hardware) gets that record and its own box flag, which the Mac side
 * keeps for Gestalt.  The Mac side relocates the
 * record's offsets against UnivInfoPtr, which is therefore the
 * record's address at UI_ROMBASE.
 *
 * K&R C.
 */

#include "uinter.h"
#include "sys/vnode.h"
#include "sys/mman.h"
#include "vm/seg.h"
#include "vm/as.h"

extern int execmap(), as_map(), as_unmap(), segvn_create();
extern caddr_t zfod_argsp;
extern int fpu_present;
__asm__(".weak cputype\n.weak mac_model\n.weak mac_romva");
extern long ui_cputype __asm__("cputype");	/* 40, 60; absent: 68020/030 */
extern unsigned long mac_model;		/* a Mac host's Gestalt machine ID */
extern unsigned long mac_romva();	/* a Mac host's ROM in kernel space */
/* read at run time: the compiler takes a declared symbol's address as nonzero */
static unsigned long (*volatile romva)() = mac_romva;
static long *volatile cpup = &ui_cputype;
static unsigned long *volatile modelp = &mac_model;

char	uinter_rom[64] = "";		/* a ROM image file to use instead */
#define	ROMFILE	"/etc/aux/rom"		/* the file without a usable host ROM */
unsigned char *ui_hwrom;		/* the host's ROM in use, else 0 */
int	uinter_boxflag = -1;
char	uinter_model[64] = "/etc/aux/model";	/* Gestalt ID override */
static int ui_box;		/* uinter_boxflag for this load */
static int busy, inuse;		/* loading; users of ui_rom */
struct uirom ui_rom;

#define	CHUNK		8192
#define	LOOPSZ		16

static unsigned char walk[LOOPSZ] = {
	0x58, 0x48, 0x20, 0x10, 0x67, 0x26, 0x43, 0xf0,
	0x08, 0x00, 0xb4, 0x69, 0x00, 0x12, 0x66, 0xf0
};

static int
rd(vp, off, b, n)
	struct vnode *vp;
	long off;
	char *b;
	int n;
{
	int resid = 0;

	if (vn_rdwr(UIO_READ, vp, (caddr_t)b, n, (off_t)off, UIO_SYSSPACE, 0,
	    0x7fffffffL, u.u_cred, &resid) || resid)
		return EIO;
	return 0;
}

static void
lowset(a, v, n)
	long a, v;
	int n;
{
	unsigned char *p = ui_rom.r_low + a;

	if (n == 4)
		P32(p, v);
	else if (n == 2)
		P16(p, v);
	else
		P8(p, v);
}

/*
 * Find the ProductInfo table.  b holds the first n bytes of the ROM.
 */
static void
findprod(b, n)
	unsigned char *b;
	long n;
{
	long i, t, e, off, rec, best = -1;
	unsigned id;

	ui_rom.r_prodoff = -1;
	for (i = 4; i + LOOPSZ <= n; i += 2)
		if (bcmp((char *)b + i, (char *)walk, LOOPSZ) == 0 &&
		    G16(b + i - 4) == 0x41fa)
			break;
	if (i + LOOPSZ > n)
		return;
	t = i - 2 + (short)G16(b + i - 2) + 4;
	for (e = t; e >= 0 && e + 4 <= n && (off = (long)G32(b + e)) != 0; e += 4) {
		if (off < -e || off > n - UI_PRODSIZE - e)
			continue;		/* not in the part read */
		rec = e + off;
		id = G16(b + rec + 0x12) >> 8;
		if (ui_box >= 0 && id == (unsigned)ui_box) {
			best = rec;
			break;
		}
		/* else the first record, or the first Quadra 700 one */
		if (best < 0 || (id == 16 && G16(b + best + 0x12) >> 8 != 16))
			best = rec;
	}
	if (best < 0)
		return;
	ui_rom.r_prodoff = best;
	bcopy((char *)b + best, (char *)ui_rom.r_prod, UI_PRODSIZE);
}

/*
 * The box flag for a ROM (by checksum) on this host, -1 for the
 * default record.  A Quadra host keeps its own model with a Quadra
 * ROM; other 68040 hosts get the ROM's Quadra, 68030 hosts its 68030
 * Mac.
 */
static int
pickbox(sum)
	unsigned long sum;
{
	int cpu = cpup ? (int)*cpup : 30;
	int host = modelp ? (int)*modelp : 0;

	switch (sum) {
	case 0x420dbff3:		/* Quadra 700, 900, 950 */
	case 0xf1acad13:		/* Quadra 800, 610, 650, Centris */
		if (cpu < 40)
			break;
		switch (host) {
		case 20: case 22: case 26:	/* Quadra 900, 700, 950 */
		case 35: case 36: case 43:	/* Quadra 800, 650, Centris 650 */
		case 52: case 53:		/* Centris 610, Quadra 610 */
			return host - 6;
		}
		return sum == 0xf1acad13 ? 29 : 16;
	case 0x368cadfe:		/* IIci */
		return 5;
	default:			/* the host's record, if the ROM has one */
		return ui_hwrom && host ? host - 6 : -1;
	}
	return -1;
}

/* the Gestalt ID in uinter_model's file as a box flag, else -1 */
static int
modelfile()
{
	struct vnode *vp;
	char b[8];
	int i, n = 0, resid = 0;

	if (lookupname(uinter_model, UIO_SYSSPACE, FOLLOW, NULLVPP, &vp))
		return -1;
	if (vp->v_type == VREG && vn_rdwr(UIO_READ, vp, (caddr_t)b, (int)sizeof b - 1,
	    (off_t)0, UIO_SYSSPACE, 0, 0x7fffffffL, u.u_cred, &resid) == 0)
		for (i = 0; i < (int)sizeof b - 1 - resid && b[i] >= '0' && b[i] <= '9'; i++)
			n = n * 10 + b[i] - '0';
	VN_RELE(vp);
	return n > 6 && n < 262 ? n - 6 : -1;
}

/* a usable image: universal, the size its header says, sum correct */
static int
romok(b, size, sum, name)
	unsigned char *b;
	unsigned long size, sum;
	char *name;
{
	ui_rom.r_version = G16(b + 8);
	if (size && G32(b) == (sum & 0xffffffffL) && ui_rom.r_version >= 0x067c &&
	    G32(b + 0x40) == size) {
		ui_rom.r_size = size;
		ui_rom.r_sum = sum;
		return 1;
	}
	printf("uinter: %s: not a usable Mac ROM (sum %x, version %x)\n",
	    name, (int)sum, ui_rom.r_version);
	return 0;
}

/* the host's own ROM, checked, else 0 */
static unsigned char *
hwrom()
{
	unsigned char *b;
	unsigned long size, sum = 0, i;

	if (romva == 0 || (b = (unsigned char *)(*romva)(0x10000L)) == 0)
		return 0;
	size = G32(b + 0x40);
	if (G16(b + 8) < 0x067c || size < 0x10000 || (*romva)(size) == 0)
		size = 0;
	for (i = 4; i + 1 < size; i += 2)
		sum += G16(b + i);
	return romok(b, size, sum, "ROMBase") ? b : 0;
}

/* the ROM image file, checked, its first head bytes in b; 0 or an errno */
static int
romfile(name, b, head)
	char *name;
	unsigned char *b;
	long head;
{
	struct vnode *vp;
	struct vattr va;
	unsigned char *c;
	unsigned long sum = 0, size;
	long off, n;
	int e, i;

	if ((e = lookupname(name, UIO_SYSSPACE, FOLLOW, NULLVPP, &vp)) != 0)
		return e;
	va.va_mask = AT_SIZE;
	if (vp->v_type != VREG || VOP_GETATTR(vp, &va, 0, u.u_cred) ||
	    va.va_size < 0x10000 || va.va_size > 0x400000) {
		VN_RELE(vp);
		return EINVAL;
	}
	size = va.va_size;
	c = (unsigned char *)kmem_alloc(CHUNK, KM_SLEEP);
	for (off = 0; off < size; off += n) {
		n = size - off < CHUNK ? size - off : CHUNK;
		if ((e = rd(vp, off, (char *)c, (int)n)) != 0)
			break;
		if (off < head)
			bcopy((char *)c, (char *)b + off, n);
		for (i = off ? 0 : 4; i + 1 < n; i += 2)
			sum += G16(c + i);
	}
	kmem_free((_VOID *)c, CHUNK);
	if (e == 0 && !romok(b, size, sum, name))
		e = EINVAL;
	if (e) {
		VN_RELE(vp);
	} else
		ui_rom.r_vp = vp;
	return e;
}

/* the host's ROM or the file, checked, and its low memory; 0 or an errno */
static int
romload1()
{
	unsigned char *b = 0;
	char *name = "ROMBase";
	long head = 0x10000;	/* ProductInfo lives in the first 64 KB */
	int e = 0, i;

	if (uinter_rom[0] || (ui_hwrom = hwrom()) == 0) {
		name = uinter_rom[0] ? uinter_rom : ROMFILE;
		b = (unsigned char *)kmem_alloc(head, KM_SLEEP);
		if ((e = romfile(name, b, head)) != 0) {
			kmem_free((_VOID *)b, head);
			return e;
		}
	}
	if (ui_box < 0)
		ui_box = pickbox(ui_rom.r_sum);
	findprod(b ? b : ui_hwrom, head);
	ui_rom.r_low = (unsigned char *)kmem_zalloc(UI_LOWSIZE, KM_SLEEP);
	if (ui_rom.r_prodoff >= 0) {
		int hw = G16(ui_rom.r_prod + 0x10) & ~0x1000;

		if (fpu_present)
			hw |= 0x1000;			/* hwCbFPU */
		lowset(0x28eL, (long)G16(ui_rom.r_prod + 0x14), 2);	/* ROM85 */
		lowset(0xb22L, (long)hw, 2);				/* HWCfgFlags */
		lowset(0xdd8L, UI_ROMBASE + ui_rom.r_prodoff, 4);	/* UnivInfoPtr */
		lowset(0xcb3L, ui_box >= 0 ? (long)ui_box :
		    (long)(G16(ui_rom.r_prod + 0x12) >> 8), 1);		/* BoxFlag */
	}
	lowset(0xd00L, 0x2000L, 2);		/* TimeDBRA */
	lowset(0xd02L, 0x0600L, 2);		/* TimeSCCDB */
	i = cpup && *cpup >= 40;
	lowset(0x12fL, i ? 4L : 3L, 1);		/* CPUFlag: 68040, 68030 */
	lowset(0x21eL, 2L, 1);			/* KbdType: ADB extended */
	lowset(0xcb1L, i ? 4L : 3L, 1);		/* MMUType: 68040, 68030 */
	lowset(0x2aeL, (long)UI_ROMBASE, 4);	/* ROMBase */
	if (b)
		kmem_free((_VOID *)b, head);
	printf("uinter: ROM %s, %d KB, version %x, sum %x, ProductInfo %x, box %d\n",
	    name, (int)(ui_rom.r_size >> 10), ui_rom.r_version, (int)ui_rom.r_sum,
	    (int)ui_rom.r_prodoff, (int)ui_rom.r_low[0xcb3]);
	return 0;
}

/* one loader at a time: the others wait for its result.  A changed
 * uinter_boxflag reads it again once its users are done.  Each
 * successful call is paired with ui_romrel. */
int
ui_romload()
{
	int e, box;

	while (busy)
		sleep((caddr_t)&busy, PZERO);
	box = uinter_boxflag >= 0 ? uinter_boxflag : modelfile();
	if ((ui_rom.r_vp || ui_hwrom) && ui_rom.r_box == box) {
		inuse++;
		return 0;
	}
	busy = 1;
	while (inuse)
		sleep((caddr_t)&inuse, PZERO);
	ui_romfree();
	ui_box = box > 255 ? -1 : box;	/* BoxFlag is a byte */
	e = romload1();
	ui_rom.r_box = box;
	if (e == 0)
		inuse++;
	busy = 0;
	wakeup((caddr_t)&busy);
	return e;
}

void
ui_romrel()
{
	if (--inuse == 0)
		wakeup((caddr_t)&inuse);
}

int
ui_rombusy()
{
	return busy || inuse;
}

void
ui_romfree()
{
	ui_hwrom = 0;
	if (ui_rom.r_vp) {
		VN_RELE(ui_rom.r_vp);
		ui_rom.r_vp = 0;
	}
	if (ui_rom.r_low) {
		kmem_free((_VOID *)ui_rom.r_low, UI_LOWSIZE);
		ui_rom.r_low = 0;
	}
}

/*
 * The ROM at user address a in the caller: a private mapping of the
 * file, pages shared through the page cache until written, or a copy
 * of the host's ROM in zero-fill memory.  Mac code writes to ROM space
 * (HSetState on ROM resources) and real ROM ignores that; here a write
 * stays in the task's own copy.
 */
int
ui_rommap(a)
	caddr_t a;
{
	int e;

	if ((e = ui_romload()) != 0)
		return e;
	if (((u_long)a & 0xfff) || !valid_usr_range(a, ui_rom.r_size))
		e = EINVAL;
	else if (ui_hwrom) {
		e = as_map(u.u_procp->p_as, a, (u_int)ui_rom.r_size, segvn_create,
		    zfod_argsp);
		if (e == 0 && copyout((caddr_t)ui_hwrom, a, ui_rom.r_size)) {
			(void)as_unmap(u.u_procp->p_as, a, ui_rom.r_size);
			e = EFAULT;
		}
	} else
		e = execmap(ui_rom.r_vp, a, ui_rom.r_size, 0L, 0L,
		    PROT_READ | PROT_WRITE | PROT_EXEC | PROT_USER);
	/*
	 * The Process Manager runs on a stack at 0x3fffff00, heap end
	 * 0x3fff0000, while it disposes of a process: zero-fill memory.
	 */
	if (e == 0)
		(void)as_map(u.u_procp->p_as, (caddr_t)UI_PMSTACK, (u_int)UI_PMSTKSZ,
		    segvn_create, zfod_argsp);
	ui_romrel();
	return e;
}

int
ui_romunmap(a)
	caddr_t a;
{
	if (!ui_rom.r_vp && !ui_hwrom)
		return EINVAL;
	return as_unmap(u.u_procp->p_as, a, ui_rom.r_size) ? EINVAL : 0;
}
