/*
 * startlisa -- run Lisa OS from the user's volume as this process.
 *
 *	startlisa [-d volume] [-p parameters] [-r rom] [-v]
 *
 * Boots the user's Lisa ROM ($NATIVEHOST_ROOT/lisa.rom) from their ProFile
 * volume (profile.dc42, DC42 or raw 532-byte blocks, opened read only;
 * written blocks go to profile.writes).  Routines of the ROM copy and the
 * loaded code that drive hardware are found by structure and patched to
 * call into this process (host calls); the Lisa's chips are not modelled.
 * A child process shows the video page on the display.  Parameter memory
 * is kept in a file, by default parameter-memory in the environment's
 * folder.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include "lisaio.h"
#include "nativehost.h"

extern char etext;
extern void _start();

#define	PHYS	0x20000000UL		/* the launcher's view of Lisa RAM */
#define	ROMVA	0x20400000UL
#define	RAMSZ	LISA_RAMMAX
#define	SYSAREA	0x8000UL		/* physical base of logical 0: the ROM keeps the first 32 KB */
#define	SCRW	720
#define	SCRH	364
#define	SCRB	(SCRW / 8 * SCRH)
#define	BLK	532			/* a volume block: 20 tag bytes, 512 data */

static char *disk;			/* the ProFile image, never written */
static char *pmfile;			/* parameter memory, 64 bytes */
static unsigned char pm[64];
static int verbose;
static unsigned long serial = 5L << 16 | 60100;	/* the machine's serial number, 005-60100 */
static int lfd;				/* /dev/lisa */

static void
nothing(s)
	int s;
{
}

static void
die(what)
	char *what;
{
	fprintf(stderr, "startlisa: %s: %s\n", what, strerror(errno));
	exit(1);
}

/* a file in the environment's folder */
static char *
envpath(root, name)
	char *root, *name;
{
	char *p = malloc(strlen(root) + strlen(name) + 2);

	if (!p)
		die("memory");
	sprintf(p, "%s/%s", root, name);
	return p;
}

#define	G16P(p)	((p)[0] << 8 | (p)[1])

static void
put32(p, v)
	unsigned char *p;
	unsigned long v;
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static int
pread(fd, buf, n, off)
	int fd, n;
	void *buf;
	long off;
{
	if (lseek(fd, off, 0) < 0)
		return -1;
	return read(fd, buf, n);
}

static int
pwrite(fd, buf, n, off)
	int fd, n;
	void *buf;
	long off;
{
	if (lseek(fd, off, 0) < 0)
		return -1;
	return write(fd, buf, n);
}

/* ---- the volume ---- */

static int vfd;
static long vblocks, vtags, vtagsz;	/* DC42: tags at vtags, vtagsz each; else raw */
static unsigned long vreads;
static unsigned char *vtag;		/* every block's tag */
static int ofd;				/* the blocks written: block number, then the block */
static long *ovat;			/* each block's place in it, or -1 */
static long ovend;
static unsigned char *fbuf;		/* the largest file, for patching */
static long fbufsz;

static int
vopen()
{
	unsigned char h[84];
	struct stat sb;
	long b;

	if ((vfd = open(disk, O_RDONLY)) < 0)
		return -1;
	if (fstat(vfd, &sb) < 0 || pread(vfd, h, 84, 0L) != 84)
		return -1;
	b = (long)h[64] << 24 | h[65] << 16 | h[66] << 8 | h[67];
	vtags = (long)h[68] << 24 | h[69] << 16 | h[70] << 8 | h[71];
	if (h[82] == 1 && h[83] == 0 && h[0] <= 63 && b >= 512 && b % 512 == 0 && 84 + b + vtags <= sb.st_size) {
		vblocks = b / 512;
		vtagsz = vtags / vblocks;
		vtags = 84 + b;
	} else if (sb.st_size >= BLK && sb.st_size % BLK == 0) {
		vblocks = sb.st_size / BLK;
		vtags = 0;
	} else {
		errno = EINVAL;
		return -1;
	}
	if ((vtag = calloc(vblocks, 20)) == 0 || (ovat = malloc(vblocks * sizeof *ovat)) == 0)
		return -1;
	for (b = 0; b < vblocks; b++) {
		ovat[b] = -1;
		if (vtags ? vtagsz && pread(vfd, vtag + b * 20, vtagsz > 20 ? 20 : vtagsz, vtags + b * vtagsz) < 0 :
		    pread(vfd, vtag + b * 20, 20, b * BLK) != 20)
			return -1;
	}
	return 0;
}

/* the overlay of blocks written, kept in the environment's folder */
static int
oopen(path)
	char *path;
{
	unsigned char r[4 + 20];
	long b;

	if ((ofd = open(path, O_RDWR | O_CREAT, 0644)) < 0)
		return -1;
	for (ovend = 0; pread(ofd, r, sizeof r, ovend) == sizeof r; ovend += 4 + BLK) {
		b = (long)r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3];
		if (b < 0 || b >= vblocks)
			break;
		ovat[b] = ovend + 4;
		memcpy(vtag + b * 20, r + 4, 20);
	}
	return 0;
}

/* a buffer for the largest file on the volume, by tag */
static int
fbufinit()
{
	long *lo, *hi;
	unsigned char *t;
	long b, n;
	int f;

	if ((lo = malloc(0x8000 * sizeof *lo)) == 0 || (hi = malloc(0x8000 * sizeof *hi)) == 0)
		return -1;
	for (f = 0; f < 0x8000; f++)
		lo[f] = 0x7fff, hi[f] = -1;
	for (b = 0; b < vblocks; b++) {
		t = vtag + b * 20;
		if ((f = (short)(t[4] << 8 | t[5])) > 0) {
			n = t[12] << 8 | t[13];
			lo[f] = n < lo[f] ? n : lo[f];
			hi[f] = n > hi[f] ? n : hi[f];
		}
	}
	for (f = 1; f < 0x8000; f++)
		if (hi[f] >= lo[f] && (hi[f] - lo[f] + 1) * 512L > fbufsz)
			fbufsz = (hi[f] - lo[f] + 1) * 512L;
	free(lo);
	free(hi);
	return fbufsz && (fbuf = malloc(fbufsz)) == 0 ? -1 : 0;
}

/* block b as 20 tag bytes then 512 data; -1 none */
static int
vread(b, p)
	long b;
	unsigned char *p;
{
	if (b < 0 || b >= vblocks)
		return -1;
	if (verbose && ++vreads % 256 == 0)
		fprintf(stderr, "lisa: %lu blocks read, the last %ld\n", vreads, b);
	if (ovat[b] >= 0)
		return pread(ofd, p, BLK, ovat[b]) == BLK ? 0 : -1;
	memcpy(p, vtag + b * 20, 20);
	return pread(vfd, p + 20, 512, vtags ? 84 + b * 512 : b * BLK + 20) == 512 ? 0 : -1;
}

static int
vwrite(b, p)
	long b;
	unsigned char *p;
{
	unsigned char n[4];

	if (b < 0 || b >= vblocks)
		return -1;
	if (ovat[b] < 0) {
		put32(n, (unsigned long)b);
		if (pwrite(ofd, p, BLK, ovend + 4) != BLK || pwrite(ofd, n, 4, ovend) != 4)
			return -1;
		ovat[b] = ovend + 4;
		ovend += 4 + BLK;
	} else if (pwrite(ofd, p, BLK, ovat[b]) != BLK)
		return -1;
	memcpy(vtag + b * 20, p, 20);
	return 0;
}

/* guest memory at a logical address, through the module */
static int
gmem(a, p, n, fl, blk)
	unsigned long a, blk;
	unsigned char *p;
	int n, fl;
{
	struct lisamem m;

	m.lm_addr = a & 0xffffff;
	m.lm_len = n;
	m.lm_flags = fl;
	m.lm_blk = blk;
	if (fl & LM_WRITE)
		memcpy(m.lm_data, p, n);
	if (ioctl(lfd, LISAIOC_MEM, &m) < 0)
		return -1;
	if (!(fl & LM_WRITE))
		memcpy(p, m.lm_data, n);
	return 0;
}

/* the guest's variables, n bytes big-endian; through the module, so an unmapped page never faults here */
static unsigned long
gget(a, n)
	unsigned long a;
	int n;
{
	unsigned char b[4];
	unsigned long v = 0;
	int i;

	if (gmem(a, b, n, 0, 0L) < 0)
		return 0;
	for (i = 0; i < n; i++)
		v = v << 8 | b[i];
	return v;
}

static void
gput(a, v, n)
	unsigned long a, v;
	int n;
{
	unsigned char b[4];
	int i;

	for (i = n - 1; i >= 0; i--, v >>= 8)
		b[i] = v;
	gmem(a, b, n, LM_WRITE, 0L);
}

#define	G8(a)		gget((unsigned long)(a), 1)
#define	G16(a)		gget((unsigned long)(a), 2)
#define	G32(a)		gget((unsigned long)(a), 4)
#define	P8(a, v)	gput((unsigned long)(a), (unsigned long)(v), 1)
#define	P16(a, v)	gput((unsigned long)(a), (unsigned long)(v), 2)
#define	P32(a, v)	gput((unsigned long)(a), (unsigned long)(v), 4)

/*
 * Host calls: patched guest code does `jsr hc_x` into this process's text.
 * The entry saves the guest's registers on its stack, moves to a host
 * stack and calls hostcall() with them:
 *
 *	r[0..7] d0-d7, r[8..14] a0-a6, r[15] the call, r[16] its return,
 *	r[17..] the guest's stack before the jsr
 *
 * Interrupts wait until the call returns.  Registers the service changes
 * in r[] go back to the guest; hc_fdin also sets the flags from D0.
 */
unsigned long hc_stack[2048], *hc_sp = hc_stack + 2048;
extern void hc_read(), hc_pfid(), hc_xfer(), hc_mmu(), hc_seg1(), hc_serial(), hc_irqon(), hc_irq1(),
    hc_clock(), hc_video(), hc_pmrd(), hc_pmwr(), hc_input(), hc_unpack(), hc_take(), hc_ctx(), hc_fdtype(), hc_fdin(),
    hc_reset(), hc_stop(), hc_vid0();

