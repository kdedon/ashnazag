/*
 * uirom.c -- the ROM object: the user's Mac ROM image file, checked
 * once, mapped copy-on-write into a Mac task by UI_ROM, and the kernel
 * low-memory values the Mac side copies with UI_COPY_OUT, derived from
 * the ROM's ProductInfo table.
 *
 * Accepted: a universal ROM (version word at +8 >= $067C), size from
 * the header long at +$40 matching the file, checksum (the first long:
 * the sum of the words from offset 4) correct.
 *
 * ProductInfo: the ROM finds its record by walking a table of
 * self-relative longs (ending in 0) and matching the word at +$12;
 * the table is the one addressed by the lea before that loop.  The
 * record's high id byte is the box flag.  uinter_boxflag picks the
 * model; -1 takes the Quadra 700 (16) if the ROM has it, else the
 * first record.  The Mac side relocates the record's offsets against
 * UnivInfoPtr, which is therefore the record's address at UI_ROMBASE.
 *
 * K&R C.
 */

#include "uinter.h"
#include "sys/vnode.h"
#include "sys/mman.h"
#include "vm/seg.h"
#include "vm/as.h"

extern int execmap(), as_unmap();
extern int fpu_present;

char	uinter_rom[64] = "/etc/aux/rom";	/* the ROM image file */
int	uinter_boxflag = -1;
struct uirom ui_rom;

#define	CHUNK		8192
#define	LOOPSZ		16

/* movea.l $4(a0)... cmp.w $12(a1),d2 of the ROM's table walk */
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
 * The ProductInfo table: the lea %pc@(d),%a0 just before the walk
 * gives the table less 4.  b holds the first n bytes of the ROM.
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
		if (uinter_boxflag >= 0 ? id == (unsigned)uinter_boxflag :
		    best < 0 || id == 16) {
			best = rec;
			if (uinter_boxflag >= 0 || id == 16)
				break;
		}
	}
	if (best < 0)
		return;
	ui_rom.r_prodoff = best;
	bcopy((char *)b + best, (char *)ui_rom.r_prod, UI_PRODSIZE);
}

/* the ROM image named by uinter_rom, checked; 0 or an errno */
static int
romload1()
{
	struct vnode *vp;
	struct vattr va;
	unsigned char *b = 0, *c = 0;
	unsigned long sum = 0, size;
	long off, n, head = 0x10000;	/* ProductInfo lives in the first 64 KB */
	int e, i;

	if ((e = lookupname(uinter_rom, UIO_SYSSPACE, FOLLOW, NULLVPP, &vp)) != 0)
		return e;
	va.va_mask = AT_SIZE;
	e = EINVAL;
	if (vp->v_type != VREG || VOP_GETATTR(vp, &va, 0, u.u_cred) ||
	    va.va_size < 0x10000 || va.va_size > 0x400000)
		goto bad;
	size = va.va_size;
	b = (unsigned char *)kmem_alloc(head, KM_SLEEP);
	c = (unsigned char *)kmem_alloc(CHUNK, KM_SLEEP);
	for (off = 0; off < size; off += n) {
		n = size - off < CHUNK ? size - off : CHUNK;
		if ((e = rd(vp, off, (char *)c, (int)n)) != 0)
			goto bad;
		if (off < head)
			bcopy((char *)c, (char *)b + off, n);
		for (i = off ? 0 : 4; i + 1 < n; i += 2)
			sum += G16(c + i);
	}
	e = EINVAL;
	ui_rom.r_version = G16(b + 8);
	if (G32(b) != (sum & 0xffffffffL) || ui_rom.r_version < 0x067c ||
	    G32(b + 0x40) != size) {
		printf("uinter: %s: not a usable Mac ROM (sum %x, version %x)\n",
		    uinter_rom, (int)sum, ui_rom.r_version);
		goto bad;
	}
	ui_rom.r_size = size;
	ui_rom.r_sum = sum;
	findprod(b, head);
	ui_rom.r_low = (unsigned char *)kmem_zalloc(UI_LOWSIZE, KM_SLEEP);
	if (ui_rom.r_prodoff >= 0) {
		int hw = G16(ui_rom.r_prod + 0x10) & ~0x1000;

		if (fpu_present)
			hw |= 0x1000;			/* hwCbFPU */
		lowset(0x28eL, (long)G16(ui_rom.r_prod + 0x14), 2);	/* ROM85 */
		lowset(0xb22L, (long)hw, 2);				/* HWCfgFlags */
		lowset(0xdd8L, UI_ROMBASE + ui_rom.r_prodoff, 4);	/* UnivInfoPtr */
		lowset(0xcb3L, (long)(G16(ui_rom.r_prod + 0x12) >> 8), 1); /* BoxFlag */
	}
	lowset(0xd00L, 0x2000L, 2);		/* TimeDBRA */
	lowset(0xd02L, 0x0600L, 2);		/* TimeSCCDB */
	lowset(0x12fL, 4L, 1);			/* CPUFlag: 68040 */
	lowset(0x21eL, 2L, 1);			/* KbdType: ADB extended */
	lowset(0xcb1L, 4L, 1);			/* MMUType: 68040 */
	lowset(0x2aeL, (long)UI_ROMBASE, 4);	/* ROMBase */
	kmem_free((_VOID *)b, head);
	kmem_free((_VOID *)c, CHUNK);
	ui_rom.r_vp = vp;
	printf("uinter: ROM %s, %d KB, version %x, sum %x, ProductInfo %x\n",
	    uinter_rom, (int)(size >> 10), ui_rom.r_version, (int)sum,
	    (int)ui_rom.r_prodoff);
	return 0;
bad:
	if (b)
		kmem_free((_VOID *)b, head);
	if (c)
		kmem_free((_VOID *)c, CHUNK);
	VN_RELE(vp);
	return e;
}

/* one loader at a time: the others wait for its result */
int
ui_romload()
{
	static int busy;
	int e;

	while (busy)
		sleep((caddr_t)&busy, PZERO);
	if (ui_rom.r_vp)
		return 0;
	busy = 1;
	e = romload1();
	busy = 0;
	wakeup((caddr_t)&busy);
	return e;
}

void
ui_romfree()
{
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
 * file, pages shared through the page cache until written.  Mac code
 * writes to ROM space (HSetState on ROM resources) and real ROM
 * ignores that; here a write stays in the task's own copy.
 */
int
ui_rommap(a)
	caddr_t a;
{
	int e;

	if ((e = ui_romload()) != 0)
		return e;
	if (((u_long)a & 0xfff) || !valid_usr_range(a, ui_rom.r_size))
		return EINVAL;
	return execmap(ui_rom.r_vp, a, ui_rom.r_size, 0L, 0L,
	    PROT_READ | PROT_WRITE | PROT_EXEC | PROT_USER);
}

int
ui_romunmap(a)
	caddr_t a;
{
	return as_unmap(u.u_procp->p_as, a, ui_rom.r_size) ? EINVAL : 0;
}