__asm__("\n\
	.text\n\
	.even\n\
hc_read:\n\
	move.l &0,-(%sp)\n\
	bra.s hc_common\n\
hc_pfid:\n\
	move.l &1,-(%sp)\n\
	bra.s hc_common\n\
hc_xfer:\n\
	move.l &2,-(%sp)\n\
	bra.s hc_common\n\
hc_mmu:\n\
	move.l &3,-(%sp)\n\
	bra.s hc_common\n\
hc_seg1:\n\
	move.l &4,-(%sp)\n\
	bra.s hc_common\n\
hc_serial:\n\
	move.l &5,-(%sp)\n\
	bra.s hc_common\n\
hc_irqon:\n\
	move.l &6,-(%sp)\n\
	bra.s hc_common\n\
hc_irq1:\n\
	move.l &7,-(%sp)\n\
	bra.s hc_common\n\
hc_clock:\n\
	move.l &8,-(%sp)\n\
	bra.s hc_common\n\
hc_video:\n\
	move.l &9,-(%sp)\n\
	bra.s hc_common\n\
hc_pmrd:\n\
	move.l &10,-(%sp)\n\
	bra.s hc_common\n\
hc_pmwr:\n\
	move.l &11,-(%sp)\n\
	bra.s hc_common\n\
hc_input:\n\
	move.l &12,-(%sp)\n\
	bra.s hc_common\n\
hc_unpack:\n\
	move.l &13,-(%sp)\n\
	bra.s hc_common\n\
hc_take:\n\
	move.l &14,-(%sp)\n\
	bra.s hc_common\n\
hc_ctx:\n\
	move.l &15,-(%sp)\n\
hc_common:\n\
	movem.l %d0-%d7/%a0-%a6,-(%sp)\n\
	move.l %sp,%a0\n\
	move.l hc_sp,%sp\n\
	move.l %a0,-(%sp)\n\
	jsr hostcall\n\
	move.l (%sp),%sp\n\
	movem.l (%sp)+,%d0-%d7/%a0-%a6\n\
	addq.l &4,%sp\n\
	rts\n\
hc_fdtype:\n\
	move.l &16,-(%sp)\n\
	bra.s hc_common\n\
hc_fdin:\n\
	jsr hc_fdin1\n\
	tst.b %d0\n\
	rts\n\
hc_fdin1:\n\
	move.l &17,-(%sp)\n\
	bra.s hc_common\n\
hc_reset:\n\
	move.l &18,-(%sp)\n\
	bra.s hc_common\n\
hc_stop:\n\
	move.l &19,-(%sp)\n\
	bra.s hc_common\n\
hc_vid0:\n\
	move.l &20,-(%sp)\n\
	bra.s hc_common\n");

/* ---- patches to files as their blocks are read ---- */

struct fpat {
	short	fid, page;		/* the block: file and page */
	short	off;			/* in its data */
	unsigned char old, val;
};

static struct fpat pat[1024];
static int npat;
static short seen[128];
static int nseen;
static int full;			/* the last file the table had no room for */

static void
addpat(fid, page0, f, off, p, n)
	int fid, page0, off, n;
	unsigned char *f, *p;
{
	int i;

	for (i = 0; i < n && npat < sizeof pat / sizeof pat[0]; i++, npat++) {
		pat[npat].fid = fid;
		pat[npat].page = page0 + (off + i) / 512;
		pat[npat].off = (off + i) % 512;
		pat[npat].old = f[off + i];
		pat[npat].val = p[i];
	}
	if (i < n)
		fprintf(stderr, "lisa: patch table full: file %d keeps %d bytes unpatched\n", fid, n - i);
}

/* the only place in f[0..n) holding sig (len k), -1 none or several */
static long
find1(f, n, sig, k)
	unsigned char *f, *sig;
	long n;
	int k;
{
	long a, at = -1;

	for (a = 0; a + k <= n; a += 2)
		if (!memcmp(f + a, sig, k)) {
			if (at >= 0)
				return -1;
			at = a;
		}
	return at;
}

/* the driver's interleave of the low four bits of a block number, by drive type */
static unsigned char ilv[2][16];
static int ilvok;
static unsigned long mousedx;		/* the keyboard decoder's variable for the horizontal delta */

/*
 * The ProFile driver steps through the drive's handshake on the parallel
 * port, one step per interrupt.  Three routines are replaced:
 *
 *  - the step loop (sig): it moves whole blocks through a host call and
 *    keeps the driver's own per-block bookkeeping call:
 *	loop:	jsr hc_xfer; tst.w 26(a2); bne done
 *		clr.w -(sp); move.l d4,-(sp); jsr next; move.w (sp)+,26(a2); bne done
 *		addq.l #1,10(a2); move.l d4,a0; tst.w 20(a0); bne loop
 *    `next` is the call the data step makes after each block (adv);
 *  - the drive check at attach (idsig) answers from the volume;
 *  - the command step's interleave tables (ilsig) map the block numbers the
 *    host call gets, when the drive's state asks for it (byte 8);
 *  - with no interrupt to come, a request that completed returns through
 *    the driver's completion call (done).
 */
static void
pfdrv(fid, page0, f, n)
	int fid, page0;
	unsigned char *f;
	long n;
{
	static unsigned char sig[] = { 0x48, 0xe7, 0x18, 0x38, 0x20, 0x6f, 0x00, 0x18, 0x28, 0x08, 0x24, 0x68,
	    0x00, 0x04, 0x26, 0x6a, 0x00, 0x04, 0x28, 0x52, 0x42, 0x6a, 0x00, 0x1a, 0x30, 0x2a, 0x00, 0x14,
	    0x30, 0x3b, 0x00, 0x14, 0x4e, 0xbb, 0x00, 0x10, 0x4a, 0x40, 0x66, 0xf0 };
	static unsigned char idsig[] = { 0x48, 0xe7, 0x00, 0x38, 0x4c, 0xef, 0x1f, 0x00, 0x00, 0x0c,
	    0x00, 0x11, 0x00, 0xa0, 0x00, 0x12, 0x00, 0xa0 };
	static unsigned char adv[] = { 0x42, 0x67, 0x2f, 0x04, 0x4e, 0xba };
	static unsigned char done[] = { 0x3d, 0x6b, 0x00, 0x1a, 0x00, 0x0c, 0x4a, 0x6b, 0x00, 0x1a, 0x67, 0x1a,
	    0x0c, 0x6b, 0x00, 0x01, 0x00, 0x1a };
	static unsigned char loop[] = { 0x48, 0xe7, 0x18, 0x38, 0x20, 0x6f, 0x00, 0x18, 0x28, 0x08, 0x24, 0x68,
	    0x00, 0x04, 0x4e, 0xb9, 0, 0, 0, 0, 0x4a, 0x6a, 0x00, 0x1a, 0x66, 0x1a, 0x42, 0x67, 0x2f, 0x04,
	    0x4e, 0xba, 0, 0, 0x35, 0x5f, 0x00, 0x1a, 0x66, 0x0c, 0x52, 0xaa, 0x00, 0x0a, 0x20, 0x44, 0x4a, 0x68,
	    0x00, 0x14, 0x66, 0xda, 0x4c, 0xdf, 0x1c, 0x18, 0x20, 0x5f, 0x58, 0x4f, 0x4e, 0xd0 };
	static unsigned char id[] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x20, 0x5f, 0x4f, 0xef, 0x00, 0x10, 0x3e, 0x80, 0x4e, 0xd0 };
	static unsigned char nop[] = { 0x4e, 0x71 };
	static unsigned char ilsig[] = { 0x10, 0x19, 0x4a, 0x2a, 0x00, 0x08, 0x67, 0x18, 0x72, 0x0f, 0xc2, 0x40,
	    0x02, 0x00, 0x00, 0xf0, 0x4a, 0x2a, 0x00, 0x13, 0x66, 0x06, 0xd0, 0x3b, 0x10 };
	long e, i, d, a = -1, t;

	if ((e = find1(f, n, sig, sizeof sig)) < 0 || (i = find1(f, n, idsig, sizeof idsig)) < 0 ||
	    (d = find1(f, n, done, sizeof done)) < 0)
		return;
	for (t = 0; t + 10 <= n; t += 2)
		if (!memcmp(f + t, adv, sizeof adv) && f[t + 8] == 0x35 && f[t + 9] == 0x5f) {
			a = t + 6 + (short)(f[t + 6] << 8 | f[t + 7]);
			break;
		}
	if (a < 0 || a + 4 > n || f[a] != 0x20 || f[a + 1] != 0x78 || e + sizeof loop > n)
		return;
	if ((t = find1(f, n, ilsig, sizeof ilsig)) >= 0 && t + 36 <= n) {
		memcpy(ilv[0], f + t + 24 + (signed char)f[t + 25], 16);
		memcpy(ilv[1], f + t + 30 + (signed char)f[t + 31], 16);
		ilvok = 1;
	}
	put32(loop + 16, (unsigned long)hc_xfer);
	t = a - (e + 32);
	loop[32] = t >> 8;
	loop[33] = t;
	put32(id + 2, (unsigned long)hc_pfid);
	addpat(fid, page0, f, (int)e, loop, sizeof loop);
	addpat(fid, page0, f, (int)i, id, sizeof id);
	addpat(fid, page0, f, (int)d + 10, nop, sizeof nop);
	fprintf(stderr, "lisa: ProFile driver in file %d: host block I/O\n", fid);
}

/*
 * The level 1 interrupt handler asks the status latch and the VIAs which
 * source fired: retrace, the 20 ms timer (VIA T1), the parallel port, the
 * floppy.  It becomes a loop over the module's sources by host call:
 *
 *	h:	movem.l d0/a0,-(sp)
 *	l:	jsr hc_irq1; subq.b #1,d0; bne.s t; jsr retrace; bra.s l
 *	t:	subq.b #1,d0; bne.s x; jsr timer; bra.s l
 *	x:	movem.l (sp)+,d0/a0; rte
 *
 * The retrace routine's tail (acknowledge the latch, wait for the status
 * bit) becomes rts; the timer routine skips its VIA reload and IER write.
 * The init's 4-byte latch write that enables retrace starts the sources.
 */
static void
lldirq(fid, page0, f, n)
	int fid, page0;
	unsigned char *f;
	long n;
{
	static unsigned char hsig[] = { 0x48, 0xe7, 0x80, 0x80, 0x08, 0x39, 0x00, 0x02, 0x00, 0xfc, 0xf8, 0x01,
	    0x66, 0x04, 0x4e, 0xba };
	static unsigned char tsig[] = { 0x10, 0x39, 0x00, 0xfc, 0xd9, 0x69, 0x08, 0x00, 0x00, 0x06, 0x67, 0x06,
	    0x4e, 0xba };
	static unsigned char ack[] = { 0x23, 0xc0, 0x00, 0xfc, 0xe0, 0x18, 0x08, 0x39, 0x00, 0x02, 0x00, 0xfc,
	    0xf8, 0x01, 0x67, 0xf0, 0x4e, 0x75 };
	static unsigned char reload[] = { 0x2f, 0x00, 0x4a, 0x39, 0x00, 0xfc, 0xd9, 0x21, 0x13, 0xf9 };
	static unsigned char ier[] = { 0x48, 0xe7, 0xf8, 0x40, 0x13, 0xfc, 0x00, 0x04, 0x00, 0xfc, 0xd9, 0x71 };
	static unsigned char on[] = { 0x21, 0xc8, 0x00, 0x64, 0x23, 0xc0, 0x00, 0xfc, 0xe0, 0x18 };
	static unsigned char h[] = { 0x48, 0xe7, 0x80, 0x80, 0x4e, 0xb9, 0, 0, 0, 0, 0x53, 0x00, 0x66, 0x06,
	    0x4e, 0xba, 0, 0, 0x60, 0xf0, 0x53, 0x00, 0x66, 0x06, 0x4e, 0xba, 0, 0, 0x60, 0xe6,
	    0x4c, 0xdf, 0x01, 0x01, 0x4e, 0x73 };
	static unsigned char rts[] = { 0x4e, 0x75 }, skip10[] = { 0x60, 0x0e }, skip8[] = { 0x60, 0x06 };
	static unsigned char jsr[6] = { 0x4e, 0xb9 };
	long a, v, tm, k, i, e, o;

	if ((a = find1(f, n, hsig, sizeof hsig)) < 0 || a + 34 > n || memcmp(f + a + 18, tsig, sizeof tsig))
		return;
	v = a + 16 + (short)(f[a + 16] << 8 | f[a + 17]);
	tm = a + 32 + (short)(f[a + 32] << 8 | f[a + 33]);
	if (v < 0 || tm < 0 || v + 0x60 > n || tm + sizeof reload > n || memcmp(f + tm, reload, sizeof reload))
		return;
	for (k = v; k < v + 0x60 && memcmp(f + k, ack, sizeof ack); k += 2)
		;
	if (k == v + 0x60 || (i = find1(f, n, ier, sizeof ier)) < 0 || (o = find1(f, n, on, sizeof on)) < 0)
		return;
	put32(h + 6, (unsigned long)hc_irq1);
	e = v - (a + 16);
	h[16] = e >> 8;
	h[17] = e;
	e = tm - (a + 26);
	h[26] = e >> 8;
	h[27] = e;
	addpat(fid, page0, f, (int)a, h, sizeof h);
	addpat(fid, page0, f, (int)k, rts, sizeof rts);
	addpat(fid, page0, f, (int)tm + 2, skip10, sizeof skip10);
	addpat(fid, page0, f, (int)i + 4, skip8, sizeof skip8);
	put32(jsr + 2, (unsigned long)hc_irqon);
	addpat(fid, page0, f, (int)o + 4, jsr, sizeof jsr);
	fprintf(stderr, "lisa: level 1 interrupts in file %d: host sources\n", fid);
}

/*
 * Other low-level driver routines that drive hardware:
 *  - the clock read sends the keyboard controller a command and polls for
 *    its answer: `jsr hc_clock; rts`, the host's local time;
 *  - the video latch setter: its byte store becomes `jsr hc_video`, D1 the page;
 *  - the contrast ramp's latch writes are skipped: its machine-type test
 *    branches to the common tail;
 *  - the fine time read adds the timer's count to the tick count: the count
 *    reads as just reloaded, so the time is the tick count's.
 */
static void
lldmisc(fid, page0, f, n)
	int fid, page0;
	unsigned char *f;
	long n;
{
	static unsigned char csig[] = { 0x32, 0x3c, 0x4e, 0x20, 0x40, 0xe7, 0x00, 0x7c, 0x07, 0x00, 0x08, 0x39,
	    0x00, 0x01, 0x00, 0xfc, 0xdd, 0x9b };
	static unsigned char vsig[] = { 0xe0, 0x89, 0xee, 0x89, 0x13, 0xc1, 0x00, 0xfc, 0xe8, 0x00 };
	static unsigned char ksig[] = { 0x46, 0x00, 0x20, 0x7c, 0x00, 0xfc, 0xd9, 0x01 };
	static unsigned char tsig[] = { 0x12, 0x39, 0x00, 0xfc, 0xd9, 0x29, 0x08, 0x39, 0x00, 0x06, 0x00, 0xfc, 0xd9, 0x69,
	    0x67, 0x0c, 0x06, 0x80, 0x00, 0x00, 0x00, 0x14, 0x12, 0x39, 0x00, 0xfc, 0xd9, 0x29 };
	static unsigned char tfix[] = { 0x72, 0x63, 0x60, 0x18 };	/* moveq #99,d1; bra past the reads */
	static unsigned char clk[8] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x4e, 0x75 }, vid[6] = { 0x4e, 0xb9 }, bra[2] = { 0x60 };
	long a, t;

	if ((a = find1(f, n, csig, sizeof csig)) >= 14 && f[a - 14] == 0x42 && f[a - 14 + 1] == 0x39) {
		put32(clk + 2, (unsigned long)hc_clock);
		addpat(fid, page0, f, (int)a - 14, clk, sizeof clk);
		fprintf(stderr, "lisa: clock in file %d: host time\n", fid);
	}
	if ((a = find1(f, n, vsig, sizeof vsig)) >= 0) {
		put32(vid + 2, (unsigned long)hc_video);
		addpat(fid, page0, f, (int)a + 4, vid, sizeof vid);
	}
	if ((a = find1(f, n, ksig, sizeof ksig)) >= 8 && f[a - 8] == 0x4a && f[a - 8 + 1] == 0x39 && f[a - 2] == 0x66 &&
	    (t = a + (signed char)f[a - 1]) >= 2 && t < n && f[t - 2] == 0x60 && f[t - 1]) {
		bra[1] = t + (signed char)f[t - 1] - (a - 6);
		addpat(fid, page0, f, (int)a - 8, bra, sizeof bra);
	}
	if ((a = find1(f, n, tsig, sizeof tsig)) >= 0)
		addpat(fid, page0, f, (int)a, tfix, sizeof tfix);
}

/*
 * The keyboard controller's byte decoder is entered from the level 2
 * handler once the VIA flags a byte.  The handler calls it every time and
 * the decoder takes the next input event by host call instead of the byte:
 *
 *	jsr hc_input; tst.l d0; bmi.s x; btst #16,d0; bne.s m
 *	bra.w key; m: bra.w mouse; x: rts
 *
 * `key` is the decoder's key path (D0 the byte), `mouse` its last mouse
 * byte's (D0 dy, dx in the variable its previous state stores).
 */
static void
lldcops(fid, page0, f, n)
	int fid, page0;
	unsigned char *f;
	long n;
{
	static unsigned char dsig[] = { 0x42, 0x40, 0x10, 0x39, 0x00, 0xfc, 0xdd, 0x83, 0x4a, 0x39 };
	static unsigned char hsig[] = { 0x08, 0x39, 0x00, 0x01, 0x00, 0xfc, 0xdd, 0x9b, 0x67, 0x04, 0x4e, 0xba };
	static unsigned char ksig[] = { 0x4a, 0x00, 0x67 };
	static unsigned char d[] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x4a, 0x80, 0x6b, 0x0e, 0x08, 0x00, 0x00, 0x10,
	    0x66, 0x04, 0x60, 0x00, 0, 0, 0x60, 0x00, 0, 0, 0x4e, 0x75 };
	static unsigned char skip[] = { 0x60, 0x08 };
	long a, h, idle, s1, s2, k;

	if ((a = find1(f, n, dsig, sizeof dsig)) < 0 || a + 0x28 > n)
		return;
	for (h = 0; h + 14 <= n; h += 2)	/* the handler: the poll that calls the decoder */
		if (!memcmp(f + h, hsig, sizeof hsig) && h + 12 + (short)(f[h + 12] << 8 | f[h + 13]) == a)
			break;
	if (h + 14 > n || f[a + 0x0e] != 0x67 || !word(f + a + 0x10, 0x0c39) || !word(f + a + 0x12, 1) || !word(f + a + 0x18, 0x6700) ||
	    !word(f + a + 0x1c, 0x0c39) || !word(f + a + 0x1e, 2) || !word(f + a + 0x24, 0x6700))
		return;
	idle = a + 0x10 + (signed char)f[a + 0x0f];
	s1 = a + 0x1a + (short)(f[a + 0x1a] << 8 | f[a + 0x1b]);
	s2 = a + 0x26 + (short)(f[a + 0x26] << 8 | f[a + 0x27]);
	if (idle < 0 || idle + 10 > n || memcmp(f + idle, ksig, sizeof ksig) || !word(f + idle + 4, 0x0c00) ||
	    s1 < 0 || s1 + 6 > n || !word(f + s1, 0x13c0) || s2 < 0 || s2 > n)
		return;
	k = idle + 10;
	mousedx = (unsigned long)f[s1 + 2] << 24 | f[s1 + 3] << 16 | f[s1 + 4] << 8 | f[s1 + 5];
	put32(d + 2, (unsigned long)hc_input);
	d[18] = (k - (a + 18)) >> 8;
	d[19] = k - (a + 18);
	d[22] = (s2 - (a + 22)) >> 8;
	d[23] = s2 - (a + 22);
	addpat(fid, page0, f, (int)a, d, sizeof d);
	addpat(fid, page0, f, (int)h, skip, sizeof skip);
	fprintf(stderr, "lisa: keyboard and mouse in file %d: host input\n", fid);
}

/*
 * The low-level drivers' start-up programs the VIAs and reads the disk
 * controller's ID for the machine type.  It keeps the stores to memory:
 * D0 = 1 (the parallel port's line as the VIA's zero reads it) and the
 * type's defaults; the VIA setups are branched over, the volume setter
 * returns at once.
 */
static void
lldinit(fid, page0, f, n)
	int fid, page0;
	unsigned char *f;
	long n;
{
	static unsigned char v1[] = { 0x42, 0x00, 0x20, 0x7c, 0x00, 0xfc, 0xd9, 0x01, 0x08, 0xa8, 0x00, 0x05, 0x00, 0x10 };
	static unsigned char id[] = { 0x08, 0x39, 0x00, 0x07, 0x00, 0xfc, 0xc0, 0x31, 0x67 };
	static unsigned char v2[] = { 0x20, 0x7c, 0x00, 0xfc, 0xdd, 0x81, 0x42, 0x28, 0x00, 0x06 };
	static unsigned char vol[] = { 0x48, 0xe7, 0xc0, 0x00, 0x02, 0x40, 0x00, 0x07, 0xe3, 0x48, 0x12, 0x39, 0x00, 0xfc, 0xdd, 0x81 };
	static unsigned char on[] = { 0x21, 0xc8, 0x00, 0x64, 0x23, 0xc0, 0x00, 0xfc, 0xe0, 0x18 };
	static unsigned char rts[] = { 0x4e, 0x75 };
	unsigned char b[4];
	long a, i, o, t, e, v;

	if ((a = find1(f, n, v1, sizeof v1)) < 0 || (i = find1(f, n, id, sizeof id)) < 0 ||
	    (o = find1(f, n, v2, sizeof v2)) < 0 || (t = find1(f, n, on, sizeof on)) < 4 || !word(f + t - 4, 0x41fa) ||
	    (v = find1(f, n, vol, sizeof vol)) < 0)
		return;
	t -= 4;
	for (e = a + 2; e < a + 0x60 && e + 2 < n && !word(f + e, 0x13c0); e += 2)
		;
	if (e >= a + 0x60 || e + 2 >= n || t - (i + 2) < 2 || t - (i + 2) > 126)
		return;
	b[0] = 0x70;			/* moveq #1,d0; bra to its store */
	b[1] = 1;
	b[2] = 0x60;
	b[3] = e - (a + 6);
	addpat(fid, page0, f, (int)a + 2, b, 4);
	b[0] = 0x60;
	b[1] = t - (i + 2);
	addpat(fid, page0, f, (int)i, b, 2);
	for (e = o + 10; e < o + 0x40 && e + 2 < n && !word(f + e, 0x103c); e += 2)
		;
	if (e < o + 0x40 && e + 2 < n) {
		b[1] = e - (o + 2);
		addpat(fid, page0, f, (int)o, b, 2);
	}
	addpat(fid, page0, f, (int)v, rts, sizeof rts);
	fprintf(stderr, "lisa: hardware setup in file %d skipped\n", fid);
}

/*
 * The micro diskette driver keeps whether a disk is in place in its drive
 * state (byte 15), set from the controller.  Its status call and its I/O
 * check read it by host call: no disk.
 */
static void
sonydrv(fid, page0, f, n)
	int fid, page0;
	unsigned char *f;
	long n;
{
	static unsigned char st[] = { 0x20, 0x47, 0x10, 0x28, 0x00, 0x0f, 0x48, 0x80, 0x48, 0xc0, 0x29, 0x40, 0x00, 0x04 };
	static unsigned char io[] = { 0x20, 0x47, 0x4a, 0x28, 0x00, 0x0f, 0x66, 0x08, 0x3d, 0x7c };
	static unsigned char j[6] = { 0x4e, 0xb9 };
	long a, b;

	if ((a = find1(f, n, st, sizeof st)) < 0 || (b = find1(f, n, io, sizeof io)) < 0)
		return;
	put32(j + 2, (unsigned long)hc_fdin);
	addpat(fid, page0, f, (int)a, j, sizeof j);
	addpat(fid, page0, f, (int)b, j, sizeof j);
	fprintf(stderr, "lisa: diskette driver in file %d: disk in place by host call\n", fid);
}

/*
 * Code segments are packed on the volume; the loader's and the OS's
 * unpacker hand each one to a host call when it is done (its epilogue,
 * over the procedure's name, becomes `jsr hc_unpack` and the rest of the
 * epilogue).  Routines patched in the unpacked code:
 *  - the unpacker itself, in the OS's copy;
 *  - parameter memory, 64 bytes at every other byte of its page, read and
 *    written with movep: the address load becomes `jsr hc_pmrd` or `jsr
 *    hc_pmwr` and the count load the routine's `jmp (a0)`; A1 the buffer;
 *  - the memory manager keeps segments in 512-byte pages; every size it
 *    gives a segment rounds up to 4 KB, so each starts and ends on a host
 *    page: the new segment's size (D7, addq.w #7; andi.w #-8 in place of its
 *    check that the segment has no memory yet), a resize's new size (the
 *    same, stored back; its byte-sized code shortened to make room), the
 *    size it takes from the top of free memory at start (by host call), the
 *    jump table page's (4 KB), and a new stack keeps the page it would give
 *    back.
 */
static unsigned char unpsig[] = { 0x4c, 0xdf, 0x1c, 0xf8, 0x4e, 0x5e, 0x20, 0x5f, 0xde, 0xfc, 0x00, 0x14, 0x4e, 0xd0,
    'U', 'N', 'P', 'A', 'C', 'K' };

/* the words at p match w[0..n), -1 any */
static int
wmatch(p, w, n)
	unsigned char *p;
	int *w, n;
{
	for (; n > 0; n--, p += 2, w++)
		if (*w >= 0 && !word(p, *w))
			return 0;
	return 1;
}

/* a jsr to c + f within 192 bytes after c + a */
static int
callsto(c, n, a, f)
	unsigned char *c;
	long n, a, f;
{
	long e = a + 192 < n - 4 ? a + 192 : n - 4;

	for (; a < e; a += 2)
		if (word(c + a, 0x4eba) && a + 2 + (short)G16P(c + a + 2) == f)
			return 1;
	return 0;
}

/* patches the code in c[0..n) in place; the number made */
static int
codepatch(c, n)
	unsigned char *c;
	long n;
{
	static const unsigned char sig[] = { 0x20, 0x5f, 0x22, 0x5f, 0x24, 0x7c, 0x00, 0xfc, 0xc1, 0x81, 0x30, 0x3c, 0x00, 0x0f };
	static const unsigned char seg[] = { 0x52, 0x47, 0x60, 0x0a, 0x4a, 0x6c, 0x00, 0x20, 0x67, 0x04, 0x3e, 0x2c, 0x00, 0x20 };
	static const unsigned char rsz[] = { 0x10, 0x2c, 0x00, 0x0c, 0x48, 0x80, 0x52, 0x40, 0x19, 0x40, 0x00, 0x0c,
	    0x0c, 0x2c, 0x00, 0x06, 0x00, 0x0d, 0x57, 0xc0, 0x0c, 0x2c, 0x00, 0x03, 0x00, 0x0d, 0x57, 0xc1,
	    0x80, 0x01, 0x44, 0x00, 0x1a, 0x00, 0x30, 0x2c, 0x00, 0x20, 0x90, 0x6c, 0x00, 0x0a, 0x3e, 0x00, 0x4a, 0x47 };
	static const unsigned char rsz2[] = { 0x52, 0x2c, 0x00, 0x0c, 0x0c, 0x2c, 0x00, 0x06, 0x00, 0x0d, 0x57, 0xc0,
	    0x0c, 0x2c, 0x00, 0x03, 0x00, 0x0d, 0x57, 0xc1, 0x80, 0x01, 0x44, 0x00, 0x1a, 0x00, 0x30, 0x2c, 0x00, 0x20,
	    0x5e, 0x40, 0x02, 0x40, 0xff, 0xf8, 0x39, 0x40, 0x00, 0x20, 0x90, 0x6c, 0x00, 0x0a, 0x3e, 0x00 };
	static const unsigned char stk[] = { 0x0c, 0x2c, 0x00, 0x01, 0x00, 0x0d, 0x66, 0x1e, 0x30, 0x2c, 0x00, 0x0a,
	    0x53, 0x40, 0x39, 0x40, 0x00, 0x0a, 0x30, 0x2c, 0x00, 0x0a, 0xd0, 0x6c, 0x00, 0x08, 0x3f, 0x00,
	    0x3f, 0x3c, 0x00, 0x01, 0x4e, 0xba };
	static int top[] = { 0x4e56, 0xfff8, 0x48e7, 0x0118, 0x3e2e, 0x000c, 0x1d7c, 0x0001, 0x000e, 0x266d, -1,
	    0x202b, -1, 0x720e, 0x9081, 0x2840, 0xbe6c, 0x000a };
	static int stb[] = { 0x202e, -1, 0xd0bc, 0x0000, 0x0200, 0x5380, 0x2f00, 0x2f3c, 0x0000, 0x0200, 0x4ead, -1,
	    0x2f3c, 0x0000, 0x0200, 0x4ead, -1, 0x2d5f };
	static int mkp[] = { 0x4e56, 0xfff6, 0x48e7, 0x0f18, 0x42a7, 0x4ead, -1, 0x2d5f, 0xfff6, 0x4267, 0x3f3c, 0x003c };
	static int jtp[] = { 0x4267, 0x3f3c, 0x0001, 0x486c, 0x0008, 0x4eba, -1, 0x101f, 0x0a00, 0x0001, 0x6708,
	    0x3f3c, -1, 0x4ead, -1, 0x397c, 0x0001, 0x000a, 0x297c, 0x0000, 0x0200, 0x0010 };
	static unsigned char unp[] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x20, 0x5f, 0xde, 0xfc, 0x00, 0x14, 0x4e, 0xd0 };
	static unsigned char tk[] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x4e, 0x71, 0x4e, 0x71 };
	static unsigned char pmc[] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x4e, 0xd0 };
	static const unsigned char ctx[] = { 0x20, 0x5f, 0x12, 0x1f, 0x08, 0x01, 0x00, 0x00, 0x66, 0x00, 0x00, 0x0c,
	    0x13, 0xc0, 0x00, 0xfc, 0xe0, 0x08 };
	static const unsigned char fdt[] = { 0x2a, 0x3c, 0x00, 0xfc, 0xc0, 0x31, 0x20, 0x45, 0x4a, 0x10, 0x6d, 0x06, 0x42, 0x6d };
	static const unsigned char scc[] = { 0x10, 0x10, 0x4e, 0x71, 0x10, 0xbc, 0x00, 0x09, 0x4e, 0x71, 0x10, 0xbc };
	static const unsigned char slot[] = { 0x20, 0x5f, 0x22, 0x78, 0x00, 0x08, 0x45, 0xfa, 0x00, 0x14, 0x21, 0xca, 0x00, 0x08,
	    0x24, 0x5f, 0x10, 0x12, 0x1e, 0xbc, 0x00, 0x01 };
	unsigned char *p;
	long a, mk = -1;
	int k = 0;

	put32(unp + 2, (unsigned long)hc_unpack);
	put32(tk + 2, (unsigned long)hc_take);
	for (a = 0; a + 2 * 12 < n; a += 2)
		if (wmatch(c + a, mkp, 12))
			mk = a;
	for (a = 0; a + 32 < n; a += 2) {
		p = c + a;
		if (mk >= 0 && a + 2 * 19 < n && wmatch(p, stb, 18) && word(p + 36, G16P(p + 2)) && callsto(c, n, a + 38, mk)) {
			p[8] = p[18] = p[28] = 0x10;	/* a new process's stack size: whole 4 KB */
			k++;
		} else if (a + 2 * 22 < n && wmatch(p, jtp, 22)) {
			p[5] = p[33] = 8;		/* the jump table page: 4 KB */
			p[40] = 0x10;
			p[41] = 0;
			k++;
		} else if (a + 2 * 18 < n && wmatch(p, top, 18)) {
			memcpy(p + 8, tk, sizeof tk);
			k++;
		} else if (a + sizeof rsz < n && !memcmp(p, rsz, sizeof rsz)) {
			memcpy(p, rsz2, sizeof rsz2);
			k++;
		} else if (a + 38 < n && !memcmp(p, stk, sizeof stk) && word(p + 36, 0x603e)) {
			memcpy(p + 8, "\x60\x5a", 2);		/* bra past the give-back */
			k++;
		} else if (!memcmp(p, unpsig, sizeof unpsig)) {
			memcpy(p + 6, unp, sizeof unp);
			k++;
		} else if (!memcmp(p, seg, sizeof seg) && word(p + 14, 0x4a6c) && word(p + 16, 8) && word(p + 18, 0x6708)) {
			memcpy(p, "\x4e\x71", 2);		/* no spare page over a stack */
			memcpy(p + 14, "\x5e\x47\x02\x47\xff\xf8\x4e\x71\x4e\x71\x4e\x71\x4e\x71", 14);
			k++;
		} else if (!memcmp(p, ctx, sizeof ctx)) {
			put32(pmc + 2, (unsigned long)hc_ctx);
			memcpy(p + 4, pmc, sizeof pmc);
			k++;
		} else if (!memcmp(p, fdt, sizeof fdt) && p[16] == 0x60) {
			/* jsr hc_fdtype; move.w d0,type(a5); bra past the probe */
			memcpy(p, "\x4e\xb9....\x3b\x40", 8);
			put32(p + 2, (unsigned long)hc_fdtype);
			p[8] = p[14];
			p[9] = p[15];
			p[10] = 0x60;
			p[11] = 6 + p[17];
			k++;
		} else if (!memcmp(p, scc, sizeof scc)) {
			memcpy(p, "\x4e\x75", 2);		/* the serial controller's setup */
			k++;
		} else if (!memcmp(p, slot, sizeof slot)) {
			memcpy(p + 16, "\x42\x17\x4e\x71\x4e\x71", 6);	/* the slot probe finds no card */
			k++;
		} else if (!memcmp(p, sig, sizeof sig) && (word(p + 14, 0x034a) || (word(p + 14, 0x2219) && word(p + 16, 0x03ca)))) {
			put32(pmc + 2, (unsigned long)(word(p + 14, 0x034a) ? hc_pmrd : hc_pmwr));
			memcpy(p + 4, pmc, sizeof pmc);
			k++;
		}
	}
	return k;
}

/* the loader's unpacker, before it runs */
static int
patchunpack(fd)
	int fd;
{
	unsigned char *ram = (unsigned char *)PHYS;
	struct lisapatch lp;
	unsigned long a;
	int n = 0;

	for (a = SYSAREA; a + sizeof unpsig < RAMSZ; a += 2)
		if (!memcmp(ram + a, unpsig, sizeof unpsig)) {
			lp.lp_addr = a + 6;
			lp.lp_len = 14;
			memcpy(lp.lp_data, "\x4e\xb9....\x20\x5f\xde\xfc\x00\x14\x4e\xd0", 14);
			put32(lp.lp_data + 2, (unsigned long)hc_unpack);
			if (ioctl(fd, LISAIOC_PATCH, &lp) == 0)
				n++;
		}
	return n;
}

/* the memory manager's take from the top of free memory: D7 the size, rounded; the result true */
static void
hctake(r)
	unsigned long *r;
{
	r[7] = (r[7] & ~0xffffUL) | ((G16(r[14] + 12) + 7) & ~7);
	P8(r[14] + 14, 1);
}

/* a segment unpacked: on the stack the return, the output size (word), the input size, the table, the output, the input */
static void
hcunpack(r)
	unsigned long *r;
{
	static unsigned char c[0x10000];
	unsigned char *s = (unsigned char *)&r[17];
	unsigned long dst = G32((unsigned long)s + 12);
	long n = G16((unsigned long)s + 4), i, k;

	if (n <= 0)
		return;
	for (i = 0; i < n; i += k)
		if (gmem(dst + i, c + i, (int)(k = n - i > 512 ? 512 : n - i), 0, 0L) < 0)
			break;
	if (i == n && (k = codepatch(c, n)) > 0) {
		for (i = 0; i < n; i += k)
			gmem(dst + i, c + i, (int)(k = n - i > 512 ? 512 : n - i), LM_WRITE | (k == n - i ? LM_FLUSH : 0), 0L);
		if (verbose)
			fprintf(stderr, "lisa: unpacked code at %#lx patched\n", dst);
	}
}

/* the patches for the file of a block's tag, worked out on its first block from the whole file */
static void
patchblock(p)
	unsigned char *p;
{
	int fid = (short)(p[4] << 8 | p[5]), page = p[12] << 8 | p[13], lo = 0x7fff, hi = -1, i, k;
	unsigned char *f, *t;
	long b;

	if (fid <= 0)
		return;
	for (i = 0; i < nseen && seen[i] != fid; i++)
		;
	if (i == nseen && nseen == sizeof seen / sizeof seen[0] && fid != full) {
		fprintf(stderr, "lisa: file table full: file %d not patched\n", fid);
		full = fid;
	}
	if (i == nseen && nseen < sizeof seen / sizeof seen[0]) {
		seen[nseen++] = fid;
		for (b = 0; b < vblocks; b++) {
			t = vtag + b * 20;
			if ((short)(t[4] << 8 | t[5]) == fid) {
				k = t[12] << 8 | t[13];
				lo = k < lo ? k : lo;
				hi = k > hi ? k : hi;
			}
		}
		if (hi >= lo && (hi - lo + 1) * 512L <= fbufsz) {
			unsigned char blk[BLK];

			f = fbuf;
			memset(f, 0, (hi - lo + 1) * 512L);
			for (b = 0; b < vblocks; b++) {
				t = vtag + b * 20;
				if ((short)(t[4] << 8 | t[5]) == fid && vread(b, blk) == 0)
					memcpy(f + ((t[12] << 8 | t[13]) - lo) * 512L, blk + 20, 512);
			}
			pfdrv(fid, lo, f, (hi - lo + 1) * 512L);
			lldirq(fid, lo, f, (hi - lo + 1) * 512L);
			lldmisc(fid, lo, f, (hi - lo + 1) * 512L);
			lldcops(fid, lo, f, (hi - lo + 1) * 512L);
			lldinit(fid, lo, f, (hi - lo + 1) * 512L);
			sonydrv(fid, lo, f, (hi - lo + 1) * 512L);
		}
	}
	/* tag byte 11 keeps a checksummed block's bytes XORing to zero */
	for (i = 0; i < npat; i++)
		if (pat[i].fid == fid && pat[i].page == page && p[20 + pat[i].off] == pat[i].old) {
			p[20 + pat[i].off] = pat[i].val;
			if (p[6] & 0x80)
				p[11] ^= pat[i].old ^ pat[i].val;
		}
}

/* ---- host services ---- */

static int copsdone;
static int patchload(), patchcops(), patchmmu(), patchserial(), patchprobe(), word();
static void hcreset();

/* the boot ROM's block read: D1 the block, A1 the tag buffer, A2 the data buffer; D0 0, or 1 on error */
static void
hcread(r)
	unsigned long *r;
{
	unsigned char b[BLK];
	unsigned long blk = r[1] & 0xffffff;
	int k;

	/* before each read, the blocks read and checked so far may need patches */
	if (!copsdone) {
		if ((k = patchload(lfd)) > 0)
			fprintf(stderr, "lisa: loader rounds to 4 KB (%d)\n", k);
		if ((k = patchmmu(lfd)) > 0)
			fprintf(stderr, "lisa: MMU setup by host call (%d)\n", k);
		if ((k = patchserial(lfd)) > 0)
			fprintf(stderr, "lisa: serial number by host call (%d)\n", k);
		if ((k = patchunpack(lfd)) > 0)
			fprintf(stderr, "lisa: unpacked code by host call (%d)\n", k);
		if ((k = patchprobe(lfd)) > 0)
			fprintf(stderr, "lisa: loader's controller probe skipped (%d)\n", k);
		if ((k = patchcops(lfd)) > 0) {
			fprintf(stderr, "lisa: controller send patched (%d)\n", k);
			copsdone = 1;
		}
	}
	r[0] = 1;
	/* block -1 is the drive's identity read: zeros */
	if (blk == 0xffffff)
		memset(b, 0, BLK);
	else if (vread((long)blk, b) < 0)
		return;
	else
		patchblock(b);
	if (gmem(r[10], b + 20, 512, LM_WRITE | LM_BLOCK, blk) < 0 || (r[9] && gmem(r[9], b, 20, LM_WRITE, 0L) < 0))
		return;
	r[0] = 0;
}

/* the driver's drive check (block, port, port, block -> error): status clear, the volume's size */
static void
hcpfid(r)
	unsigned long *r;
{
	unsigned long c = r[21];

	P32(c + 22, 0);
	P8(c + 19, 0);
	P8(c + 35, vblocks >> 16);
	P8(c + 36, vblocks >> 8);
	P8(c + 37, vblocks);
	r[0] = 0;
}

/*
 * One block of a driver request: A2 the drive's state (command and block at
 * 10, status at 22, error at 26), D4 the request (data at 8 plus 16, tags
 * at 12).  Tags in memory are 24 bytes, on the volume 20: the zero high
 * bytes at 8 and 12 and the sign-extension bytes at 16 and 20 are not
 * stored, and byte 11 on the volume makes the block's bytes XOR to zero
 * when bit 7 of byte 6 is set.
 */
static void
hcxfer(r)
	unsigned long *r;
{
	static int warned;
	unsigned long c = r[10], q = r[4], blk = G32(c + 10) & 0xffffff;
	unsigned long data = G32(q + 8) + G32(q + 16), tag = G32(q + 12);
	unsigned char b[BLK], m[24], x;
	int cmd = G8(c + 10), i, e = 0;

	if (ilvok && G8(c + 8))
		blk = (blk & ~0xfUL) | ilv[G8(c + 19) != 0][blk & 0xf];

	P32(c + 22, 0);
	if (cmd == 0) {
		if (vread((long)blk, b) < 0) {
			P16(c + 26, 1885);
			return;
		}
		patchblock(b);
		memcpy(m, b, 8);
		m[8] = 0;
		memcpy(m + 9, b + 8, 3);
		m[12] = 0;
		memcpy(m + 13, b + 11, 3);
		m[16] = b[14] & 0x80 ? 0xff : 0;
		memcpy(m + 17, b + 14, 3);
		m[20] = b[17] & 0x80 ? 0xff : 0;
		memcpy(m + 21, b + 17, 3);
		for (i = x = 0; i < BLK; i++)
			x ^= b[i];
		if (m[6] & 0x80) {
			m[13] = 0;
			m[6] &= 0x7f;
			if (x)
				e = -663;
		}
		if (gmem(data, b + 20, 512, LM_WRITE, 0L) < 0 || gmem(tag, m, 24, LM_WRITE, 0L) < 0)
			e = 1885;
	} else if (cmd == 1 || cmd == 2) {
		if (gmem(data, b + 20, 512, 0, 0L) < 0 || gmem(tag, m, 24, 0, 0L) < 0) {
			P16(c + 26, 1885);
			return;
		}
		memcpy(b, m, 6);
		b[6] = m[6] | 0x80;
		b[7] = m[7];
		memcpy(b + 8, m + 9, 3);
		b[11] = 0;
		memcpy(b + 12, m + 14, 2);
		memcpy(b + 14, m + 17, 3);
		memcpy(b + 17, m + 21, 3);
		for (i = x = 0; i < BLK; i++)
			x ^= b[i];
		b[11] = x;
		if (vwrite((long)blk, b) < 0)
			e = 1885;
	} else {
		if (!warned++)
			fprintf(stderr, "lisa: ProFile command %d, block %#lx\n", cmd, blk);
		e = 1885;
	}
	P16(c + 26, e);
}

/*
 * The loader's MMU trap handler (D2 context, D3 first segment, D1 count,
 * D0 the context to select after): it loads each segment's registers in
 * setup mode from its table of (origin, type, limit) per context.  The
 * same values go to the module in one call.
 */
static void
hcmmu(r)
	unsigned long *r;
{
	struct lisaseg g;
	unsigned long h = r[16] - 6, a, phys = G32(0x2a4) >> 9;
	unsigned short org;
	unsigned char tb[512], *e;
	int i, type, lim;

	a = h + 0x12 + (short)G16(h + 0x12) + (short)(r[2] << 9) + (short)(r[3] << 2);
	g.lg_ctx = r[2] & 3;
	g.lg_first = r[3] & 0x3fff;
	g.lg_n = ((r[1] & 0xff00) | ((r[1] - 1) & 0xff)) + 1;
	g.lg_final = r[0] & 3;
	if (g.lg_n > 128)
		g.lg_n = 128;
	if (gmem(a, tb, g.lg_n * 4 > 256 ? 256 : g.lg_n * 4, 0, 0L) < 0 ||
	    (g.lg_n > 64 && gmem(a + 256, tb + 256, (g.lg_n - 64) * 4, 0, 0L) < 0))
		return;
	for (i = 0; i < g.lg_n; i++) {
		e = tb + 4 * i;
		org = e[0] << 8 | e[1];
		type = e[2];
		lim = e[3];
		if (type == 6) {			/* a stack: the limit counts down from the top */
			if (!lim)
				lim = 256;
			org += lim - 256;
			lim--;
		} else if (!(org & 7))
			lim = -((lim + 7) & ~7);	/* to the end of its host page, which is its own */
		else
			lim = -lim;
		if ((short)(type << 8) <= 0x700)
			org += phys;
		g.lg_slr[i] = type << 8 | (lim & 0xff);
		g.lg_sor[i] = org;
		if (verbose && type >= 4 && type <= 7 && (org & 7))
			fprintf(stderr, "lisa: segment %#x origin %#x type %d size %#x\n", g.lg_first + i, org, type, e[3]);
	}
	ioctl(lfd, LISAIOC_SEG, &g);
}

/* one segment's registers set inline: the jsr is followed by the SLR, its address, the context and the SOR's register */
static void
hcseg1(r)
	unsigned long *r;
{
	struct lisaseg g;
	unsigned long p = r[16];

	g.lg_slr[0] = G16(p);
	g.lg_first = G32(p + 2) >> 17 & 0x7f;
	g.lg_ctx = g.lg_final = G8(p + 6);
	g.lg_sor[0] = r[G8(p + 7) & 7];
	g.lg_n = 1;
	if (verbose)
		fprintf(stderr, "lisa: segment %#x SLR %#x SOR %#x context %d set inline\n", g.lg_first, g.lg_slr[0],
		    g.lg_sor[0], g.lg_ctx);
	r[16] += 8;
	ioctl(lfd, LISAIOC_SEG, &g);
}

/* d0: year since 1980 (a nibble) and the day of the year's first two BCD digits; d1: the rest, hours to tenths */
static void
hcclock(r)
	unsigned long *r;
{
	time_t now = time((time_t *)0);
	struct tm *t = localtime(&now);
	int d = t->tm_yday + 1;

	r[0] = ((t->tm_year + 1900 - 1980) & 15) << 8 | (d / 100) << 4 | d / 10 % 10;
	r[1] = (unsigned long)(d % 10) << 28 | (unsigned long)(t->tm_hour / 10) << 24 | (t->tm_hour % 10) << 20 |
	    (t->tm_min / 10) << 16 | (t->tm_min % 10) << 12 | (t->tm_sec / 10) << 8 | (t->tm_sec % 10) << 4;
}


/* the next input event: D0 the key byte; or $10000 | dy, with dx stored; or -1 none */
static void
hcinput(r)
	unsigned long *r;
{
	long ev = ioctl(lfd, LISAIOC_INPUT, 0);

	if (ev & LIN_MOUSE) {
		P8(mousedx, ev >> 8);
		r[0] = 0x10000 | (ev & 0xff);
	} else if (ev & LIN_KEY)
		r[0] = ev & 0xff;
	else
		r[0] = -1;
}

/*
 * The OS's A-line dispatcher (vector 10) saves A0, pops the frame and drops
 * to user mode:
 *
 *	move.l a0,save; tst.w (sp)+; move.l (sp)+,a0; bge.s 1f; ori #$8000,sr
 *  1:	andi #$dfff,sr
 *
 * User-mode A-lines come here instead with their pc pushed: the same save,
 * then the dispatcher's user-mode tail, so they need no switch of context.
 */
static unsigned char aentry[14];

static void
alinereg()
{
	static const unsigned char sig[] = { 0x4a, 0x5f, 0x20, 0x5f, 0x6c, 0x04, 0x00, 0x7c, 0x80, 0x00, 0x02, 0x7c, 0xdf, 0xff };
	unsigned char c[20];
	unsigned long h;

	if (aentry[0] || (h = G32(0x28) & 0xffffff) == 0 || gmem(h, c, sizeof c, 0, 0L) < 0 ||
	    c[0] != 0x23 || c[1] != 0xc8 || memcmp(c + 6, sig, sizeof sig))
		return;
	memcpy(aentry, c, 6);				/* move.l a0,save */
	memcpy(aentry + 6, "\x20\x5f\x4e\xf9", 4);	/* move.l (sp)+,a0; jmp tail */
	put32(aentry + 10, h + 20);
	if (ioctl(lfd, LISAIOC_ALINE, (caddr_t)aentry) < 0)
		aentry[0] = 0;
	else if (verbose)
		fprintf(stderr, "lisa: A-line dispatcher at %#lx: user-mode entry\n", h);
}

void
hostcall(r)
	unsigned long *r;
{
	int fd;

	switch (r[15]) {
	case 0:
		hcread(r);
		break;
	case 1:
		hcpfid(r);
		break;
	case 2:
		hcxfer(r);
		break;
	case 3:
		hcmmu(r);
		break;
	case 4:
		hcseg1(r);
		break;
	case 5:
		r[0] = serial;			/* the serial number: three digits, then five */
		r[1] = 0;
		break;
	case 6:
		ioctl(lfd, LISAIOC_IRQ, LIRQ_ON);
		break;
	case 7:
		r[0] = ioctl(lfd, LISAIOC_IRQ, LIRQ_TAKE);
		break;
	case 8:
		hcclock(r);
		break;
	case 9:
		ioctl(lfd, LISAIOC_VIDEO, (int)(r[1] & 0x3f));
		break;
	case 12:
		hcinput(r);
		break;
	case 13:
		hcunpack(r);
		break;
	case 14:
		hctake(r);
		break;
	case 15:
		ioctl(lfd, LISAIOC_CTX, (int)(r[1] & 3));
		alinereg();
		break;
	case 16:
		r[0] = 2;			/* a Lisa 2/5: micro diskette drive, ProFile on the parallel port */
		break;
	case 17:
		r[0] = 0;
		break;
	case 18:
		hcreset(r);
		break;
	case 19:
		fprintf(stderr, "lisa: stopped by the ROM at %#lx, error %d\n", (r[16] - 6) & 0xffffff,
		    (int)G8(0x1b4));
		exit(1);
	case 20:
		ioctl(lfd, LISAIOC_VIDEO, (int)(r[0] & 0x3f));
		break;
	case 10:
		gmem(r[9], pm, sizeof pm, LM_WRITE, 0L);
		break;
	case 11:
		if (gmem(r[9], pm, sizeof pm, 0, 0L) == 0 && pmfile && (fd = open(pmfile, O_WRONLY | O_CREAT, 0644)) >= 0) {
			write(fd, pm, sizeof pm);
			close(fd);
		}
		break;
	}
}

static unsigned char *romp;		/* the copy of the user's ROM the guest runs */
static unsigned long rstop;		/* its stop stub */

/* the ROM's word at offset o */
#define	RW(o)	((unsigned)romp[o] << 8 | romp[(o) + 1])

/* the offset of sig in the ROM from o on, or -1 */
static long
romfind(sig, n, o)
	unsigned char *sig;
	int n;
	long o;
{
	for (; o + n <= LISA_ROMSZ; o += 2)
		if (memcmp(romp + o, sig, n) == 0)
			return o;
	return -1;
}

/* where the jump table entry at o goes: jmp d16(pc) */
static long
romjt(o)
	long o;
{
	return RW(o) == 0x4efa ? o + 2 + (short)RW(o + 2) : -1;
}

/* jsr abs.l at offset o */
static void
romjsr(o, f)
	long o;
	void (*f)();
{
	romp[o] = 0x4e;
	romp[o + 1] = 0xb9;
	put32(romp + o + 2, (unsigned long)f);
}

/* the first instruction at o returns at once */
static void
romrts(o)
	long o;
{
	romp[o] = 0x4e;
	romp[o + 1] = 0x75;
}

static void
romload(path)
	char *path;
{
	int fd = open(path, O_RDONLY);

	if (fd < 0 || read(fd, romp, LISA_ROMSZ) != LISA_ROMSZ)
		die(path);
	close(fd);
}

/*
 * The user's ROM runs from its reset vector, with the routines that touch
 * hardware patched in the loaded copy:
 *  - the self-test (the reset entry): a host call leaves what it finds
 *    (memory, the screen, the boot device), then the ROM's own boot device
 *    choice runs, found by its test of the "device given" flag;
 *  - the block read (jump table $90): a host call, D1 the block, A1 the
 *    tags, A2 the data, carry on error;
 *  - the keyboard controller's send (jump table $A8) and its reset pulse,
 *    found by the controller's VIA address: return at once;
 *  - the video latch store: a host call, D0 the page;
 *  - the disk controller command (by its address) returns at once;
 *  - the boot menu's input loop (interrupts masked, then the wait), the
 *    restart entry ($80) and every `bra .`: a host call that stops the
 *    environment.
 */
static void
romfix()
{
	static unsigned char boot[] = { 0x08, 0x07, 0x00, 0x1c, 0x67, 0x10, 0x10, 0x38, 0x01, 0xb3 };
	static unsigned char cops[] = { 0x20, 0x7c, 0x00, 0xfc, 0xdd, 0x81 };
	static unsigned char cpulse[] = { 0x20, 0x7c, 0x00, 0xfc, 0xdd, 0x81, 0x08, 0x90, 0x00, 0x00 };
	static unsigned char vlatch[] = { 0x13, 0xc0, 0x00, 0xfc, 0xe8, 0x00 };
	static unsigned char fdc[] = { 0x20, 0x7c, 0x00, 0xfc, 0xc0, 0x01, 0x11, 0x7c, 0x00, 0x88, 0x00, 0x02 };
	static unsigned char menu[] = { 0x00, 0x7c, 0x07, 0x00, 0x61, 0x00 };
	unsigned long go = 0;
	long a, b, rst = RW(4) == 0xfe ? (long)RW(6) & ~1L : -1L;
	int n;

	if ((a = romfind(boot, sizeof boot, 0L)) >= 0)
		for (b = a; b > a - 16 && b > 0; b -= 2)
			if (RW(b) == 0x42b8) {
				go = 0xfe0000 | b;
				break;
			}
	if (!go || rst < 0x80 || rst + 18 > LISA_ROMSZ)
		die("ROM: no reset entry or boot device choice");
	romjsr(rst, hc_reset);
	romp[rst + 6] = 0x4e;			/* jmp go */
	romp[rst + 7] = 0xf9;
	put32(romp + rst + 8, go);
	romjsr(rst + 12, hc_stop);
	rstop = 0xfe0000 | (rst + 12);
	if ((a = romjt(0x90L)) < 0 || a + 10 > LISA_ROMSZ)
		die("ROM: no block read entry");
	romjsr(a, hc_read);
	romp[a + 6] = 0x44;			/* neg.l d0: carry on error */
	romp[a + 7] = 0x80;
	romrts(a + 8);
	fprintf(stderr, "lisa: ROM reset at %#lx, boot choice at %#lx, block read at %#lx\n", 0xfe0000 | rst, go, 0xfe0000 | a);
	if ((a = romjt(0xa8L)) >= 0 && romfind(cops, sizeof cops, a) >= 0 && romfind(cops, sizeof cops, a) < a + 16)
		romrts(a);
	else
		fprintf(stderr, "lisa: ROM: no controller send\n");
	if ((a = romfind(cpulse, sizeof cpulse, 0L)) >= 0) {
		romrts(a);
		for (b = a + 10; b < a + 32; b += 2)
			if (RW(b) == 0x08d0 && RW(b + 2) == 0) {
				romrts(b);
				break;
			}
	}
	for (n = 0, a = 0; (a = romfind(vlatch, sizeof vlatch, a)) >= 0; a += 6, n++)
		romjsr(a, hc_vid0);
	if ((a = romfind(fdc, sizeof fdc, 0L)) >= 0)
		romrts(a);
	for (a = 0; (a = romfind(menu, sizeof menu, a)) >= 0; a += 2)
		if (RW(a + 8) == 0x4278) {
			romjsr(a, hc_stop);
			break;
		}
	if ((a = romjt(0x80L)) >= 0)
		romjsr(a, hc_stop);
	for (a = 0; a < LISA_ROMSZ; a += 2)
		if (RW(a) == 0x60fe) {
			b = rst + 12 - (a + 2);
			if (b < -128 || b > 127)
				fprintf(stderr, "lisa: ROM: spin at %#lx out of reach\n", 0xfe0000 | a);
			else
				romp[a + 1] = b;
		}
}

/*
 * The self-test's results: RAM from $8000 (the ROM's first 32 KB below it)
 * to its end, logical 0 at its start, the screen in its last 32 KB, the
 * ProFile the boot device, and D7's flag that the boot device is given.
 * Exceptions before the boot code sets its own handlers stop the
 * environment.
 */
static void
hcreset(r)
	unsigned long *r;
{
	int i;

	P32(0x2a4, 0x8000);
	P32(0x294, RAMSZ);
	P32(0x2a8, RAMSZ - 0x8000);
	P32(0x110, RAMSZ - 0x10000);
	P32(0x260, 0x2b0);
	P8(0x1b3, 2);
	for (i = 2; i < 64; i++)
		P32(i * 4, rstop);
	r[7] = 1L << 28;
	fprintf(stderr, "lisa: self-test by host call\n");
}

/*
 * The keyboard controller's byte-send routine in the loaded drivers waits on a
 * handshake line no host call answers.  It is found by what it does: it loads
 * the controller's VIA address into A0 and the line's offset into A1, right after a
 * register save.  The first instruction becomes an RTS: bytes to the controller
 * are dropped.  The module writes the patch and pushes the caches.
 */
static int
patchcops(fd)
	int fd;
{
	static const unsigned char sig[] = { 0x20, 0x7c, 0x00, 0xfc, 0xdd, 0x81, 0x43, 0xe8, 0x00, 0x06 };
	unsigned char *ram = (unsigned char *)PHYS;
	struct lisapatch lp;
	unsigned long a, b;
	int n = 0;

	for (a = SYSAREA; a + sizeof sig < SYSAREA + 0x30000UL; a += 2) {
		if (memcmp(ram + a, sig, sizeof sig))
			continue;
		for (b = a; b > a - 16 && b >= SYSAREA; b -= 2)
			if (ram[b] == 0x48 && ram[b + 1] == 0xe7) {
				lp.lp_addr = b;
				lp.lp_len = 2;
				lp.lp_data[0] = 0x4e;
				lp.lp_data[1] = 0x75;
				if (ioctl(fd, LISAIOC_PATCH, &lp) == 0)
					n++;
				break;
			}
	}
	return n;
}

static int
word(p, v)
	unsigned char *p;
	int v;
{
	return p[0] == (v >> 8 & 0xff) && p[1] == (v & 0xff);
}

static void
setw(fd, a, v)
	int fd;
	unsigned long a;
	int v;
{
	struct lisapatch lp;

	lp.lp_addr = a;
	lp.lp_len = 2;
	lp.lp_data[0] = v >> 8;
	lp.lp_data[1] = v;
	ioctl(fd, LISAIOC_PATCH, &lp);
}

/*
 * The loader picks the boot device from the disk controller's ID when the
 * ROM left none; the launcher leaves one, so the read is branched over.
 */
static int
patchprobe(fd)
	int fd;
{
	static const unsigned char sig[] = { 0x20, 0x7c, 0x00, 0x00, 0x01, 0xb3, 0x22, 0x7c, 0x00, 0xfc, 0xc0, 0x31,
	    0x10, 0x11, 0x6a };
	unsigned char *ram = (unsigned char *)PHYS;
	unsigned long a;
	int n = 0;

	for (a = SYSAREA; a + sizeof sig + 1 < RAMSZ; a += 2)
		if (!memcmp(ram + a, sig, sizeof sig)) {
			setw(fd, a + 6, 0x6000 | (8 + ram[a + 15]));
			n++;
		}
	return n;
}

/*
 * The loader rounds its two allocation pointers and the sizes it pads to
 * 512 bytes, the MMU's page; the host maps 4 KB pages, so they become 4 KB
 * and every segment it places starts and ends on a host page.  Each
 * rounding divides by 512 and multiplies back:
 *
 *	[add.l #512,d0; subq.l #1,d0; move.l d0,-(sp)]	round up
 *	move.l #512,-(sp); jsr div; move.l #512,-(sp); jsr mul
 *
 * Patched where the result goes back to the global it came from (the
 * pointers) or a function returns it minus its argument (the padding).
 * A segment limit it sets as a constant (move.w #slr,seg+$8000) grows to
 * whole 4 KB.
 */
static int
patchload(fd)
	int fd;
{
	static const unsigned char rnd[] = { 0x2f, 0x3c, 0x00, 0x00, 0x02, 0x00, 0x4e, 0xba };
	static const unsigned char up[] = { 0xd0, 0xbc, 0x00, 0x00, 0x02, 0x00, 0x53, 0x80, 0x2f, 0x00 };
	static const unsigned char pad[] = { 0x2d, 0x5f, 0xff, 0xfc, 0x20, 0x2e, 0xff, 0xfc, 0x90, 0xae, 0x00, 0x08 };
	unsigned char *ram = (unsigned char *)PHYS, *p;
	unsigned long a;
	int n = 0, u;

	for (a = SYSAREA + 16; a + 32 < RAMSZ; a += 2) {
		p = ram + a;
		if (word(p, 0x33fc) && p[2] >= 4 && p[2] <= 7 && !p[4] && (p[5] & 1) == 0 && p[6] == 0x80 && !p[7] &&
		    (u = p[2] & 1 ? p[3] & ~7 : p[3] | 7) != p[3]) {
			setw(fd, a + 2, p[2] << 8 | u);		/* normal: pages 0..255-limit; stack: 255-limit..255 */
			n++;
			continue;
		}
		if (memcmp(p, rnd, sizeof rnd) || memcmp(p + 10, rnd, sizeof rnd))
			continue;
		u = !memcmp(p - 10, up, sizeof up);
		if (!(u && word(p - 14, 0x202d) && word(p + 20, 0x2b5f) && word(p - 12, p[22] << 8 | p[23])) &&
		    !(!u && word(p - 4, 0x2f2d) && word(p + 20, 0x2b5f) && word(p - 2, p[22] << 8 | p[23])) &&
		    !(u && word(p - 14, 0x202e) && word(p - 12, 8) && !memcmp(p + 20, pad, sizeof pad)))
			continue;
		if (u)
			setw(fd, a - 6, 0x1000);
		setw(fd, a + 4, 0x1000);
		setw(fd, a + 14, 0x1000);
		n++;
	}
	return n;
}

/*
 * The loader programs the MMU in setup mode in two places, both found by
 * what they write:
 *  - its trap handler, copied to a segment of its own before use: the
 *    first instructions become `jsr hc_mmu; rte`;
 *  - one segment set inline: SEG1, SEG2 and setup latches, then
 *    `move.w Dn,sor` and `move.w #slr,slr`, then setup off.
 */
/* move.b Dn,$FCE0xx: the latch at o, or at o + 2 (its other state) for the context bits */
static int
latch(p, o)
	unsigned char *p;
	int o;
{
	return p[0] == 0x13 && (p[1] & 0xf8) == 0xc0 && !p[2] && p[3] == 0xfc && p[4] == 0xe0 &&
	    (p[5] == o || (o < 0x10 && p[5] == o + 2));
}

static int
patchmmu(fd)
	int fd;
{
	static const unsigned char trap[] = { 0x2a, 0x38, 0x02, 0xa4, 0xe0, 0x8d, 0xe2, 0x8d, 0x26, 0x45, 0x40, 0xc4,
	    0x3c, 0x3c, 0x27, 0x00, 0x43, 0xfa };
	unsigned char *ram = (unsigned char *)PHYS, *p;
	struct lisapatch lp;
	unsigned long a, h = (unsigned long)hc_mmu, h1 = (unsigned long)hc_seg1;
	int n = 0;

	for (a = SYSAREA; a + 40 < RAMSZ; a += 2) {
		p = ram + a;
		if (!memcmp(p, trap, sizeof trap)) {
			lp.lp_addr = a;
			lp.lp_len = 8;
			memcpy(lp.lp_data, "\x4e\xb9....\x4e\x73", 8);
			put32(lp.lp_data + 2, h);
			if (ioctl(fd, LISAIOC_PATCH, &lp) == 0)
				n++;
			continue;
		}
		if (!latch(p, 0x08) || !latch(p + 6, 0x0c) || !latch(p + 12, 0x10) || !latch(p + 32, 0x12) ||
		    p[18] != 0x33 || (p[19] & 0xf8) != 0xc0 || p[24] != 0x33 || p[25] != 0xfc ||
		    ((p[20] << 24 | p[21] << 16 | p[22] << 8 | p[23]) ^ (p[28] << 24 | p[29] << 16 | p[30] << 8 | p[31])) != 8)
			continue;
		lp.lp_addr = a;
		lp.lp_len = 16;
		lp.lp_data[0] = 0x4e;
		lp.lp_data[1] = 0xb9;
		put32(lp.lp_data + 2, h1);
		memcpy(lp.lp_data + 6, p + 26, 6);		/* SLR, its address */
		lp.lp_data[12] = (p[5] & 2) >> 1 | (p[11] & 2);	/* context */
		lp.lp_data[13] = p[19] & 7;			/* the SOR's register */
		lp.lp_data[14] = 0x60;
		lp.lp_data[15] = 38 - 16;			/* bra.s past the rest */
		if (ioctl(fd, LISAIOC_PATCH, &lp) == 0)
			n++;
	}
	return n;
}

/*
 * The loader reads the machine's serial number from a bit stream after
 * retrace and returns it through two var parameters.  The routine is found
 * by the stream's address after a register save; it becomes a host call
 * followed by its own epilogue (pop the return and the two addresses,
 * store D0 and D1).
 */
static int
patchserial(fd)
	int fd;
{
	static const unsigned char sig[] = { 0x22, 0x7c, 0x00, 0xfe, 0x80, 0x00 };
	static const unsigned char out[] = { 0x4c, 0xdf, 0x07, 0x00, 0x22, 0x80, 0x34, 0x81, 0x4e, 0xd0 };
	unsigned char *ram = (unsigned char *)PHYS;
	struct lisapatch lp;
	unsigned long a, b, e;
	int n = 0;

	for (a = SYSAREA + 16; a + sizeof sig < RAMSZ; a += 2) {
		if (memcmp(ram + a, sig, sizeof sig))
			continue;
		for (b = a - 2; b > a - 16 && (ram[b] != 0x48 || ram[b + 1] != 0xe7); b -= 2)
			;
		for (e = a; e < a + 0x200 && e + sizeof out < RAMSZ && memcmp(ram + e, out, sizeof out); e += 2)
			;
		if (b <= a - 16 || e >= a + 0x200 || e + sizeof out >= RAMSZ)
			continue;
		lp.lp_addr = b;
		lp.lp_len = 6 + sizeof out;
		lp.lp_data[0] = 0x4e;
		lp.lp_data[1] = 0xb9;
		put32(lp.lp_data + 2, (unsigned long)hc_serial);
		memcpy(lp.lp_data + 6, out, sizeof out);
		if (ioctl(fd, LISAIOC_PATCH, &lp) == 0)
			n++;
	}
	return n;
}

/* Lisa key codes by ADB key code; 0 none */
static unsigned char adb2lisa[128] = {
	0x70, 0x76, 0x7b, 0x69, 0x6b, 0x6a, 0x79, 0x7a,	/* a s d f h g z x */
	0x6d, 0x6c, 0x00, 0x6e, 0x75, 0x77, 0x60, 0x65,	/* c v ISO b q w e r */
	0x67, 0x66, 0x74, 0x71, 0x72, 0x73, 0x61, 0x64,	/* y t 1 2 3 4 6 5 */
	0x41, 0x50, 0x62, 0x40, 0x63, 0x51, 0x57, 0x5f,	/* = 9 7 - 8 0 ] o */
	0x52, 0x56, 0x53, 0x44, 0x48, 0x59, 0x54, 0x5b,	/* u [ i p ret l j ' */
	0x55, 0x5a, 0x42, 0x5d, 0x4c, 0x6f, 0x58, 0x5e,	/* k ; \ , / n m . */
	0x78, 0x5c, 0x68, 0x45, 0x46, 0x20, 0x00, 0x7f,	/* tab sp ` bs enter esc(clear) ctrl cmd */
	0x7e, 0x7d, 0x7c, 0x22, 0x23, 0x2b, 0x27, 0x00,	/* shift caps opt left right down up */
	0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20,	/* . clear */
	0x00, 0x00, 0x00, 0x00, 0x2f, 0x00, 0x21, 0x00,	/* enter - */
	0x00, 0x00, 0x49, 0x4d, 0x2d, 0x2e, 0x28, 0x29,	/* 0 1 2 3 4 5 */
	0x2a, 0x24, 0x00, 0x25, 0x26, 0x00, 0x00, 0x00,	/* 6 7 8 9 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0x00, 0x00, 0x00, 0x7e, 0x7c, 0x00, 0x00, 0x00	/* rshift ropt */
};

/* host input to the module's queue as Lisa key bytes (bit 7 down; the mouse button is 6) and mouse deltas */
static void
events(lf, kfd, mfd)
	int lf, kfd, mfd;
{
	struct inev ev[32];
	long dx = 0, dy = 0, x, y;
	int fd, n, i, k;

	for (fd = kfd; fd >= 0; fd = fd == kfd ? mfd : -1)
		while ((n = read(fd, (char *)ev, sizeof ev)) > 0)
			for (i = 0; i < n / (int)sizeof ev[0]; i++)
				switch (ev[i].ie_type) {
				case IE_KEY:
					if (ev[i].ie_code < 128 && (k = adb2lisa[ev[i].ie_code]) != 0)
						ioctl(lf, LISAIOC_INPUT, LIN_KEY | k | (ev[i].ie_value ? 0x80 : 0));
					break;
				case IE_BTN:
					if (ev[i].ie_code == 1)
						ioctl(lf, LISAIOC_INPUT, LIN_KEY | 6 | (ev[i].ie_value ? 0x80 : 0));
					break;
				case IE_REL:
					if (ev[i].ie_code == IE_RELX)
						dx += ev[i].ie_value;
					else
						dy += ev[i].ie_value;
					break;
				}
	while (dx || dy) {
		x = dx < -127 ? -127 : dx > 127 ? 127 : dx;
		y = dy < -127 ? -127 : dy > 127 ? 127 : dy;
		ioctl(lf, LISAIOC_INPUT, LIN_MOUSE | (x & 0xff) << 8 | (y & 0xff));
		dx -= x;
		dy -= y;
	}
}

/* the display process: the video page on the screen, and the boot's progress in the log */
static void
show(fd)
	int fd;
{
	struct nh_display d;
	struct fbcmap colors;
	unsigned short grey[256];
	struct lisastat st, was;
	unsigned char *scr, *row;
	pid_t pp = getppid();
	unsigned long lit, lastlit = 0;
	int x, y, i, n = 0, have = 0, up = 0, ev = 0;
	struct pollfd pf[2];

	close(vfd);
	nh_display_init(&d);
	if (nh_display_open(&d, "/dev/fb0", "Lisa", 1) < 0 || d.info.fi_depth != 8 ||
	    d.info.fi_width < SCRW || d.info.fi_height < SCRH)
		fprintf(stderr, "lisa: display (8 bits per pixel): %s\n", strerror(errno));
	else {
		for (i = 0; i < 256; i++)
			grey[i] = i * 257;
		memset(&colors, 0, sizeof colors);
		colors.cm_count = 256;
		colors.cm_red = colors.cm_green = colors.cm_blue = grey;
		ioctl(d.framebuffer, FBIOPUTCMAP, &colors);
		memset(d.mapping, 255, d.info.fi_size);
		up = 1;
	}
	memset(&was, 0, sizeof was);
	pf[0].fd = d.keyboard;
	pf[1].fd = d.mouse;
	pf[0].events = pf[1].events = POLLIN;
	for (;; n++) {
		if (poll(pf, 2, 50) > 0) {
			events(fd, d.keyboard, d.mouse);
			if (++ev % 4)
				continue;
		}
		if (kill(pp, 0) < 0)
			_exit(0);
		if (ioctl(fd, LISAIOC_STAT, &st) < 0)
			continue;
		if (!have++)
			fprintf(stderr, "lisa: running\n");
		if ((st.ls_flags & LSF_VIDEO) && st.ls_vid != was.ls_vid)
			fprintf(stderr, "lisa: video page %d\n", st.ls_vid);
		if ((st.ls_flags & LSF_PFREAD) && !(was.ls_flags & LSF_PFREAD))
			fprintf(stderr, "lisa: first volume read\n");
		if (st.ls_berr != was.ls_berr)
			fprintf(stderr, "lisa: bus error at %#lx, pc %#lx\n", st.ls_berraddr, st.ls_berrpc);
		if (st.ls_hwfault != was.ls_hwfault)
			fprintf(stderr, "lisa: hardware access at %#lx, pc %#lx\n", st.ls_hwaddr, st.ls_hwpc);
		if (verbose && n % 40 == 0)
			fprintf(stderr, "lisa: remaps %lu intr %lu blocks %lu last %lu faults %lu switches %lu a-line %lu alias %lu\n",
			    st.ls_remap, st.ls_intr, st.ls_pfreads, st.ls_pfblock,
			    st.ls_hwfault, st.ls_ctxsw, st.ls_aline, st.ls_alias);
		was = st;
		if (!(st.ls_flags & LSF_VIDEO) || (unsigned long)st.ls_vid * 0x8000 + SCRB > RAMSZ)
			continue;
		scr = (unsigned char *)PHYS + st.ls_vid * 0x8000UL;
		lit = 0;
		for (y = 0; y < SCRH; y++) {
			row = up ? (unsigned char *)d.mapping + d.info.fi_offset + y * d.info.fi_rowbytes : 0;
			for (x = 0; x < SCRW; x++)
				if (scr[y * (SCRW / 8) + x / 8] >> (7 - x % 8) & 1) {
					lit++;
					if (row)
						row[x] = 0;
				} else if (row)
					row[x] = 255;
		}
		if (lit != lastlit && n % 10 == 0) {
			lastlit = lit;
			fprintf(stderr, "lisa: screen %lu pixels lit\n", lit);
		}
	}
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct lisaenter le;
	unsigned long ssp, pc;
	int c, fd;
	pid_t pid;
	time_t now;
	struct tm *lt;
	char *root, *rompath = 0;

	while ((c = getopt(argc, argv, "d:p:r:v")) != -1)
		switch (c) {
		case 'd': disk = optarg; break;
		case 'r': rompath = optarg; break;
		case 'p': pmfile = optarg; break;
		case 'v': verbose = 1; break;
		default:
			fprintf(stderr, "usage: startlisa [-d volume] [-p parameters] [-r rom] [-v]\n");
			return 2;
		}
	if ((root = getenv("NATIVEHOST_ROOT")) == 0 || !*root) {
		fprintf(stderr, "startlisa: NATIVEHOST_ROOT names no environment folder\n");
		return 2;
	}
	if (!disk)
		disk = envpath(root, "profile.dc42");
	if (!pmfile)
		pmfile = envpath(root, "parameter-memory");
	memset(pm, 0xff, sizeof pm);
	if (pmfile && (fd = open(pmfile, O_RDONLY)) >= 0) {
		read(fd, pm, sizeof pm);
		close(fd);
	}
	if ((fd = lfd = open("/dev/lisa", O_RDWR)) < 0)
		die("/dev/lisa");
	if (ioctl(fd, LISAIOC_RAM, (caddr_t)RAMSZ) < 0)
		die("Lisa RAM");
	if (mmap((caddr_t)PHYS, RAMSZ, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == (caddr_t)-1 ||
	    mmap((caddr_t)ROMVA, LISA_ROMSZ, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd,
	    (off_t)LISA_ROMOFF) == (caddr_t)-1)
		die("map");
	romp = (unsigned char *)ROMVA;
	romload(rompath ? rompath : envpath(root, "lisa.rom"));
	romfix();
	if (vopen() < 0 || fbufinit() < 0)
		die(disk);
	if (oopen(envpath(root, "profile.writes")) < 0)
		die("profile.writes");
	if ((pid = fork()) < 0)
		die("fork");
	if (pid == 0)
		show(fd);
	le.le_phys = PHYS;
	le.le_rom = ROMVA;
	le.le_hostlo = (unsigned long)_start;
	le.le_hosthi = (unsigned long)&etext;
	time(&now);
	lt = localtime(&now);
	le.le_tzoff = -timezone + (lt->tm_isdst ? 3600 : 0);
	/* the module's interrupts come as SIGUSR2, which must be caught */
	{
		struct sigaction sa;

		memset((char *)&sa, 0, sizeof sa);
		sa.sa_handler = nothing;
		sa.sa_flags = SA_NODEFER;
		if (sigaction(SIGUSR2, &sa, (struct sigaction *)0) < 0)
			die("SIGUSR2");
	}
	if (ioctl(fd, LISAIOC_ENTER, &le) < 0)
		die("enter");
	/* reset: the stack and the entry from the ROM's first two longs */
	pc = 0xfe0000 | RW(6);
	ssp = RW(2);
	__asm__ __volatile__("move.l %0,%%a1; move.l %1,%%a0; move.l %%a0,%%sp; suba.l %%a0,%%a0; jmp (%%a1)"
	    : : "g" (pc), "g" (ssp) : "a0", "a1");
	return 1;
}
