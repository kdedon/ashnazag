/*
 * startlisa -- run Lisa OS from the user's volume as this process.
 *
 *	startlisa [-e env [-b base]] [-d env] -- [-i volume] [-p parameters] [-r rom] [-s screen] [-v]
 *
 * Boots the user's Lisa ROM ($NATIVEHOST_ROOT/lisa.rom) from the files of
 * their volume in that folder: the boot block and loader from .boot, the
 * loader's files by name.  The OS's own I/O goes to the ProFile volume
 * (profile.dc42, DC42 or raw 532-byte blocks, opened read only; written
 * blocks go to profile.writes).  Routines of the ROM copy and the
 * loaded code that drive hardware are found by structure and patched to
 * call into this process (host calls); the Lisa's chips are not modelled.
 * A child process shows the video page on the display.  Parameter memory
 * is kept in a file, by default parameter-memory in the environment's
 * folder.  The screen is drawn on the host's display as it is, or on the
 * mode nearest a requested size (-s WxH, or lisa for the Lisa's own at the
 * host's aspect; by default the folder's file screen holds it).
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
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include "hostcall.h"
#include "lisaio.h"
#include "nativehost.h"

extern char etext, _end;
extern void _start();

#define	PHYS	0x20000000UL		/* the launcher's view of Lisa RAM */
#define	ROMVA	0x20400000UL
#define	RAMSZ	LISA_RAMMAX
#define	SYSAREA	0x8000UL		/* physical base of logical 0: the ROM keeps the first 32 KB */
#define	SCRW	720
#define	SCRH	364
#define	BLK	532			/* a volume block: 20 tag bytes, 512 data */

static char *disk;			/* the ProFile image, never written */
static char *pmfile;			/* parameter memory, 64 bytes */
static int curctx = -1;			/* the module's user context */
static unsigned char pm[64];
static int verbose;
static unsigned long serial = 5L << 16 | 60100;	/* the machine's serial number, 005-60100 */
static int lfd;				/* /dev/lisa */
static char *screen;			/* the display size to request */


static char fstack[8192];		/* signal handlers' stack */

static void
nothing(s)
	int s;
{
}

#define	STOPSECS	120		/* for the OS to shut down */

/*
 * The environment ended from outside: the soft power switch, so the OS
 * shuts down and leaves its volume clean.
 */
static void
stopreq(s)
	int s;
{
	static int asked;

	if (s == SIGALRM) {
		fprintf(stderr, "startlisa: the Lisa did not shut down in %d s; its volume was not closed\n", STOPSECS);
		_exit(1);
	}
	if (asked++)
		return;
	if (verbose)
		fprintf(stderr, "lisa: shutting down\n");
	ioctl(lfd, LISAIOC_INPUT, LIN_POWER);
	alarm(STOPSECS);
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
static int vsaid;
static unsigned char vmddf[512];		/* the volume's, from .volume */
static long vover = -1;			/* pages in use that are no file's: the volume's own */
static long volblocks;
static char volname[64];

static int
vread(b, p)
	long b;
	unsigned char *p;
{
	if (!vsaid++)
		fprintf(stderr, "lisa: ProFile block %ld read\n", b);
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

	if (!vsaid++)
		fprintf(stderr, "lisa: ProFile block %ld written\n", b);
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
 * Host calls (hostcall.h): patched guest code does `jsr hc_x`.  Registers
 * the service changes in r[] go back to the guest; hc_fdin also sets the
 * flags from D0.
 */
HC_ENTRY
enum { HC_READ, HC_PFID, HC_XFER, HC_MMU, HC_SEG1, HC_SERIAL, HC_IRQON, HC_IRQ1, HC_CLOCK, HC_VIDEO, HC_PMRD, HC_PMWR, HC_INPUT, HC_UNPACK, HC_TAKE, HC_CTX, HC_FDTYPE, HC_FDIN1, HC_RESET, HC_STOP, HC_VID0, HC_ROUND, HC_FDCMD, HC_LMEDIA,
    HC_LOPEN, HC_LFILL, HC_LMOVE, HC_FSLIST, HC_FSPAGE, HC_FSDATA,
    HC_CREL, HC_CGET, HC_CSRCH, HC_CPTR, HC_CNEXT, HC_CLOOK, HC_CINS, HC_CDEL, HC_COPEN, HC_CENUM, HC_MDDF, HC_BMAP, HC_POLL, HC_POWER, HC_MOVE, HC_SEGSZ, HC_SHRINK, HC_IDLE };
#define	hc_read	HC_STUB(HC_READ)
#define	hc_pfid	HC_STUB(HC_PFID)
#define	hc_xfer	HC_STUB(HC_XFER)
#define	hc_mmu	HC_STUB(HC_MMU)
#define	hc_seg1	HC_STUB(HC_SEG1)
#define	hc_serial	HC_STUB(HC_SERIAL)
#define	hc_irqon	HC_STUB(HC_IRQON)
#define	hc_irq1	HC_STUB(HC_IRQ1)
#define	hc_clock	HC_STUB(HC_CLOCK)
#define	hc_video	HC_STUB(HC_VIDEO)
#define	hc_power	HC_STUB(HC_POWER)
#define	hc_pmrd	HC_STUB(HC_PMRD)
#define	hc_pmwr	HC_STUB(HC_PMWR)
#define	hc_input	HC_STUB(HC_INPUT)
#define	hc_unpack	HC_STUB(HC_UNPACK)
#define	hc_take	HC_STUB(HC_TAKE)
#define	hc_ctx	HC_STUB(HC_CTX)
#define	hc_fdtype	HC_STUB(HC_FDTYPE)
#define	hc_reset	HC_STUB(HC_RESET)
#define	hc_stop	HC_STUB(HC_STOP)
#define	hc_vid0	HC_STUB(HC_VID0)
#define	hc_round	HC_STUB(HC_ROUND)
#define	hc_lmedia	HC_STUB(HC_LMEDIA)
#define	hc_fslist	HC_STUB(HC_FSLIST)
#define	hc_fspage	HC_STUB(HC_FSPAGE)
#define	hc_fsdata	HC_STUB(HC_FSDATA)
extern void hc_fdin(), hc_fdcmd(), hc_lopen(), hc_lfill(), hc_lmove();

void hc_fdinbody() { __asm__("\n\
	.text\n\
	.even\n\
hc_fdin:\n\
	jsr hc_stubs+102\n\
	tst.b %d0\n\
	rts\n\
hc_fdcmd:\n\
	jsr hc_stubs+132\n\
	move.l (%sp)+,%a0\n\
	addq.l &4,%sp\n\
	jmp (%a0)\n\
hc_lopen:\n\
	jsr hc_stubs+144\n\
	move.l (%sp)+,(%sp)\n\
	rts\n\
hc_lfill:\n\
	jsr hc_stubs+150\n\
	move.l (%sp)+,(%sp)\n\
	rts\n\
hc_lmove:\n\
	jsr hc_stubs+156\n\
	move.l (%sp)+,%a0\n\
	addq.l &8,%sp\n\
	jmp (%a0)\n"); }

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
 *    the driver's completion call (done);
 *  - its shutdown, the port's interrupts disabled (ier) once the OS has
 *    closed the volume at power off, becomes `jsr hc_power`: the end.
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
	static unsigned char nop[] = { 0x4e, 0x71 }, pwr[6] = { 0x4e, 0xb9 };
	static unsigned char ier[] = { 0x20, 0x5f, 0x22, 0x5f, 0x13, 0x7c, 0x00, 0x3b, 0x00, 0x70, 0x4e, 0xd0 };
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
	put32(pwr + 2, (unsigned long)hc_power);
	if ((t = find1(f, n, ier, sizeof ier)) >= 0)
		addpat(fid, page0, f, (int)t, pwr, sizeof pwr);
	if (verbose)
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
 * The poll that runs the same sources and the keyboard with interrupts
 * masked becomes the same loop, then a call to the keyboard decoder, which
 * takes the next input event itself.
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
	static unsigned char psig[] = { 0x48, 0xe7, 0x80, 0x80, 0x40, 0xe7, 0x00, 0x7c, 0x07, 0x00, 0x08, 0x39,
	    0x00, 0x02, 0x00, 0xfc, 0xf8, 0x01, 0x66, 0x04, 0x4e, 0xba };
	static unsigned char ptsig[] = { 0x10, 0x39, 0x00, 0xfc, 0xd9, 0x69, 0x08, 0x00, 0x00, 0x06, 0x67, 0x04, 0x4e, 0xba };
	static unsigned char ksig[] = { 0x08, 0x39, 0x00, 0x01, 0x00, 0xfc, 0xdd, 0x9b, 0x67, 0x04, 0x4e, 0xba };
	static unsigned char p[32] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    0x4e, 0xba, 0, 0, 0x60, 0x0c };
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
	if (verbose)
		fprintf(stderr, "lisa: level 1 interrupts in file %d: host sources\n", fid);
	/* the poll: register save, sr, then the three tests from 10 on, its sr restore at 54 */
	if ((a = find1(f, n, psig, sizeof psig)) < 0 || a + 56 > n || memcmp(f + a + 24, ptsig, sizeof ptsig) ||
	    memcmp(f + a + 40, ksig, sizeof ksig) || !word(f + a + 54, 0x46df))
		return;
	k = a + 52 + (short)(f[a + 52] << 8 | f[a + 53]);
	a += 10;
	memcpy(p, h + 4, 26);
	e = v - (a + 12);
	p[12] = e >> 8;
	p[13] = e;
	e = tm - (a + 22);
	p[22] = e >> 8;
	p[23] = e;
	e = k - (a + 28);
	p[28] = e >> 8;
	p[29] = e;
	addpat(fid, page0, f, (int)a, p, sizeof p);
	if (verbose)
		fprintf(stderr, "lisa: interrupt poll in file %d: host sources\n", fid);
}

/*
 * Other low-level driver routines that drive hardware:
 *  - the clock read sends the keyboard controller a command and polls for
 *    its answer: `jsr hc_clock; rts`, the host's local time;
 *  - the video latch setter: its byte store becomes `jsr hc_video`, D1 the page;
 *  - the contrast ramp's latch writes are skipped: its machine-type test
 *    branches to the common tail;
 *  - the fine time read adds the timer's count to the tick count: the count
 *    reads as just reloaded, so the time is the tick count's;
 *  - the speaker: the tone's start returns at once and its stop skips the
 *    VIA write, so beeps keep their length in silence; the volume reads 4;
 *  - power off, now or at a time (the screen dimmed, then the controller's
 *    power-off command, in a loop): `jsr hc_power`, which ends the environment.
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
	static unsigned char onsig[] = { 0x20, 0x7c, 0x00, 0xfc, 0xdd, 0x81, 0x11, 0x40, 0x00, 0x10 };
	static unsigned char offsig[] = { 0x02, 0x39, 0x00, 0xe3, 0x00, 0xfc, 0xdd, 0x97, 0x4e, 0x75 };
	static unsigned char volsig[] = { 0x10, 0x39, 0x00, 0xfc, 0xdd, 0x81, 0xe2, 0x48, 0x02, 0x40, 0x00, 0x07, 0x4e, 0x75 };
	static unsigned char rts[] = { 0x4e, 0x75 }, vol[] = { 0x70, 0x04, 0x4e, 0x75 };
	static unsigned char nops[] = { 0x4e, 0x71, 0x4e, 0x71, 0x4e, 0x71, 0x4e, 0x71 };
	static unsigned char clk[8] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x4e, 0x75 }, vid[6] = { 0x4e, 0xb9 }, bra[2] = { 0x60 };
	static unsigned char pwsig[] = { 0x48, 0xe7, 0xc0, 0x00, 0x30, 0x3c, 0x00, 0xff, 0x4e, 0xba };
	static unsigned char pwr[6] = { 0x4e, 0xb9 };
	long a, t;

	put32(pwr + 2, (unsigned long)hc_power);
	for (a = 0; a + 22 <= n; a += 2)	/* movem; move.w #$ff,d0; jsr; jsr; cmpi.w #$fff,d0; bne */
		if (!memcmp(f + a, pwsig, sizeof pwsig) && f[a + 12] == 0x4e && f[a + 13] == 0xba &&
		    f[a + 16] == 0x0c && f[a + 17] == 0x40 && f[a + 18] == 0x0f && f[a + 19] == 0xff && f[a + 20] == 0x66)
			addpat(fid, page0, f, (int)a, pwr, sizeof pwr);

	if ((a = find1(f, n, csig, sizeof csig)) >= 14 && f[a - 14] == 0x42 && f[a - 14 + 1] == 0x39) {
		put32(clk + 2, (unsigned long)hc_clock);
		addpat(fid, page0, f, (int)a - 14, clk, sizeof clk);
		if (verbose)
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
	if ((a = find1(f, n, onsig, sizeof onsig)) >= 0) {
		for (t = a - 2; t > a - 0x40 && t >= 0 && !word(f + t, 0x48e7); t -= 2)
			;
		if (t > a - 0x40 && t >= 0)
			addpat(fid, page0, f, (int)t, rts, sizeof rts);
	}
	if ((a = find1(f, n, offsig, sizeof offsig)) >= 0)
		addpat(fid, page0, f, (int)a, nops, sizeof nops);
	if ((a = find1(f, n, volsig, sizeof volsig)) >= 0)
		addpat(fid, page0, f, (int)a, vol, sizeof vol);
}

/*
 * The keyboard controller's byte decoder is entered from the level 2
 * handler once the VIA flags a byte.  The handler calls it every time and
 * the decoder takes the next input event by host call instead of the byte:
 *
 *	jsr hc_input; tst.l d0; bmi.s x; btst #16,d0; bne.s m; btst #17,d0; bne.s p
 *	bra.w key; m: bra.w mouse; p: bra.w power; x: rts
 *
 * `key` is the decoder's key path (D0 the byte), `mouse` its last mouse
 * byte's (D0 dy, dx in the variable its previous state stores), `power`
 * its reset code state's case for the soft power switch ($FB):
 *
 *	cmpi.b #$df,d0; bls.s; cmpi.b #$ef,d0; bls.s; cmpi.b #$fb,d0; bcs.w; beq.s power
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
	static unsigned char d[] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x4a, 0x80, 0x6b, 0x18, 0x08, 0x00, 0x00, 0x10,
	    0x66, 0x0a, 0x08, 0x00, 0x00, 0x11, 0x66, 0x08, 0x60, 0x00, 0, 0, 0x60, 0x00, 0, 0, 0x60, 0x00, 0, 2,
	    0x4e, 0x75 };
	static unsigned char rsig[] = { 0x0c, 0x00, 0x00, 0xdf, 0x63 };
	static unsigned char skip[] = { 0x60, 0x08 };
	long a, h, idle, s1, s2, k, rc, pw = -1;

	if ((a = find1(f, n, dsig, sizeof dsig)) < 0 || a + 0x38 > n)
		return;
	for (h = 0; h + 20 <= n; h += 2)	/* the handler: the poll that calls the decoder, then rte */
		if (!memcmp(f + h, hsig, sizeof hsig) && h + 12 + (short)(f[h + 12] << 8 | f[h + 13]) == a &&
		    word(f + h + 18, 0x4e73))
			break;
	if (h + 20 > n || f[a + 0x0e] != 0x67 || !word(f + a + 0x10, 0x0c39) || !word(f + a + 0x12, 1) || !word(f + a + 0x18, 0x6700) ||
	    !word(f + a + 0x1c, 0x0c39) || !word(f + a + 0x1e, 2) || !word(f + a + 0x24, 0x6700))
		return;
	idle = a + 0x10 + (signed char)f[a + 0x0f];
	s1 = a + 0x1a + (short)(f[a + 0x1a] << 8 | f[a + 0x1b]);
	s2 = a + 0x26 + (short)(f[a + 0x26] << 8 | f[a + 0x27]);
	if (idle < 0 || idle + 10 > n || memcmp(f + idle, ksig, sizeof ksig) || !word(f + idle + 4, 0x0c00) ||
	    s1 < 0 || s1 + 6 > n || !word(f + s1, 0x13c0) || s2 < 0 || s2 > n)
		return;
	k = idle + 10;
	/* the last state's branch, to the reset code's decoder */
	rc = a + 0x36 + (short)(f[a + 0x36] << 8 | f[a + 0x37]);
	if (word(f + a + 0x28, 0x0c39) && word(f + a + 0x2a, 3) && word(f + a + 0x30, 0x6700) && word(f + a + 0x34, 0x6000) &&
	    rc >= 0 && rc + 22 <= n && !memcmp(f + rc, rsig, sizeof rsig) && word(f + rc + 6, 0x0c00) &&
	    word(f + rc + 8, 0xef) && word(f + rc + 12, 0x0c00) && word(f + rc + 14, 0xfb) && word(f + rc + 16, 0x6500) &&
	    f[rc + 20] == 0x67)
		pw = rc + 22 + (signed char)f[rc + 21];
	mousedx = (unsigned long)f[s1 + 2] << 24 | f[s1 + 3] << 16 | f[s1 + 4] << 8 | f[s1 + 5];
	put32(d + 2, (unsigned long)hc_input);
	d[24] = (k - (a + 24)) >> 8;
	d[25] = k - (a + 24);
	d[28] = (s2 - (a + 28)) >> 8;
	d[29] = s2 - (a + 28);
	if (pw >= 0) {
		d[32] = (pw - (a + 32)) >> 8;
		d[33] = pw - (a + 32);
	}
	addpat(fid, page0, f, (int)a, d, sizeof d);
	addpat(fid, page0, f, (int)h, skip, sizeof skip);
	if (verbose)
		fprintf(stderr, "lisa: keyboard and mouse in file %d: host input%s\n", fid, pw >= 0 ? ", power switch" : "");
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
	if (verbose)
		fprintf(stderr, "lisa: hardware setup in file %d skipped\n", fid);
}

/*
 * The micro diskette driver keeps whether a disk is in place in its drive
 * state (byte 15), set from the controller.  Its status call and its I/O
 * check read it by host call: no disk.  Its controller command routine
 * becomes a host call that gives every command no result and no error.
 */
static void
sonydrv(fid, page0, f, n)
	int fid, page0;
	unsigned char *f;
	long n;
{
	static unsigned char st[] = { 0x20, 0x47, 0x10, 0x28, 0x00, 0x0f, 0x48, 0x80, 0x48, 0xc0, 0x29, 0x40, 0x00, 0x04 };
	static unsigned char io[] = { 0x20, 0x47, 0x4a, 0x28, 0x00, 0x0f, 0x66, 0x08, 0x3d, 0x7c };
	/* the command routine: clears the command's result and status, loads the controller's address */
	static unsigned char cmd[] = { 0x48, 0xe7, 0x3c, 0x38, 0x28, 0x6f, 0x00, 0x20, 0x26, 0x6c, 0x00, 0x04,
	    0x42, 0x6b, 0x00, 0x08, 0x42, 0x6b, 0x00, 0x0a, 0x24, 0x53 };
	static unsigned char j[6] = { 0x4e, 0xb9 }, jm[6] = { 0x4e, 0xf9 };
	long a, b, c;

	if ((a = find1(f, n, st, sizeof st)) < 0 || (b = find1(f, n, io, sizeof io)) < 0 ||
	    (c = find1(f, n, cmd, sizeof cmd)) < 0)
		return;
	put32(j + 2, (unsigned long)hc_fdin);
	addpat(fid, page0, f, (int)a, j, sizeof j);
	addpat(fid, page0, f, (int)b, j, sizeof j);
	put32(jm + 2, (unsigned long)hc_fdcmd);
	addpat(fid, page0, f, (int)c, jm, sizeof jm);
	if (verbose)
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
 *    page: the new segment's size (D7, by host call in place of the test in
 *    its check that the segment has no memory yet), a resize's new size (the
 *    same, stored back; its byte-sized code shortened to make room, and its
 *    shrink skipped by host call when the rounded size is unchanged), the
 *    size it takes from the top of free memory at start (by host call), the
 *    jump table page's (4 KB), the size of a segment moved into the free
 *    region next to it (by host call: the loader's code segments keep their
 *    byte sizes, and the region left shrinks by what it gains), and a new stack
 *    keeps the page it would give back;
 *  - the drivers' code space grows its segment by each driver's size and
 *    fails unless the segment has exactly that size, so the size rounds up
 *    to 4 KB (by host call) in place of 512 bytes;
 *  - Yield_CPU with no other process ready waits in the host for an
 *    interrupt: an idle application's event loop yields on every pass.
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
/* a patched routine's first instructions, then a jump back to the rest */
static unsigned short ctramp[12][12];
static short ctlen[12];
static long cpra5, cdva5;	/* off A5, from the code: the process's listing state, the device table */

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
	static int csp[] = { 0x42ae, 0x0010, 0x206e, 0x000c, 0x30bc, 0x0001, 0x42a7, 0x2f2e, 0x0008, 0x4eba, -1,
	    0x2d5f, 0x0008, 0x2f2e, 0x0008, 0x2f3c, 0x0000, 0x0200 };
	static unsigned char unp[] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x20, 0x5f, 0xde, 0xfc, 0x00, 0x14, 0x4e, 0xd0 };
	static unsigned char tk[] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x4e, 0x71, 0x4e, 0x71 };
	static unsigned char pmc[] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x4e, 0xd0 };
	static const unsigned char ctx[] = { 0x20, 0x5f, 0x12, 0x1f, 0x08, 0x01, 0x00, 0x00, 0x66, 0x00, 0x00, 0x0c,
	    0x13, 0xc0, 0x00, 0xfc, 0xe0, 0x08 };
	static const unsigned char fdt[] = { 0x2a, 0x3c, 0x00, 0xfc, 0xc0, 0x31, 0x20, 0x45, 0x4a, 0x10, 0x6d, 0x06, 0x42, 0x6d };
	static const unsigned char scc[] = { 0x10, 0x10, 0x4e, 0x71, 0x10, 0xbc, 0x00, 0x09, 0x4e, 0x71, 0x10, 0xbc };
	static const unsigned char slot[] = { 0x20, 0x5f, 0x22, 0x78, 0x00, 0x08, 0x45, 0xfa, 0x00, 0x14, 0x21, 0xca, 0x00, 0x08,
	    0x24, 0x5f, 0x10, 0x12, 0x1e, 0xbc, 0x00, 0x01 };
	/* the file layer: s-list entry, bytes of a page, file data */
	static int fsl[] = { 0x4e56, -1, 0x48e7, -1, 0x3e2e, 0x000e, 0x4a47, 0x6c0a, 0x206e, 0x0012, 0x30bc, -1, -1,
	    0x302e, 0x0010, 0x41ed, -1, 0xe540 };
	static int fsh[] = { 0x4e56, -1, 0x48e7, -1, 0x302e, 0x0016, 0x41ed, -1, 0xe540, 0x2d70, 0x0000, -1,
	    0x0cae, 0x0041, 0x4231 };
	/* in the file data routine, past its checks and any extension: the transfer, and its end's bookkeeping */
	static int fsd[] = { 0x4aae, 0x0016, 0x6f00, -1, 0x2d6e, 0x0016, -1, 0x2d6e, 0x001a, -1, 0x206e, 0x000a, 0x302e, -1,
	    0x9050 };
	static int fse[] = { 0x2f0e, 0x4eba, -1, 0x206e, 0x0022, 0x0c50, 0x033a };
	static unsigned char fsj[] = { 0x4e, 0xb9, 0, 0, 0, 0, 0x20, 0x5f, 0xde, 0xfc, 0, 0, 0x4e, 0xd0 };
	/* the catalog module's entry points, the MDDF's and the bitmap's I/O */
	static int crel[] = { 0x4e56, -1, 0x206e, 0x0008, 0x4228, 0x0009, 0x206e, 0x0008, 0x4868, 0x0010, 0x4ead, -1,
	    0x4e5e, 0x2e9f, 0x4e75 };
	static int cget[] = { 0x4e56, -1, 0x48e7, -1, 0x206e, 0x0008, 0x4250, 0x49ee, -1, 0x422c, 0x0003, 0x026c, 0x3fff,
	    0x0002, 0x4254, 0x397c, 0x0004, 0x0004, 0x397c, 0x0200 };
	static int csrch[] = { 0x4e56, -1, 0x48e7, -1, 0x282e, 0x0018, 0x206e, 0x0008, 0x4250, 0x206e, 0x000c, 0x4210,
	    0x2044, 0x2068, 0x0036, 0x0ca8, 0xffff, 0xffff, 0x012e };
	static int cptr[] = { 0x4e56, -1, 0x48e7, -1, 0x2f2e, 0x001c, 0x2f2e, 0x0018, 0x2f2e, 0x0014, 0x2f2e, 0x000c,
	    0x2f2e, 0x0008, 0x4eba, -1, 0x206e, 0x0008, 0x4a50, 0x6e46, 0x206e, 0x0014, 0x2050, 0x41e8 };
	static int cnext[] = { 0x4e56, -1, 0x48e7, -1, 0x282e, 0x0010, 0x206e, 0x0008, 0x4250, 0x206e, 0x0014, 0x2050,
	    0x41e8, 0x0018, 0x2648, 0x284b, 0xd8fc, 0x07f4, 0x200c, 0x5580 };
	static int clook[] = { 0x4e56, -1, 0x48e7, -1, 0x206e, 0x001c, 0x43ee, 0xff00, 0x4a10, 0x7040, 0x22d8, 0x5340,
	    0x6efa, 0x286e, 0x0010, 0x3e2e, 0x0020, 0x282e, 0x0018, 0x2d6d };
	static int cins[] = { 0x4e56, -1, 0x48e7, -1, 0x286e, 0x0016, 0x2c2e, 0x0012, 0x3a2e, 0x0010, 0x206e, 0x0008,
	    0x4250, 0x266c, 0x0036, 0x4aae, 0x000c, 0x6642 };
	static int cdel[] = { 0x4e56, -1, 0x48e7, -1, 0x206e, 0x0008, 0x4250, 0x206e, 0x0018, 0x2d68, 0x0036, -1,
	    0x4aae, 0x0010, 0x6642, 0x2f2e, 0x0018, 0x2f2e };
	static int copen[] = { 0x4e56, -1, 0x48e7, -1, 0x266e, 0x0012, 0x206e, 0x0008, 0x4250, 0x286d, -1, 0x296b,
	    0x006e, 0x00e6, 0x486b, 0x005e, 0x3f3a, -1 };
	static int cenum[] = { 0x4e56, -1, 0x48e7, -1, 0x206e, 0x0008, 0x4250, 0x422e, -1, 0x2d6d, -1, -1, 0x206e, -1,
	    0x3028, 0x00b0, 0x41ed, -1 };
	static int vmd[] = { 0x4e56, -1, 0x302e, 0x000e, 0x41ed, -1, 0xe540, 0x2070, 0x0000, 0x2068, 0x0036, 0x2d48, -1,
	    0x2f2e, 0x0010, 0x3f2e, 0x000e, 0x3f3c, 0x0001, 0x206e };
	/* a 1 s timer's test of an I/O board status bit */
	static int pol[] = { 0x707e, 0x2f00, 0x2f3c, 0x0002, 0x0000, 0x4ead, -1, 0x201f, 0xd0bc, 0x0000, 0xd801, 0x2d40, -1,
	    0x206e, -1, 0x0810, 0x0006, 0x56c0 };
	static int vbm[] = { 0x4e56, -1, 0x2f0c, 0x302e, 0x000e, 0x41ed, -1, 0xe540, 0x2070, 0x0000, 0x2868, 0x0036,
	    0x2f2e, 0x0010, 0x3f2e, 0x000e, 0x3f3c, 0x0002, 0x2f2c, 0x0088 };
	/*
	 * Yield_CPU after its queue work: whether to reschedule, to the next
	 * process in the ready queue if it may run
	 */
	static int yld[] = { 0x102b, -1, 0x6704, 0x422b, -1, 0x3f3c, 0x0700, 0x486e, -1, 0x4ead, -1, 0x2f0c, 0x4267,
	    0x4ead, -1, 0x3f2e, -1, 0x4ead, -1, 0x102d, -1, 0x0a00, 0x0001, 0xc02e, 0x0008, 0x6712, 0x2854, 0x4a6c,
	    0x000c, 0x6f0a, 0x2b4c, -1, 0x1b7c, 0x0001, -1, 0x102d, -1, 0x670e, 0x202d, -1, 0xb0ad, -1, 0x6604, 0x42ad, -1 };
	/* a segment moved into free memory: its size and old place */
	static int mvs[] = { 0x2f2c, 0xfff2, 0x1f3c, 0x0001, 0x4eba, -1, 0x206c, 0xfffa, 0x3e28, 0x000a, 0x206c, 0xfffa,
	    0x3828, 0x0008, 0x206c, 0xfff2, 0x3a28, 0x000a };
	/* in HC_ order; len: the instructions the patch covers, run from ctramp for other volumes */
	static struct { int *sig, n, args, len; } cpt[] = {
		{ crel, 15, 4, 16 }, { cget, 20, 18, 14 }, { csrch, 19, 20, 16 }, { cptr, 24, 24, 16 },
		{ cnext, 20, 20, 16 }, { clook, 20, 30, 16 }, { cins, 18, 18, 16 }, { cdel, 18, 20, 14 },
		{ copen, 18, 14, 16 }, { cenum, 18, 16, 14 }, { vmd, 20, 12, 14 }, { vbm, 20, 12, 14 } };
	int j;
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
		if (a + 2 * 15 < n && wmatch(p, fsd, 15)) {
			for (j = 0; j < 0x1000 && a + j + 14 < n && !wmatch(p + j, fse, 7); j += 2)
				;
			if (j < 0x1000 && a + j + 14 < n) {
				/* jsr hc_fsdata; bra.w to the bookkeeping */
				memcpy(p, "\x4e\xb9....\x60\x00", 8);
				put32(p + 2, hc_fsdata);
				p[8] = (j + 6 - 8) >> 8;
				p[9] = j + 6 - 8;
				k++;
			}
			continue;
		}
		if (a + 2 * 18 < n && (wmatch(p, fsl, 18) || wmatch(p, fsh, 15))) {
			/* jsr hc_x; movea.l (sp)+,a0; adda.w #args,sp; jmp (a0) */
			put32(fsj + 2, wmatch(p, fsl, 18) ? hc_fslist : hc_fspage);
			fsj[11] = wmatch(p, fsl, 18) ? 14 : 20;
			memcpy(p, fsj, sizeof fsj);
			k++;
			continue;
		}
		for (j = 0; j < sizeof cpt / sizeof cpt[0] && !(a + 2 * cpt[j].n < n && wmatch(p, cpt[j].sig, cpt[j].n)); j++)
			;
		if (j < sizeof cpt / sizeof cpt[0]) {
			memcpy(ctramp[j], p, cpt[j].len);
			ctramp[j][cpt[j].len / 2] = 0x4ef9;
			ctlen[j] = cpt[j].len;
			if (HC_CREL + j == HC_COPEN || HC_CREL + j == HC_CENUM)
				cpra5 = (short)G16P(p + 20);
			else if (HC_CREL + j == HC_MDDF)
				cdva5 = (short)G16P(p + 10);
			put32(fsj + 2, HC_STUB(HC_CREL + j));
			fsj[11] = cpt[j].args;
			memcpy(p, fsj, sizeof fsj);
			k++;
		} else if (a + 2 * 18 < n && wmatch(p, pol, 18)) {
			/* jsr hc_poll for the btst/sne */
			memcpy(p + 26, "\x4e\xb9....\x4e\x71\x4e\x71", 10);
			put32(p + 28, HC_STUB(HC_POLL));
			k++;
		} else if (mk >= 0 && a + 2 * 19 < n && wmatch(p, stb, 18) && word(p + 36, G16P(p + 2)) && callsto(c, n, a + 38, mk)) {
			p[8] = p[18] = p[28] = 0x10;	/* a new process's stack size: whole 4 KB */
			k++;
		} else if (a + 2 * 22 < n && wmatch(p, jtp, 22)) {
			p[5] = p[33] = 8;		/* the jump table page: 4 KB */
			p[40] = 0x10;
			p[41] = 0;
			k++;
		} else if (a + 70 < n && wmatch(p, mvs, 18) && word(p + 36, 0x4a2e) && word(p + 66, 0x48c4)) {
			/* jsr hc_move; bra.w to the copy */
			memcpy(p + 16, "\x4e\xb9....\x60\x00\x00\x2a", 10);
			put32(p + 18, HC_STUB(HC_MOVE));
			k++;
		} else if (a + 2 * 45 <= n && wmatch(p, yld, 45) && word(p + 8, G16P(p + 2)) && word(p + 32, G16P(p + 16)) &&
		    word(p + 68, G16P(p + 40)) && word(p + 72, G16P(p + 40))) {
			/*
			 * clr.b x(a3) in place of its test makes room at the end:
			 * tst.b flag(a5); bne.s A; tst.b to_any(a6); beq.s ret
			 * movea.l (a4),a4; tst.w $c(a4); bgt.s set; jsr hc_idle; bra.s A
			 * set: move.l a4,next(a5); move.b #1,flag(a5)
			 * A: tst.b flag(a5); beq.s ret
			 * move.l x(a5),d0; cmp.l y(a5),d0; bne.s ret; clr.l z(a5)
			 */
			static int w[] = { 0x4a2d, -20, 0x6620, 0x4a2e, 0x0008, 0x672e, 0x2854, 0x4a6c, 0x000c, 0x6e08,
			    0x4eb9, 0, 0, 0x600a, 0x2b4c, -31, 0x1b7c, 0x0001, -20, 0x4a2d, -20, 0x670e,
			    0x202d, -39, 0xb0ad, -41, 0x6604, 0x42ad, -44 };	/* -n: word n of the original */
			unsigned short o[45];
			int v;

			for (j = 0; j < 45; j++)
				o[j] = G16P(p + 2 * j);
			p[0] = 0x42;
			p[1] = 0x2b;
			memmove(p + 4, p + 10, 28);
			for (j = 0; j < 29; j++) {
				v = w[j] < 0 ? o[-w[j]] : w[j];
				p[32 + 2 * j] = v >> 8;
				p[33 + 2 * j] = v;
			}
			put32(p + 54, HC_STUB(HC_IDLE));
			k++;
		} else if (a + 2 * 18 < n && wmatch(p, csp, 18)) {
			/* move.l size(a6),-(sp); jsr hc_round */
			memcpy(p + 12, "\x2f\x2e\x00\x08\x4e\xb9", 6);
			put32(p + 18, (unsigned long)hc_round);
			k++;
		} else if (a + 2 * 18 < n && wmatch(p, top, 18)) {
			memcpy(p + 8, tk, sizeof tk);
			k++;
		} else if (a + 0xcc < n && !memcmp(p, rsz, sizeof rsz) && word(p + 0xc2, 0x6042) && word(p + 0xc4, 0x382c) &&
		    word(p + 0xc6, 8) && word(p + 0xc8, 0x1005)) {
			memcpy(p, rsz2, sizeof rsz2);
			memcpy(p + 0xc4, "\x4e\xb9", 2);	/* the shrink: jsr hc_shrink */
			put32(p + 0xc6, HC_STUB(HC_SHRINK));
			k++;
		} else if (a + 38 < n && !memcmp(p, stk, sizeof stk) && word(p + 36, 0x603e)) {
			memcpy(p + 8, "\x60\x5a", 2);		/* bra past the give-back */
			k++;
		} else if (!memcmp(p, unpsig, sizeof unpsig)) {
			memcpy(p + 6, unp, sizeof unp);
			k++;
		} else if (!memcmp(p, seg, sizeof seg) && word(p + 14, 0x4a6c) && word(p + 16, 8) && word(p + 18, 0x6708)) {
			memcpy(p, "\x4e\x71", 2);		/* no spare page over a stack */
			memcpy(p + 14, "\x4e\xb9", 2);	/* its check: jsr hc_segsz */
			put32(p + 16, HC_STUB(HC_SEGSZ));
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

/*
 * The loader's code is searched in RAM (sm = Lisa RAM, plo..phi physical),
 * or, with pfile, in a file's bytes as it is read: then a patch goes to the
 * file's patch table.
 */
static unsigned char *sm = (unsigned char *)PHYS;
static unsigned long plo = SYSAREA, phi = RAMSZ;
static int pfile, pfid, ppage0;
static long nscan;			/* RAM searches */

static int
pput(fd, a, d, n)
	int fd, n;
	unsigned long a;
	unsigned char *d;
{
	struct lisapatch lp;

	/* later searches see the patched code, as in RAM */
	if (pfile) {
		addpat(pfid, ppage0, sm, (int)a, d, n);
		memcpy(sm + a, d, n);
		return 1;
	}
	lp.lp_addr = a;
	lp.lp_len = n;
	memcpy(lp.lp_data, d, n);
	return ioctl(fd, LISAIOC_PATCH, &lp) == 0;
}

/* the loader's unpacker, before it runs */
static int
patchunpack(fd)
	int fd;
{
	unsigned char d[14];
	unsigned long a;
	int n = 0;

	for (a = plo; a + sizeof unpsig < phi; a += 2)
		if (sm[a] == unpsig[0] && !memcmp(sm + a, unpsig, sizeof unpsig)) {
			memcpy(d, "\x4e\xb9....\x20\x5f\xde\xfc\x00\x14\x4e\xd0", 14);
			put32(d + 2, (unsigned long)hc_unpack);
			n += pput(fd, a + 6, d, 14);
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

/*
 * A segment moved into the free region next to it, rounded to 4 KB: D4 its
 * origin, D7 its size, D6 its new origin, D5 the free region left.  The
 * pages after a segment up to the next region are free though no region
 * holds them, so the region left spans the whole hole; the segment keeps
 * its size if that leaves nothing.
 */
static void
hcmove(r)
	unsigned long *r;
{
	unsigned long s = G32(r[12] - 6), f = G32(r[12] - 14);
	long so = G16(s + 8), sz = G16(s + 10), fs = G16(f + 10), fo = G16(f + 8), rs = (sz + 7) & ~7, no, left;
	int top = G8(r[14] + 12) != 0;

	if (top) {
		no = fo + fs - rs;
		left = no - so;
	} else {
		no = fo;
		left = G16(G32(s) + 8) - fo - rs;
	}
	if (left <= 0 || rs > 0x7ff) {
		rs = sz;
		left = fs;
		no = top ? fo + fs - sz : fo;
	}
	P16(s + 10, rs);
	r[4] = (r[4] & ~0xffffUL) | so;
	r[5] = (r[5] & ~0xffffUL) | (left & 0xffff);
	r[6] = (r[6] & ~0xffffUL) | (no & 0xffff);
	r[7] = (r[7] & ~0xffffUL) | rs;
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
			gmem(dst + i, c + i, (int)(k = n - i > 512 ? 512 : n - i), LM_WRITE, 0L);
		if (verbose)
			fprintf(stderr, "lisa: unpacked code at %#lx patched\n", dst);
	}
	/* the unpacker's output reaches the instruction cache, patched or not */
	gmem(dst, c, 0, LM_WRITE | LM_FLUSH, 0L);
}

/* 1 the first time file fid is seen, when the table has room for it */
static int
firstseen(fid)
	int fid;
{
	int i;

	for (i = 0; i < nseen && seen[i] != fid; i++)
		;
	if (i < nseen)
		return 0;
	if (nseen == sizeof seen / sizeof seen[0]) {
		if (fid != full)
			fprintf(stderr, "lisa: file table full: file %d not patched\n", fid);
		full = fid;
		return 0;
	}
	seen[nseen++] = fid;
	return 1;
}

/* the patches for file fid, its pages from page0 in f[0..n) */
static void
filepats(fid, page0, f, n)
	int fid, page0;
	unsigned char *f;
	long n;
{
	pfdrv(fid, page0, f, n);
	lldirq(fid, page0, f, n);
	lldmisc(fid, page0, f, n);
	lldcops(fid, page0, f, n);
	lldinit(fid, page0, f, n);
	sonydrv(fid, page0, f, n);
}

static void applypat();

/* the patches for the file of a block's tag, worked out on its first block from the whole file */
static void
patchblock(p)
	unsigned char *p;
{
	int fid = (short)(p[4] << 8 | p[5]), lo = 0x7fff, hi = -1, k;
	unsigned char *f, *t;
	long b;

	if (fid <= 0)
		return;
	if (firstseen(fid)) {
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
			filepats(fid, lo, f, (hi - lo + 1) * 512L);
		}
	}
	applypat(p);
}

/* the patches for a block of a file, by its tag */
static void
applypat(p)
	unsigned char *p;
{
	int fid = (short)(p[4] << 8 | p[5]), page = p[12] << 8 | p[13], i;

	/* tag byte 11 keeps a checksummed block's bytes XORing to zero */
	for (i = 0; i < npat; i++)
		if (pat[i].fid == fid && pat[i].page == page && p[20 + pat[i].off] == pat[i].old) {
			p[20 + pat[i].off] = pat[i].val;
			if (p[6] & 0x80)
				p[11] ^= pat[i].old ^ pat[i].val;
		}
}

/* ---- the environment's folder ---- */

static int word(), patchlfs(), copsdone;
static void rampatches(), ldrscan();
static char *root;			/* the environment's folder */
static unsigned char *boot;		/* .boot: block number, then tag and data, each */
static long nboot;
static int lbs, lcur, lmax, loff, lbuf;	/* off A5, the loader's block size (512), block, last block, offset, buffer */
static unsigned char *lf;		/* the file the loader reads, patched */
static long lfn;
static char *lfname;

/* .boot's copy of block b as 20 tag bytes then 512 data; -1 none */
static int
bootread(b, p)
	long b;
	unsigned char *p;
{
	long i;

	for (i = 0; i < nboot; i++)
		if (((long)boot[i * 536] << 24 | boot[i * 536 + 1] << 16 | boot[i * 536 + 2] << 8 | boot[i * 536 + 3]) == b) {
			memcpy(p, boot + i * 536 + 4, BLK);
			return 0;
		}
	return -1;
}

static int
bootopen()
{
	struct stat sb;
	int fd;

	if ((fd = open(envpath(root, ".boot"), O_RDONLY)) < 0)
		return -1;
	if (fstat(fd, &sb) < 0 || sb.st_size % 536 || (boot = malloc(sb.st_size + 1)) == 0 ||
	    read(fd, boot, sb.st_size) != sb.st_size) {
		close(fd);
		return -1;
	}
	close(fd);
	nboot = sb.st_size / 536;
	return 0;
}

/* the boot code's and loader's patches, from their blocks in .boot (tag files $AAAA, $BBBB) */
static void
bootpats()
{
	unsigned char *t, *f;
	int lo, hi, k, fid;
	long i;

	for (fid = (short)0xaaaa; fid != (short)0xcccc; fid += 0x1111) {
		lo = 0x7fff;
		hi = -1;
		for (i = 0; i < nboot; i++) {
			t = boot + i * 536 + 4;
			if ((short)(t[4] << 8 | t[5]) == fid) {
				k = t[12] << 8 | t[13];
				lo = k < lo ? k : lo;
				hi = k > hi ? k : hi;
			}
		}
		if (hi < lo || (f = calloc(hi - lo + 1, 512)) == 0)
			continue;
		for (i = 0; i < nboot; i++) {
			t = boot + i * 536 + 4;
			if ((short)(t[4] << 8 | t[5]) == fid)
				memcpy(f + ((t[12] << 8 | t[13]) - lo) * 512L, t + 20, 512);
		}
		if (fid == (short)0xbbbb && patchlfs(fid, lo, f, (hi - lo + 1) * 512L) && verbose)
			fprintf(stderr, "lisa: loader reads its files by host call\n");
		ldrscan(fid, lo, f, (hi - lo + 1) * 512L, 0);
		free(f);
	}
}

/* host name h as a Lisa name, in out (64 bytes): %XX is byte XX */
static int
lisaname(h, out)
	char *h, *out;
{
	int n = 0, c;

	for (; *h && n < 63; n++)
		if (*h == '%' && sscanf(h + 1, "%2x", &c) == 1 && h[1] && h[2]) {
			out[n] = c;
			h += 3;
		} else
			out[n] = *h++;
	out[n] = 0;
	return !*h;
}

/*
 * Lisa file nm (case ignored) from the folder into lf, with the patches of
 * the file number in its sidecar; 0 none.  Dot names are not Lisa files and
 * only regular files are.
 */
static int
lisaopen(nm)
	char *nm;
{
	char ln[64], *p;
	struct dirent *de;
	struct stat sb;
	DIR *d;
	FILE *m;
	int fd, fid = 0, i, k;

	if ((d = opendir(root)) == 0)
		return 0;
	while ((de = readdir(d)) != 0) {
		if (de->d_name[0] == '.' || !lisaname(de->d_name, ln))
			continue;
		for (i = 0; ln[i] && nm[i] && tolower((unsigned char)ln[i]) == tolower((unsigned char)nm[i]); i++)
			;
		if (!ln[i] && !nm[i])
			break;
	}
	if (!de) {
		closedir(d);
		return 0;
	}
	free(lfname);
	lfname = envpath(root, de->d_name);
	p = malloc(strlen(lfname) + 2);
	sprintf(p, "%s/.%s", root, de->d_name);
	closedir(d);
	free(lf);
	lf = 0;
	if (lstat(lfname, &sb) < 0 || !S_ISREG(sb.st_mode) || (fd = open(lfname, O_RDONLY)) < 0) {
		free(p);
		return 0;
	}
	lfn = sb.st_size;
	if ((lf = malloc(lfn + 1)) == 0 || read(fd, lf, lfn) != lfn) {
		close(fd);
		free(p);
		return 0;
	}
	close(fd);
	if ((m = fopen(p, "r")) != 0) {
		fscanf(m, "file %d", &fid);
		fclose(m);
	}
	free(p);
	if (fid > 0 && firstseen(fid))
		filepats(fid, 0, lf, lfn);
	if (fid > 0 && !copsdone)
		ldrscan(fid, 0, lf, lfn, 1);
	for (i = 0; i < npat; i++)
		if (fid > 0 && pat[i].fid == fid && (k = pat[i].page * 512 + pat[i].off) < lfn && lf[k] == pat[i].old)
			lf[k] = pat[i].val;
	if (verbose)
		fprintf(stderr, "lisa: loader file %s from host\n", strrchr(lfname, '/') + 1);
	return 1;
}

/* the loader's OPENINPU(name): the file, its last block, and an empty buffer */
static void
hclopen(r)
	unsigned long *r;
{
	unsigned long a5 = r[13], nm = G32(&r[18]);
	char n[64];
	int k = G8(nm), i, ok;

	rampatches();

	for (i = 0; i < k && i < 63; i++)
		n[i] = G8(nm + 1 + i);
	n[i] = 0;
	if ((ok = lisaopen(n)) != 0) {
		P32(a5 + lcur, -1);
		P16(a5 + lmax, lfn ? (lfn - 1) / 512 : 0);
		P16(a5 + loff, 512);
	}
	P8(&r[19], ok);
}

/* block k of the file into the loader's buffer */
static void
lblock(a5, k)
	unsigned long a5;
	long k;
{
	unsigned char b[512];
	long o = k * 512;

	memset(b, 0, sizeof b);
	if (o >= 0 && o < lfn)
		memcpy(b, lf + o, lfn - o < 512 ? lfn - o : 512);
	gmem(a5 + lbuf, b, 512, LM_WRITE | LM_FLUSH, 0L);
	P32(a5 + lcur, k);
}

/* the loader's FILLBUF(pos): the buffer holds the block of pos */
static void
hclfill(r)
	unsigned long *r;
{
	unsigned long a5 = r[13];
	long pos = G32(&r[18]), k = pos / 512;

	rampatches();

	if (pos < 0 || k > (short)G16(a5 + lmax)) {
		fprintf(stderr, "lisa: loader read past the end of %s\n", lfname);
		exit(1);
	}
	P16(a5 + loff, pos - k * 512);
	if (k != (long)G32(a5 + lcur))
		lblock(a5, k);
}

/* the loader's MOVEMULT(dst, n): n bytes on from the position to dst */
static void
hclmove(r)
	unsigned long *r;
{
	unsigned long a5 = r[13], dst = G32(&r[18]);
	long n = G32(&r[19]), pos = (long)G32(a5 + lcur) * 512 + (short)G16(a5 + loff), i, k = 0;

	rampatches();

	if (n <= 0)
		return;
	if (pos < 0 || pos + n > lfn) {
		fprintf(stderr, "lisa: loader read past the end of %s\n", lfname);
		exit(1);
	}
	for (i = 0; i < n; i += k)
		gmem(dst + i, lf + pos + i, (int)(k = n - i > 512 ? 512 : n - i), LM_WRITE | (k == n - i ? LM_FLUSH : 0), 0L);
	pos += n;
	k = pos / 512;
	if (pos % 512 == 0)
		k--;
	lblock(a5, k);
	P16(a5 + loff, pos - k * 512);
	rampatches();
}

/* the loader's procedure named nm in f[0..n): its entry (a LINK after the previous name); -1 none or several */
static long
procfind(f, n, nm)
	unsigned char *f;
	long n;
	char *nm;
{
	unsigned char *q;
	long a, e, at = -1;
	int i;

	for (a = 12; a + 10 < n; a += 2) {
		q = f + a;
		if (q[2] != (nm[0] | 0x80) || memcmp(q + 3, nm + 1, 7) || !(word(q, 0x4e75) || word(q, 0x4ed0)))
			continue;
		for (e = a; e >= 12; e -= 2) {
			q = f + e;
			if (word(q, 0x4e56) && word(q - 2, 0) && (q[-10] & 0x80)) {
				for (i = -9; i < -2 && (isupper(q[i]) || isdigit(q[i]) || q[i] == '_' || q[i] == ' '); i++)
					;
				if (i == -2)
					break;
			}
		}
		if (e < 12 || at >= 0)
			return -1;
		at = e;
	}
	return at;
}

/* the offset from e of the first word w in the procedure there (up to its name); -1 none */
static long
procop(f, n, e, w)
	unsigned char *f;
	long n, e;
	int w;
{
	long a;

	for (a = e; a < e + 0x400 && a + 4 < n && !(f[a + 2] & 0x80 && (word(f + a, 0x4e75) || word(f + a, 0x4ed0))); a += 2)
		if (word(f + a, w))
			return a - e;
	return -1;
}

/*
 * The loader's file system reads its files by name from the folder.  Its
 * files are read through OPENINPU (open by name), FILLBUF (the block of a
 * position into a buffer) and MOVEMULT (bytes from the position to memory),
 * all keeping the position in globals off A5; those three jump to host calls.
 * INITMEDI reads the volume's MDDF for the block size and the file system's
 * layout; its read becomes a host call setting the block size, then it returns:
 *
 *	clr.l -(sp); jsr READ_PAG	->	jsr hc_lmedia; bra.w exit
 *
 * The globals come from the instructions of FILLBUF and READ_PAG:
 *	move.l cur(a5),-4(a6)  move.w max(a5),d1  move.w d0,off(a5)  move.w bs(a5),d0
 *	lea buf(a5),a0
 * The patches go in the loader's blocks (file fid, pages from page0 in f[0..n)) as they are read.
 */
static int
patchlfs(fid, page0, f, n)
	int fid, page0;
	unsigned char *f;
	long n;
{
	long fill = procfind(f, n, "FILLBUF "), rp = procfind(f, n, "READ_PAG"), im = procfind(f, n, "INITMEDI");
	long op = procfind(f, n, "OPENINPU"), mv = procfind(f, n, "MOVEMULT"), a, o[5];
	unsigned char j[10];

	if (fill < 0 || rp < 0 || im < 0 || op < 0 || mv < 0 || !word(f + fill + 4, 0x2d6d) ||
	    (o[1] = procop(f, n, fill, 0x322d)) < 0 || (o[2] = procop(f, n, fill, 0x3b40)) < 0 ||
	    (o[3] = procop(f, n, fill, 0x302d)) < 0 || (o[4] = procop(f, n, rp, 0x41ed)) < 0)
		return 0;
	for (a = im; a < im + 0x200 && a + 6 < n; a += 2)
		if (word(f + a, 0x42a7) && word(f + a + 2, 0x4eba) && a + 4 + (short)G16P(f + a + 4) == rp)
			break;
	if (a >= im + 0x200 || a + 6 >= n || (o[0] = procop(f, n, a, 0x4cdf)) < 0 || !word(f + a + o[0] + 4, 0x4e5e))
		return 0;
	lcur = (short)G16P(f + fill + 6);
	lmax = (short)G16P(f + fill + o[1] + 2);
	loff = (short)G16P(f + fill + o[2] + 2);
	lbs = (short)G16P(f + fill + o[3] + 2);
	lbuf = (short)G16P(f + rp + o[4] + 2);
	memcpy(j, "\x4e\xb9....\x60\x00..", 10);
	put32(j + 2, (unsigned long)hc_lmedia);
	j[8] = (o[0] - 8) >> 8;
	j[9] = o[0] - 8;
	addpat(fid, page0, f, (int)a, j, 10);
	j[1] = 0xf9;
	put32(j + 2, (unsigned long)hc_lopen);
	addpat(fid, page0, f, (int)op, j, 6);
	put32(j + 2, (unsigned long)hc_lfill);
	addpat(fid, page0, f, (int)fill, j, 6);
	put32(j + 2, (unsigned long)hc_lmove);
	addpat(fid, page0, f, (int)mv, j, 6);
	return 1;
}

/*
 * ---- the OS's s-files as host files ----
 *
 * The OS's file layer under every caller: the s-list (s-file -> header page,
 * first page, size), header bytes by page, and file data by page.  Each
 * s-file s is host file NAME with sidecar .NAME ("file s", "catalog HEX",
 * "header HEX").  A header page is HPAGE + s, past the volume's pages; data
 * pages map through the extents in the header to bytes of the host file.
 */
#define	NSF	1024
#define	HPAGE	0x8000L
#define	G32P(p)	((unsigned long)(p)[0] << 24 | (unsigned long)(p)[1] << 16 | (p)[2] << 8 | (p)[3])

static struct sfile {
	char	*name;			/* in the folder, 0 until its header names it */
	int	fd;
	long	hpage;
	unsigned char hdr[512], cat[64];
} *sf[NSF];
static int sfscanned;

static void
unhex(s, p, n)
	char *s;
	unsigned char *p;
	int n;
{
	unsigned int c;

	for (; n > 0 && sscanf(s, "%2x", &c) == 1; n--, s += 2)
		*p++ = c;
}

static void
sfscan()
{
	static char l[1200];
	struct dirent *de;
	struct sfile *f;
	DIR *d;
	FILE *m;
	char *p;
	int s;

	sfscanned = 1;
	if ((d = opendir(root)) == 0)
		return;
	while ((de = readdir(d)) != 0) {
		if (de->d_name[0] != '.' || !de->d_name[1] || de->d_name[1] == '.')
			continue;
		p = envpath(root, de->d_name);
		m = fopen(p, "r");
		free(p);
		if (!m)
			continue;
		if (fscanf(m, "file %d\n", &s) == 1 && s > 0 && s < NSF && !sf[s] && (f = calloc(1, sizeof *f)) != 0) {
			f->name = strdup(de->d_name + 1);
			f->fd = -1;
			f->hpage = HPAGE + s;
			while (fgets(l, sizeof l, m))
				if (!strncmp(l, "catalog ", 8))
					unhex(l + 8, f->cat, 64);
				else if (!strncmp(l, "header ", 7))
					unhex(l + 7, f->hdr, 512);
			sf[s] = f;
		}
		fclose(m);
	}
	closedir(d);
}

static struct sfile *
sfget(s)
	int s;
{
	if (!sfscanned)
		sfscan();
	return s > 0 && s < NSF ? sf[s] : 0;
}

static int
sfopen(f)
	struct sfile *f;
{
	char *p;

	if (f->fd < 0 && f->name) {
		p = envpath(root, f->name);
		if ((f->fd = open(p, O_RDWR)) < 0)
			f->fd = open(p, O_RDONLY);
		free(p);
	}
	return f->fd;
}

/* the sidecar of s-file s */
static void
sfsave(s, f)
	int s;
	struct sfile *f;
{
	char *p = malloc(strlen(root) + strlen(f->name) + 3);
	FILE *m;
	int i;

	sprintf(p, "%s/.%s", root, f->name);
	if ((m = fopen(p, "w")) != 0) {
		fprintf(m, "file %d\ncatalog ", s);
		for (i = 0; i < 64; i++)
			fprintf(m, "%02x", f->cat[i]);
		fprintf(m, "\nheader ");
		for (i = 0; i < 512; i++)
			fprintf(m, "%02x", f->hdr[i]);
		fprintf(m, "\n");
		fclose(m);
	}
	free(p);
}

/* the header's name as a host name: % and / escaped, no leading dot */
static void
sfhname(h, out)
	unsigned char *h;
	char *out;
{
	int i, n = h[0] > 32 ? 32 : h[0];

	for (i = 1; i <= n; i++)
		if (h[i] == '%' || h[i] == '/' || (i == 1 && h[i] == '.') || !h[i])
			out += sprintf(out, "%%%02X", h[i]);
		else
			*out++ = h[i];
	*out = 0;
}

/* a header written: a new file is made, a renamed one renamed */
static void
sfnamed(s, f)
	int s;
	struct sfile *f;
{
	char h[120], *a, *b, *c;
	int fd, t;

	sfhname(f->hdr, h);
	if (!h[0] || (f->name && !strcmp(h, f->name)))
		return;
	/* stepped aside: keeps that name until deleted */
	sprintf(h + strlen(h), "%%z%d", s);
	if (f->name && !strcmp(h, f->name))
		return;
	sfhname(f->hdr, h);
	/* a file still holding the name (a save's old copy, deleted next) steps aside under a name no header makes */
	for (t = 1; t < NSF; t++)
		if (t != s && sf[t] && sf[t]->name && !strcmp(sf[t]->name, h)) {
			a = malloc(strlen(h) + 12);
			sprintf(a, "%s%%z%d", h, t);
			b = malloc(strlen(root) + strlen(a) + 3);
			sprintf(b, "%s/%s", root, a);
			c = envpath(root, h);
			rename(c, b);
			free(c);
			sprintf(b, "%s/.%s", root, h);
			unlink(b);
			free(b);
			free(sf[t]->name);
			sf[t]->name = a;
			sfsave(t, sf[t]);
		}
	b = envpath(root, h);
	if (!f->name) {
		if ((fd = open(b, O_RDWR | O_CREAT | O_EXCL, 0644)) >= 0)
			f->fd = fd;
		else
			fprintf(stderr, "lisa: s-file %d: %s: %s\n", s, h, strerror(errno));
	} else {
		a = envpath(root, f->name);
		rename(a, b);
		free(a);
		a = malloc(strlen(root) + strlen(f->name) + 3);
		sprintf(a, "%s/.%s", root, f->name);
		unlink(a);
		free(a);
		free(f->name);
	}
	free(b);
	f->name = strdup(h);
	if (verbose)
		fprintf(stderr, "lisa: s-file %d is host file %s\n", s, h);
}

/* the file page of absolute page p, by the header's extents; -1 none */
static long
sfrel(f, p)
	struct sfile *f;
	long p;
{
	unsigned char *e = f->hdr + 0x88;
	long rel = 0, st, n;
	int k = G16P(f->hdr + 0x86);

	for (; k > 0 && e + 6 <= f->hdr + 0xf0; k--, e += 6) {
		st = G32P(e);
		n = G16P(e + 4);
		if (p >= st && p < st + n)
			return rel + p - st;
		rel += n;
	}
	return -1;
}

#define	ARG(r, k)	((unsigned char *)&(r)[16] + (k))	/* argument at frame offset k */

/* the error word, last, and the caches pushed for all the call wrote */
static void
errout(a, e)
	unsigned long a;
	int e;
{
	unsigned char b[2];

	b[0] = e >> 8;
	b[1] = e;
	gmem(a, b, 2, LM_WRITE | LM_FLUSH, 0L);
}

/* the patches for s-file s, worked out from the whole file when first read */
static void
sfpats(s, f)
	int s;
	struct sfile *f;
{
	struct stat sb;
	unsigned char *w;

	if (firstseen(s) && fstat(f->fd, &sb) == 0 && sb.st_size > 0 && (w = malloc(sb.st_size)) != 0) {
		if (pread(f->fd, w, sb.st_size, 0L) == sb.st_size)
			filepats(s, 0, w, (long)sb.st_size);
		free(w);
	}
}

/* s-list entry of s-file s: header page, first page, size in bytes */
static void
hcslist(r)
	unsigned long *r;
{
	unsigned long buf = G32P(ARG(r, 8)), err = G32P(ARG(r, 18));
	int s = (short)G16P(ARG(r, 14));
	struct sfile *f = sfget(s);
	unsigned char e[14];
	char *p;

	if (!*ARG(r, 12)) {
		memset(e, 0, sizeof e);
		if (f) {
			put32(e, f->hpage);
			put32(e + 4, G32P(f->hdr + 0x88));
			put32(e + 8, G16P(f->hdr + 0x82) * 512UL);
		}
		gmem(buf, e, sizeof e, LM_WRITE | LM_FLUSH, 0L);
	} else if (gmem(buf, e, sizeof e, 0, 0L) == 0 && s > 0 && s < NSF) {
		/* header page 0: free; -1: held (being made, or retired) */
		if ((long)G32P(e) <= 0 && f) {
			if (f->fd >= 0)
				close(f->fd);
			if (f->name) {
				p = envpath(root, f->name);
				unlink(p);
				free(p);
				p = malloc(strlen(root) + strlen(f->name) + 3);
				sprintf(p, "%s/.%s", root, f->name);
				unlink(p);
				free(p);
				if (verbose)
					fprintf(stderr, "lisa: s-file %d deleted: %s\n", s, f->name);
				free(f->name);
			}
			free(f);
			sf[s] = 0;
		} else if ((long)G32P(e) > 0 && f)
			f->hpage = G32P(e);
		else if (G32P(e) && !f && (f = calloc(1, sizeof *f)) != 0) {
			f->fd = -1;
			f->hpage = G32P(e);
			sf[s] = f;
		}
	}
	errout(err, 0);
}

/* absolute page of file page rel, -1 none */
static long
sfabs(f, rel)
	struct sfile *f;
	long rel;
{
	unsigned char *e = f->hdr + 0x88;
	long n;
	int k = G16P(f->hdr + 0x86);

	for (; rel >= 0 && k > 0 && e + 6 <= f->hdr + 0xf0; k--, e += 6) {
		if (rel < (n = G16P(e + 4)))
			return G32P(e) + rel;
		rel -= n;
	}
	return -1;
}

/* a page's 24 tag bytes as the OS keeps them: file, absolute page, file page, links */
static void
sftag(t, fid, page, rel, next, prev)
	unsigned char *t;
	int fid;
	long page, rel, next, prev;
{
	memset(t, 0, 24);
	t[2] = 1;
	t[4] = fid >> 8;
	t[5] = fid;
	put32(t + 8, page & 0xffffff);
	put32(t + 12, rel);
	put32(t + 16, next);
	put32(t + 20, prev);
}

/* the 536-byte pages written that are no file's: the session's only */
static struct { long page; unsigned char *b; } spg[64];

/*
 * Bytes of a page: 24 tag bytes, then 512 data.  Header pages are the
 * sidecar's header, data pages the host file's bytes.
 */
static void
hcpage(r)
	unsigned long *r;
{
	static int warned;
	unsigned long buf = G32P(ARG(r, 12)), err = G32P(ARG(r, 24));
	long page = G32P(ARG(r, 18)), rel = -1;
	int n = G16P(ARG(r, 10)), off = G16P(ARG(r, 16)), wr = *ARG(r, 8), s, i, k;
	unsigned char pg[536], *keep = 0;
	struct sfile *f = 0;

	if (off < 0 || n < 0 || off + n > sizeof pg) {
		errout(err, 1885);
		return;
	}
	sfget(0);
	for (s = 1; s < NSF && !(sf[s] && sf[s]->hpage == page); s++)
		;
	if (s < NSF) {
		f = sf[s];
		sftag(pg, -s, page, 0L, -1L, -1L);
		memcpy(pg + 24, f->hdr, 512);
	} else {
		for (s = 1; s < NSF && !(sf[s] && (rel = sfrel(sf[s], page)) >= 0); s++)
			;
		if (s < NSF && sfopen(f = sf[s]) >= 0) {
			sftag(pg, s, page, rel, sfabs(f, rel + 1), rel ? sfabs(f, rel - 1) : -1L);
			memset(pg + 24, 0, 512);
			sfpats(s, f);
			pread(f->fd, pg + 24, 512, rel * 512);
			for (i = 0; i < npat; i++)
				if (pat[i].fid == s && pat[i].page == rel && pg[24 + pat[i].off] == pat[i].old)
					pg[24 + pat[i].off] = pat[i].val;
		} else {
			f = 0;
			for (i = 0; i < 64 && spg[i].b && spg[i].page != page; i++)
				;
			if (i < 64 && !spg[i].b && wr && (spg[i].b = calloc(1, 536)) != 0)
				spg[i].page = page;
			if (i < 64 && spg[i].b)
				keep = spg[i].b;
			else if (warned++ < 10)
				fprintf(stderr, "lisa: page %#lx: no file's (op %d, %d at %d, from %#lx)\n", page, wr, n, off, r[17]);
			if (keep)
				memcpy(pg, keep, 536);
			else
				memset(pg, 0, 536);
		}
	}
	for (i = 0; i < n; i += k) {
		k = n - i > 512 ? 512 : n - i;
		gmem(buf + i, pg + off + i, k, wr ? 0 : LM_WRITE | LM_FLUSH, 0L);
	}
	if (wr && keep)
		memcpy(keep, pg, 536);
	else if (wr && f && rel < 0) {
		memcpy(f->hdr, pg + 24, 512);
		sfnamed(s, f);
		if (f->name)
			sfsave(s, f);
		/* pages written before the header gave them to this file */
		for (i = 0; i < 64 && spg[i].b; )
			if ((rel = sfrel(f, spg[i].page)) >= 0 && sfopen(f) >= 0) {
				pwrite(f->fd, spg[i].b + 24, 512, rel * 512);
				if (verbose)
					fprintf(stderr, "lisa: s-file %d: page %#lx kept to its header\n", s, spg[i].page);
				free(spg[i].b);
				for (k = i; k < 63 && spg[k + 1].b; k++)
					spg[k] = spg[k + 1];
				spg[k].b = 0;
			} else
				i++;
	} else if (wr && f && off + n > 24) {
		k = off > 24 ? off : 24;
		pwrite(f->fd, pg + k, off + n - k, rel * 512 + k - 24);
	}
	errout(err, 0);
}

/*
 * File data, in the OS's routine after it has checked the request and
 * extended the file: count bytes at the page and offset, both moved on.
 * The frame: +8 write, +10 offset, +14 page (-1: at the end, no page yet),
 * +18 bytes done, +22 count, +26 buffer, +30 the file's control block
 * (+4 s-file, +22 first data page, +26 logical end), +34 error.
 */
static void
hcdata(r)
	unsigned long *r;
{
	static int said;
	unsigned long fp = r[14], poff = G32(fp + 10), ppage = G32(fp + 14), buf = G32(fp + 26), fcb = G32(fp + 30);
	unsigned long count = G32(fp + 22), done = 0;
	int s = (short)G16(fcb + 4), wr = G8(fp + 8), off = G16(poff), k, i, e = 0;
	long page = G32(ppage), leof = G32(fcb + 26), rel;
	struct sfile *f = sfget(s);
	unsigned char b[512];

	if (!f || sfopen(f) < 0)
		e = 1885;
	else if (!wr)
		sfpats(s, f);
	/* the control block's copy of the extents is ahead of the header the OS last wrote */
	if (!e && gmem(fcb + 156, b, 0xf0 - 0x82, 0, 0L) == 0 && (k = G16P(b + 4)) > 0 && k <= (0xf0 - 0x88) / 6 &&
	    k >= G16P(f->hdr + 0x86) && (!G16P(f->hdr + 0x86) || !memcmp(b + 6, f->hdr + 0x88, 4)) &&
	    G32P(b + 6) > 0 && G32P(b + 6) < G32P(vmddf + 0x8c))
		memcpy(f->hdr + 0x82, b, 6 + 6 * k);
	if (e)
		rel = -1;
	else if (!G32(fcb + 22) || !leof)
		rel = 0;
	else if (page == -1)
		rel = leof / 512;
	else if ((rel = sfrel(f, page)) < 0) {
		fprintf(stderr, "lisa: s-file %d: page %#lx in no extent\n", s, page);
		e = 1885;
	}
	/* a write to an empty file makes its first page known, as the skipped code does */
	if (!e && wr && (!G32(fcb + 22) || !leof)) {
		long p0 = sfabs(f, 0L);

		if (p0 >= 0) {
			P32(fcb + 22, p0);
			P32(fcb + 8, p0);
			P16(fcb + 12, 0);
		}
	}
	while (!e && done < count) {
		k = count - done < 512 - off ? count - done : 512 - off;
		if (wr) {
			if (gmem(buf + done, b, k, 0, 0L) < 0 || pwrite(f->fd, b, k, rel * 512 + off) != k)
				e = 1885;
		} else {
			memset(b, 0, sizeof b);
			pread(f->fd, b, k, rel * 512 + off);
			for (i = 0; i < npat; i++)
				if (pat[i].fid == s && pat[i].page == rel && pat[i].off >= off && pat[i].off < off + k &&
				    b[pat[i].off - off] == pat[i].old)
					b[pat[i].off - off] = pat[i].val;
			if (gmem(buf + done, b, k, LM_WRITE | LM_FLUSH, 0L) < 0)
				e = 1885;
		}
		if (e)
			break;
		done += k;
		if ((off += k) == 512) {
			off = 0;
			rel++;
		}
	}
	if (verbose && !said++)
		fprintf(stderr, "lisa: OS files from host: s-file %d, %ld RAM searches\n", s, nscan);
	/* a page boundary at or past the logical end: no page yet, as the OS's own code leaves it */
	if (rel >= 0) {
		P16(poff, off);
		P32(ppage, !off && rel * 512 >= leof ? -1L : sfabs(f, rel));
	}
	P32(G32(fp + 18), done);
	errout(G32(fp + 34), e);
}

/* ---- the catalog: one entry per s-file, from its sidecar ---- */

/*
 * The catalog module's entry points answer from the sidecars.  Callers
 * get entries in the OS's own cache buffers (+8 dirty, +9 held, +10 device,
 * +12 page, +24 2048 data): the entry at the data's start, its place in
 * the slot table at the end, as the module's own code reads a node.
 */
#define	CTAG	0x7f000000L		/* page of a buffer holding entry id */
#define	NXC	32
#define	CEND	848			/* no more entries */
static unsigned char xcat[NXC][64];	/* entries of no s-file: the session's only */

static void
gwrite(a, p, n)
	unsigned long a;
	unsigned char *p;
	int n;
{
	int i, k;

	for (i = 0; i < n; i += k) {
		k = n - i > 512 ? 512 : n - i;
		gmem(a + i, p + i, k, LM_WRITE | LM_FLUSH, 0L);
	}
}

static unsigned char *
cslot(id)
	int id;
{
	unsigned char *e = id < NSF ? (sf[id] ? sf[id]->cat : 0) : id < NSF + NXC ? xcat[id - NSF] : 0;

	return e && e[0] == 36 ? e : 0;
}

/* key order: length and parent exact, then the name without case */
static int
ckey(a, b)
	unsigned char *a, *b;
{
	int i, x, y;

	for (i = 0; i < 36; i++) {
		x = i > 2 && a[i] >= 'a' && a[i] <= 'z' ? a[i] - 32 : a[i];
		y = i > 2 && b[i] >= 'a' && b[i] <= 'z' ? b[i] - 32 : b[i];
		if (x != y)
			return x - y;
	}
	return 0;
}

static int cord[NSF + NXC], ncord = -1;	/* the entries in key order */

static int
cbykey(a, b)
	const void *a, *b;
{
	return ckey(cslot(*(int *)a), cslot(*(int *)b));
}

/* the entry of key k, or (after) the first one past it; -1 none */
static int
cfind(k, after)
	unsigned char *k;
	int after;
{
	int id, n = 0, lo, hi, m;

	sfget(0);
	for (id = 1; id < NSF + NXC; id++)
		n += cslot(id) != 0;
	for (id = 0; id < ncord && cslot(cord[id]); id++)
		;
	if (ncord != n || id < n) {
		for (id = 1, ncord = 0; id < NSF + NXC; id++)
			if (cslot(id))
				cord[ncord++] = id;
		qsort(cord, ncord, sizeof cord[0], cbykey);
	}
	for (lo = 0, hi = ncord; lo < hi; )
		if (ckey(cslot(cord[m = (lo + hi) / 2]), k) < 0 || (after && !ckey(cslot(cord[m]), k)))
			lo = m + 1;
		else
			hi = m;
	return lo < ncord && (after || !ckey(cslot(cord[lo]), k)) ? cord[lo] : -1;
}

/* the leaf level of the module's search path */
#define	CSTATE(a5, vol)	((a5) - 2452 + 16 * G16(G32((vol) + 54) + 306))

/* a free cache buffer holding entry id, or k alone (not found); 0 none free */
static unsigned long
cbuf(a5, vol, id, k)
	unsigned long a5, vol;
	int id;
	unsigned char *k;
{
	unsigned long d, h = G32(a5 - 1742), st = CSTATE(a5, vol);
	unsigned char b[2048], s[8];
	int v = G16(vol + 38);

	for (d = h; G8(d + 9); )
		if ((d = G32(d + 4)) == h) {
			fprintf(stderr, "lisa: catalog: no free buffer\n");
			return 0;
		}
	memset(b, 0, sizeof b);
	memcpy(b, id >= 0 ? cslot(id) : k, id >= 0 ? 64 : 36);
	b[2033] = 64;
	b[2037] = id >= 0;
	memset(b + 2038, 0xff, 8);
	gwrite(d + 24, b, sizeof b);
	s[0] = 0;
	s[1] = 1;
	s[2] = v >> 8;
	s[3] = v;
	put32(s + 4, CTAG | (id & 0xffff));
	gwrite(d + 8, s, 8);
	put32(s, CTAG | (id & 0xffff));
	s[4] = s[5] = s[6] = 0;
	s[7] = id >= 0;
	gwrite(st, s, 8);
	return d;
}

/* buffer d given back: an entry changed in it goes to its sidecar */
static void
cput(d)
	unsigned long d;
{
	unsigned long t = G32(d + 12);
	unsigned char *e;
	struct sfile *f;
	int id = t & 0xffff;

	if (G8(d + 8) && (t & 0xffff0000UL) == CTAG && (e = cslot(id)) != 0) {
		gmem(d + 24, e, 64, 0, 0L);
		ncord = -1;
		if (id < NSF && (f = sf[id])->name) {
			sfsave(id, f);
			if (sfopen(f) >= 0)
				ftruncate(f->fd, (off_t)G32P(e + 48));
		}
	}
	P32(d + 12, -1);
	P16(d + 8, 0);
	gmem(d + 8, (unsigned char *)"", 0, LM_WRITE | LM_FLUSH, 0L);
}

/* a call for the boot volume (the folder), the first one mounted; others run the OS's own code */
static int
cours(r, hc)
	unsigned long *r;
	int hc;
{
	static int voff[] = { 0, 22, 24, 28, 24, 34, 22, 24, 18 };
	static long bootdev = -1;
	int dev;

	if (hc == HC_CREL)
		return (G32(G32P(ARG(r, 8)) + 12) & 0xffff0000UL) == CTAG;
	if (hc == HC_CENUM)
		dev = G16(G32(r[13] + cpra5) + 176);
	else if (hc >= HC_MDDF)
		dev = G16P(ARG(r, 14));
	else
		dev = G16(G32P(ARG(r, voff[hc - HC_CREL])) + 38);
	if (bootdev < 0) {
		bootdev = dev;
		fprintf(stderr, "lisa: boot volume on device %d (%ld)\n", dev, G16(r[13] - 1168));
	}
	return dev == bootdev;
}

/* runs the OS's own routine: its first instructions, then a jump to the rest */
static void
cother(r, j)
	unsigned long *r;
	int j;
{
	unsigned long back = r[16] - 6 + ctlen[j];
	unsigned short *t = &ctramp[j][ctlen[j] / 2 + 1];

	if (t[0] != back >> 16 || t[1] != (back & 0xffff)) {
		t[0] = back >> 16;
		t[1] = back;
		gmem(0L, (unsigned char *)"", 0, LM_WRITE | LM_FLUSH, 0L);
	}
	r[16] = (unsigned long)ctramp[j];
}

static void
hccrel(r)
	unsigned long *r;
{
	cput(G32P(ARG(r, 8)));
}

/* a page of the catalog: an empty buffer */
static void
hccget(r)
	unsigned long *r;
{
	unsigned long d = cbuf(r[13], G32P(ARG(r, 22)), -1, (unsigned char *)"");

	P32(G32P(ARG(r, 12)), d);
	errout(G32P(ARG(r, 8)), d ? 0 : 1885);
}

/* search: found flag and buffer; ep, when given, gets the entry */
static void
csearch(r, err, found, ep, desc, key, vol)
	unsigned long *r, err, found, ep, desc, key, vol;
{
	static int said;
	unsigned char k[36];
	unsigned long d;
	int id;

	if (verbose && !said++)
		fprintf(stderr, "lisa: catalog from host\n");
	gmem(key, k, 36, 0, 0L);
	id = cfind(k, 0);
	d = cbuf(r[13], vol, id, k);
	P32(desc, d);
	P8(found, id >= 0);
	if (ep)
		P32(ep, d + 24);
	errout(err, d ? 0 : 1885);
}

static void
hccsrch(r)
	unsigned long *r;
{
	csearch(r, G32P(ARG(r, 8)), G32P(ARG(r, 12)), 0L, G32P(ARG(r, 16)), G32P(ARG(r, 20)), G32P(ARG(r, 24)));
}

static void
hccptr(r)
	unsigned long *r;
{
	csearch(r, G32P(ARG(r, 8)), G32P(ARG(r, 12)), G32P(ARG(r, 16)), G32P(ARG(r, 20)), G32P(ARG(r, 24)),
	    G32P(ARG(r, 28)));
}

/* the entry after the buffer's into out; the buffer moves on */
static void
hccnext(r)
	unsigned long *r;
{
	unsigned long err = G32P(ARG(r, 8)), pdesc = G32P(ARG(r, 20)), d = G32(pdesc);
	unsigned char k[36];
	int id;

	gmem(d + 24, k, 36, 0, 0L);
	cput(d);
	if ((id = cfind(k, 1)) < 0) {
		errout(err, CEND);
		return;
	}
	d = cbuf(r[13], G32P(ARG(r, 24)), id, k);
	P32(pdesc, d);
	P16(G32P(ARG(r, 16)), 0);
	gwrite(G32P(ARG(r, 12)), cslot(id), 64);
	errout(err, d ? 0 : 1885);
}

/* the key of name n (Pascal) in directory dir */
static void
cmkey(k, dir, n)
	unsigned char *k, *n;
	int dir;
{
	memset(k, 0, 36);
	k[0] = 36;
	k[1] = dir >> 8;
	k[2] = dir;
	memcpy(k + 3, n + 1, n[0] > 32 ? 32 : n[0]);
}

/* a pathname: one name, after any leading '-'; the folder has no directories */
static int
cpath(path, n)
	unsigned long path;
	unsigned char *n;
{
	unsigned char p[256];
	int i;

	gmem(path, p, 256, 0, 0L);
	for (i = 1; i <= p[0] && p[i] == '-'; i++)
		;
	n[0] = p[0] - i + 1;
	memcpy(n + 1, p + i, n[0]);
	for (i = 1; i <= n[0]; i++)
		if (n[i] == '-')
			return -1;
	return n[0] ? 0 : -1;
}

static void
hcclook(r)
	unsigned long *r;
{
	unsigned long err = G32P(ARG(r, 8)), a5 = r[13];
	unsigned char n[256], k[36];
	int dir = (short)G16P(ARG(r, 32));

	P16(a5 - 2486, 0);
	P16(a5 - 2484, dir);
	P8(G32P(ARG(r, 12)), 0);
	if (cpath(G32P(ARG(r, 28)), n) < 0) {
		errout(err, 1282);
		return;
	}
	cmkey(k, dir, n);
	gwrite(G32P(ARG(r, 24)), k, 36);
	csearch(r, err, G32P(ARG(r, 12)), G32P(ARG(r, 16)), G32P(ARG(r, 20)), G32P(ARG(r, 24)), G32P(ARG(r, 34)));
}

/* a new entry: an s-file's goes to its sidecar */
static void
hccins(r)
	unsigned long *r;
{
	unsigned long err = G32P(ARG(r, 8)), d = G32P(ARG(r, 12));
	int n = G16P(ARG(r, 16)), s, i;
	unsigned char e[64], *p = 0;
	struct sfile *f = 0;

	if (d)
		cput(d);
	memset(e, 0, sizeof e);
	gmem(G32P(ARG(r, 18)), e, n > 64 ? 64 : n, 0, 0L);
	if (cfind(e, 0) >= 0) {
		errout(err, 1287);
		return;
	}
	s = G16P(e + 38);
	sfget(0);
	if (e[36] == 3 && s > 0 && s < NSF && (f = sf[s]) != 0)
		p = f->cat;
	for (i = 0; !p && i < NXC; i++)
		if (xcat[i][0] != 36)
			p = xcat[i];
	if (!p) {
		errout(err, 1885);
		return;
	}
	memcpy(p, e, 64);
	ncord = -1;
	if (f && p == f->cat && f->name)
		sfsave(s, f);
	errout(err, 0);
}

/* an entry removed: copied out first */
static void
hccdel(r)
	unsigned long *r;
{
	unsigned long err = G32P(ARG(r, 8)), d = G32P(ARG(r, 16)), out = G32P(ARG(r, 20));
	unsigned char k[36];
	int id;

	if (d)
		cput(d);
	gmem(out, k, 36, 0, 0L);
	if ((id = cfind(k, 0)) < 0) {
		errout(err, 1289);
		return;
	}
	gwrite(out, cslot(id), 64);
		fprintf(stderr, "lisa: catalog entry %d removed: %.32s\n", id, cslot(id) + 3);
	P16(G32P(ARG(r, 12)), 64);
	cslot(id)[0] = 0;
	if (id < NSF && sf[id]->name)
		sfsave(id, sf[id]);
	ncord = -1;
	errout(err, 0);
}

/* a listing of a directory: its entries in key order; the process keeps the place */
static void
hccopen(r)
	unsigned long *r;
{
	unsigned long err = G32P(ARG(r, 8)), vol = G32P(ARG(r, 18)), pr = G32(r[13] + cpra5);
	unsigned char n[256], k[36];
	int dir = (short)G16P(ARG(r, 16)), id;

	if (G8(G32P(ARG(r, 12))) || dir) {
		if (cpath(G32P(ARG(r, 12)), n) < 0 || (cmkey(k, dir, n), id = cfind(k, 0)) < 0) {
			errout(err, 1281);
			return;
		}
		if (cslot(id)[36] != 1) {
			errout(err, 1121);
			return;
		}
		dir = G16P(cslot(id) + 38);
	}
	P16(pr + 174, dir);
	P16(pr + 176, G16(vol + 38));
	P8(pr + 172, 0);
	P32(pr + 178, 0);
	P16(pr + 220, 0);
	P32(pr + 230, G32(vol + 110));
	errout(err, 0);
}

static void
hccenum(r)
	unsigned long *r;
{
	unsigned long err = G32P(ARG(r, 8)), name = G32P(ARG(r, 20)), pr = G32(r[13] + cpra5);
	long pos = G32(pr + 178), i;
	unsigned char k[36], n[34], *e;
	int id = -1;

	P8(name, 0);
	if (pos >= 0) {
		memset(k, 0, sizeof k);
		k[0] = 36;
		k[1] = G8(pr + 174);
		k[2] = G8(pr + 175);
		if ((id = cfind(k, 1)) >= 0) {
			for (i = 0; cord[i] != id; i++)
				;
			id = i + pos < ncord && !memcmp(cslot(cord[i + pos]), k, 3) ? cord[i + pos] : -1;
		}
	}
	if (id < 0) {
		P32(pr + 178, -1);
		errout(err, CEND);
		return;
	}
	e = cslot(id);
	gwrite(G32P(ARG(r, 16)), e, 64);
	for (n[0] = 0; n[0] < 32 && e[3 + n[0]]; n[0]++)
		n[1 + n[0]] = e[3 + n[0]];
	gwrite(name, n, n[0] + 1);
	P16(G32P(ARG(r, 12)), G16(pr + 220));
	P32(pr + 178, pos + 1);
	errout(err, 0);
}

/* ---- the volume's MDDF (.volume) and free bitmap (the files' extents) ---- */


/*
 * The free bitmap: a bit set for each page of a file's extents, one per file
 * for its header and the volume's own pages, the lowest free ones.  The
 * MDDF's free page count is the OS's own; vmap returns the bitmap's.
 */
static unsigned char *vbits;
static long vbn;


static long
vmap()
{
	long n = G32P(vmddf + 0x8c), st, p, used = 0, hout = 0;
	unsigned char *e;
	int s, k;

	if (n <= 0 || n > 0x100000 || (!vbits && (vbits = malloc((n + 7) / 8)) == 0))
		return -1;
	vbn = (n + 7) / 8;
	memset(vbits, 0, vbn);
	for (p = n; p < vbn * 8; p++)
		vbits[p >> 3] |= 1 << (p & 7);
	sfget(0);
	for (s = 1; s < NSF; s++)
		if (sf[s]) {
			if (sf[s]->hpage >= 0 && sf[s]->hpage < n && !(vbits[sf[s]->hpage >> 3] >> (sf[s]->hpage & 7) & 1)) {
				vbits[sf[s]->hpage >> 3] |= 1 << (sf[s]->hpage & 7);
				used++;
			} else
				hout++;
			for (k = G16P(sf[s]->hdr + 0x86), e = sf[s]->hdr + 0x88; k > 0 && e + 6 <= sf[s]->hdr + 0xf0; k--, e += 6)
				for (st = G32P(e), p = st; p < st + G16P(e + 4); p++)
					if (p >= 0 && p < n && !(vbits[p >> 3] >> (p & 7) & 1)) {
						vbits[p >> 3] |= 1 << (p & 7);
						used++;
					}
		}
	if (vover < 0)
		vover = n - used - hout - (long)G32P(vmddf + 0xba) > 0 ? n - used - hout - (long)G32P(vmddf + 0xba) : 0;
	/* headers kept outside the volume stand in at its start */
	for (st = vover + hout, p = 0; p < n && st > 0; p++)
		if (!(vbits[p >> 3] >> (p & 7) & 1)) {
			vbits[p >> 3] |= 1 << (p & 7);
			st--;
			used++;
		}
	return n - used;
}

static void
volread()
{
	static char l[1100];
	char *p = envpath(root, ".volume");
	FILE *m = fopen(p, "r");
	long n;

	free(p);
	if (!m)
		return;
	while (fgets(l, sizeof l, m))
		if (!strncmp(l, "mddf ", 5))
			unhex(l + 5, vmddf, 512);
		else if (!strncmp(l, "blocks ", 7))
			volblocks = atol(l + 7);
		else if (!strncmp(l, "name ", 5))
			sscanf(l + 5, "%63[^\n]", volname);
		else if (!strncmp(l, "overhead ", 9))
			vover = atol(l + 9);
	fclose(m);
	/* host files need no scavenging: an unclean stop's open flag goes */
	vmddf[0x138] = 0;
	if ((n = vmap()) >= 0 && n != (long)G32P(vmddf + 0xba))
		fprintf(stderr, "lisa: %ld pages free, the files leave %ld\n", (long)G32P(vmddf + 0xba), n);
	else if (verbose)
		fprintf(stderr, "lisa: %ld pages free, %ld the volume's\n", n, vover);
}

static void
volsave()
{
	char *p = envpath(root, ".volume");
	FILE *m = fopen(p, "w");
	int i;

	free(p);
	if (!m)
		return;
	vmap();
	fprintf(m, "name %s\nblocks %ld\noverhead %ld\nmddf ", volname, volblocks, vover);
	for (i = 0; i < 512; i++)
		fprintf(m, "%02x", vmddf[i]);
	fprintf(m, "\n");
	fclose(m);
}

static void
hcmddf(r)
	unsigned long *r;
{
	unsigned long buf = G32P(ARG(r, 10));
	if (*ARG(r, 8)) {
		gmem(buf, vmddf, 314, 0, 0L);
		if (verbose)
			fprintf(stderr, "lisa: MDDF written, %ld pages free, the files leave %ld\n",
			    (long)G32P(vmddf + 0xba), vmap());
		volsave();
	} else
		gwrite(buf, vmddf, 314);
	errout(G32P(ARG(r, 16)), 0);
}

static void
hcbmap(r)
	unsigned long *r;
{
	unsigned long buf = G32P(ARG(r, 8)), vol = G32(r[13] + cdva5 + 4 * G16P(ARG(r, 14)));
	long n = G16(G32(vol + 54) + 144);

	if (!*ARG(r, 12) && n > 0 && vmap() >= 0)
		gwrite(buf, vbits, (int)(n < vbn ? n : vbn));
	errout(G32P(ARG(r, 16)), 0);
}

/* ---- host services ---- */

static int pastboot;
static int patchload(), patchcops(), patchmmu(), patchserial(), patchprobe(), word();
static void hcreset();

/* before each read, the code read and checked so far may need patches */
static void
rampatches()
{
	int k;

	if (!copsdone && !nboot) {
		nscan++;
		if ((k = patchload(lfd)) > 0 && verbose)
			fprintf(stderr, "lisa: loader rounds to 4 KB (%d)\n", k);
		if ((k = patchmmu(lfd)) > 0 && verbose)
			fprintf(stderr, "lisa: MMU setup by host call (%d)\n", k);
		if ((k = patchserial(lfd)) > 0 && verbose)
			fprintf(stderr, "lisa: serial number by host call (%d)\n", k);
		if ((k = patchunpack(lfd)) > 0 && verbose)
			fprintf(stderr, "lisa: unpacked code by host call (%d)\n", k);
		if ((k = patchprobe(lfd)) > 0 && verbose)
			fprintf(stderr, "lisa: loader's controller probe skipped (%d)\n", k);
		if ((k = patchcops(lfd)) > 0) {
			if (verbose)
				fprintf(stderr, "lisa: controller send patched (%d)\n", k);
			copsdone = 1;
		}
	}
}

/* the loader's patches in a file's bytes f[0..n) as it is read (cops: the controller send only) */
static void
ldrscan(fid, page0, f, n, cops)
	int fid, page0, cops;
	unsigned char *f;
	long n;
{
	int k;

	sm = f;
	plo = 0;
	phi = n;
	pfile = 1;
	pfid = fid;
	ppage0 = page0;
	if (!cops) {
		if ((k = patchload(lfd)) > 0 && verbose)
			fprintf(stderr, "lisa: loader rounds to 4 KB (%d)\n", k);
		if ((k = patchmmu(lfd)) > 0 && verbose)
			fprintf(stderr, "lisa: MMU setup by host call (%d)\n", k);
		if ((k = patchserial(lfd)) > 0 && verbose)
			fprintf(stderr, "lisa: serial number by host call (%d)\n", k);
		if ((k = patchunpack(lfd)) > 0 && verbose)
			fprintf(stderr, "lisa: unpacked code by host call (%d)\n", k);
		if ((k = patchprobe(lfd)) > 0 && verbose)
			fprintf(stderr, "lisa: loader's controller probe skipped (%d)\n", k);
	} else if ((k = patchcops(lfd)) > 0) {
		if (verbose)
			fprintf(stderr, "lisa: controller send patched (%d)\n", k);
		copsdone = 1;
	}
	sm = (unsigned char *)PHYS;
	plo = SYSAREA;
	phi = RAMSZ;
	pfile = 0;
}

/* the boot ROM's block read: D1 the block, A1 the tag buffer, A2 the data buffer; D0 0, or 1 on error */
static void
hcread(r)
	unsigned long *r;
{
	unsigned char b[BLK], x;
	unsigned long blk = r[1] & 0xffffff;
	int k;

	rampatches();
	r[0] = 1;
	/* block -1 is the drive's identity read: zeros */
	if (blk == 0xffffff) {
		memset(b, 0, BLK);
		k = -1;
	} else if ((k = bootread((long)blk, b)) < 0 && vread((long)blk, b) < 0)
		return;
	else if (k < 0) {
		if (nboot && !pastboot++)
			fprintf(stderr, "lisa: block %lu read from the volume\n", blk);
		patchblock(b);
	} else if (k == 0)
		applypat(b);
	if (k >= 0 && b[4] == 0 && b[5] == 1) {
		memcpy(b + 20, vmddf, 512);	/* the MDDF as last written */
		if (b[6] & 0x80) {		/* its checksum to match */
			for (x = k = 0; k < BLK; k++)
				x ^= b[k];
			b[11] ^= x;
		}
	}
	if (gmem(r[10], b + 20, 512, LM_WRITE | LM_BLOCK | LM_FLUSH, blk) < 0 || (r[9] && gmem(r[9], b, 20, LM_WRITE, 0L) < 0))
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
	P8(c + 35, (vblocks ? vblocks : volblocks) >> 16);
	P8(c + 36, (vblocks ? vblocks : volblocks) >> 8);
	P8(c + 37, vblocks ? vblocks : volblocks);
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
		if (gmem(data, b + 20, 512, LM_WRITE | LM_FLUSH, 0L) < 0 || gmem(tag, m, 24, LM_WRITE, 0L) < 0)
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

/* logs a segment setting the 1st, 2nd, 4th, 8th ... time it is seen, so a loop can't flood the log */
static void
segnote(seg, org, type, size)
	int seg, org, type, size;
{
	static unsigned long key[61], cnt[61];
	unsigned long k = (unsigned long)seg << 24 ^ (unsigned long)org << 8 ^ type << 4 ^ size, n;
	int i = (int)(k % 61);

	if (key[i] != k + 1) {
		key[i] = k + 1;
		cnt[i] = 0;
	}
	n = ++cnt[i];
	if (n & (n - 1))
		return;
	if (n == 1)
		fprintf(stderr, "lisa: segment %#x origin %#x type %d size %#x\n", seg, org, type, size);
	else
		fprintf(stderr, "lisa: segment %#x origin %#x type %d size %#x set %lu times\n", seg, org, type, size, n);
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
			segnote(g.lg_first + i, org, type, e[3]);
	}
	ioctl(lfd, LISAIOC_SEG, &g);
	curctx = g.lg_final & 3;
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
	curctx = g.lg_final & 3;
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


/* the next input event: D0 the key byte; or $10000 | dy, with dx stored; or $20000 the power switch; or -1 none */
static void
hcinput(r)
	unsigned long *r;
{
	long ev = ioctl(lfd, LISAIOC_INPUT, 0);

	if (ev & LIN_MOUSE) {
		P8(mousedx, ev >> 8);
		r[0] = 0x10000 | (ev & 0xff);
	} else if (ev & LIN_POWER)
		r[0] = 0x20000;
	else if (ev & LIN_KEY)
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

	if (HC_CALL(r) >= HC_CREL && HC_CALL(r) <= HC_BMAP && !cours(r, HC_CALL(r))) {
		cother(r, HC_CALL(r) - HC_CREL);
		return;
	}
	switch (HC_CALL(r)) {
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
		if ((int)(r[1] & 3) != curctx)
			ioctl(lfd, LISAIOC_CTX, curctx = (int)(r[1] & 3));
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
	case 22:
		P32(G32(G32(&r[18]) + 4) + 8, 0);	/* the command's result and status words */
		break;
	case 21:
		P32(&r[17], (G32(&r[17]) + 0xfff) & ~0xfffUL);	/* the size on the stack becomes the result */
		break;
	case 23:
		P16(r[13] + lbs, 512);
		break;
	case 24:
		hclopen(r);
		break;
	case 25:
		hclfill(r);
		break;
	case 26:
		hclmove(r);
		break;
	case 27:
		hcslist(r);
		break;
	case 28:
		hcpage(r);
		break;
	case 29:
		hcdata(r);
		break;
	case HC_CREL: hccrel(r); break;
	case HC_CGET: hccget(r); break;
	case HC_CSRCH: hccsrch(r); break;
	case HC_CPTR: hccptr(r); break;
	case HC_CNEXT: hccnext(r); break;
	case HC_CLOOK: hcclook(r); break;
	case HC_CINS: hccins(r); break;
	case HC_CDEL: hccdel(r); break;
	case HC_COPEN: hccopen(r); break;
	case HC_CENUM: hccenum(r); break;
	case HC_MDDF: hcmddf(r); break;
	case HC_BMAP: hcbmap(r); break;
	case HC_POLL: r[0] &= ~0xffUL; break;	/* the bit clear */
	case HC_MOVE: hcmove(r); break;
	case HC_IDLE:
		/* until an interrupt, which ends the wait by its signal, or 50 ms */
		poll((struct pollfd *)0, 0L, 50);
		break;
	case HC_SHRINK:
		/*
		 * D7 the pages a resize gains, D5 true for a stack: move.w 8(a4),d4;
		 * move.b d5,d0; beq.s, and a size unchanged once rounded goes to the end
		 * instead of freeing no pages at the next region's start.
		 */
		r[4] = (r[4] & ~0xffffUL) | G16(r[12] + 8);
		r[0] = (r[0] & ~0xffUL) | (r[5] & 0xff);
		r[16] += !(r[7] & 0xffff) ? 0x3c : !(r[5] & 0xff) ? 0x28 : 2;
		break;
	case HC_SEGSZ:
		/* a segment with no memory yet skips the 8 bytes for one that has some; its size rounds up */
		if (G16(r[12] + 8) == 0) {
			r[7] = (r[7] & ~0xffffUL) | ((r[7] + 7) & 0xfff8);
			r[16] += 8;
		}
		break;
	case HC_POWER:
		if (verbose)
			fprintf(stderr, "lisa: power off\n");
		exit(0);
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
	unsigned long f;
{
	romp[o] = 0x4e;
	romp[o + 1] = 0xb9;
	put32(romp + o + 2, f);
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
	if (verbose)
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
	if (verbose)
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
	unsigned long a, b, e = pfile ? phi : SYSAREA + 0x30000UL;
	int n = 0;

	for (a = plo; a + sizeof sig < e; a += 2) {
		if (sm[a] != sig[0] || memcmp(sm + a, sig, sizeof sig))
			continue;
		for (b = a; b > a - 16 && b >= plo; b -= 2)
			if (sm[b] == 0x48 && sm[b + 1] == 0xe7) {
				n += pput(fd, b, (unsigned char *)"\x4e\x75", 2);
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
	unsigned char d[2];

	d[0] = v >> 8;
	d[1] = v;
	pput(fd, a, d, 2);
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
	unsigned long a;
	int n = 0;

	for (a = plo; a + sizeof sig + 1 < phi; a += 2)
		if (sm[a] == sig[0] && !memcmp(sm + a, sig, sizeof sig)) {
			setw(fd, a + 6, 0x6000 | (8 + sm[a + 15]));
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
	unsigned char *p;
	unsigned long a;
	int n = 0, u;

	for (a = plo + 16; a + 32 < phi; a += 2) {
		p = sm + a;
		if (p[0] != 0x33 && p[0] != rnd[0])
			continue;
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
	unsigned char *p, d[16];
	unsigned long a, h = (unsigned long)hc_mmu, h1 = (unsigned long)hc_seg1;
	int n = 0;

	for (a = plo; a + 40 < phi; a += 2) {
		p = sm + a;
		if (p[0] != trap[0] && p[0] != 0x13)
			continue;
		if (!memcmp(p, trap, sizeof trap)) {
			memcpy(d, "\x4e\xb9....\x4e\x73", 8);
			put32(d + 2, h);
			n += pput(fd, a, d, 8);
			continue;
		}
		if (!latch(p, 0x08) || !latch(p + 6, 0x0c) || !latch(p + 12, 0x10) || !latch(p + 32, 0x12) ||
		    p[18] != 0x33 || (p[19] & 0xf8) != 0xc0 || p[24] != 0x33 || p[25] != 0xfc ||
		    ((p[20] << 24 | p[21] << 16 | p[22] << 8 | p[23]) ^ (p[28] << 24 | p[29] << 16 | p[30] << 8 | p[31])) != 8)
			continue;
		d[0] = 0x4e;
		d[1] = 0xb9;
		put32(d + 2, h1);
		memcpy(d + 6, p + 26, 6);		/* SLR, its address */
		d[12] = (p[5] & 2) >> 1 | (p[11] & 2);	/* context */
		d[13] = p[19] & 7;			/* the SOR's register */
		d[14] = 0x60;
		d[15] = 38 - 16;			/* bra.s past the rest */
		n += pput(fd, a, d, 16);
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
	unsigned char d[6 + sizeof out];
	unsigned long a, b, e;
	int n = 0;

	for (a = plo + 16; a + sizeof sig < phi; a += 2) {
		if (sm[a] != sig[0] || memcmp(sm + a, sig, sizeof sig))
			continue;
		for (b = a - 2; b > a - 16 && (sm[b] != 0x48 || sm[b + 1] != 0xe7); b -= 2)
			;
		for (e = a; e < a + 0x200 && e + sizeof out < phi && memcmp(sm + e, out, sizeof out); e += 2)
			;
		if (b <= a - 16 || e >= a + 0x200 || e + sizeof out >= phi)
			continue;
		d[0] = 0x4e;
		d[1] = 0xb9;
		put32(d + 2, (unsigned long)hc_serial);
		memcpy(d + 6, out, sizeof out);
		n += pput(fd, b, d, sizeof d);
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

/* the Lisa's screen, rows lrb bytes apart, and its size on the host */
static int lw = SCRW, lh = SCRH, lrb = SCRW / 8, ow, oh;

/* host input to the module's queue as Lisa key bytes (bit 7 down; the mouse button is 6) and mouse deltas; 1 if any */
static int
events(lf, kfd, mfd)
	int lf, kfd, mfd;
{
	static int cmd;
	static long rx, ry;
	struct inev ev[32];
	long dx = 0, dy = 0, x, y;
	int fd, n, i, k, got = 0;

	for (fd = kfd; fd >= 0; fd = fd == kfd ? mfd : -1)
		while ((n = read(fd, (char *)ev, sizeof ev)) > 0)
			for (i = 0, got = 1; i < n / (int)sizeof ev[0]; i++)
				switch (ev[i].ie_type) {
				case IE_KEY:
					if (ev[i].ie_code == 0x37)
						cmd = ev[i].ie_value != 0;
					if (ev[i].ie_code == 0x7f && ev[i].ie_value)
						ioctl(lf, LISAIOC_INPUT, LIN_POWER);
					else if (cmd && ev[i].ie_code == 0x0c) {
						if (ev[i].ie_value)
							kill(getppid(), SIGTERM);	/* Cmd-Q */
					}
					else if (ev[i].ie_code < 128 && (k = adb2lisa[ev[i].ie_code]) != 0)
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
	/*
	 * Host pixels to Lisa ones, remainders kept.  The OS scales dy for its
	 * tall pixels, so a screen shown 3:2 at full width takes deltas as they are.
	 */
	rx += dx * lw;
	ry += dy * 3 * lh;
	dx = rx / ow;
	dy = ry / (2 * oh);
	rx -= dx * ow;
	ry -= dy * 2 * oh;
	while (dx || dy) {
		x = dx < -127 ? -127 : dx > 127 ? 127 : dx;
		y = dy < -127 ? -127 : dy > 127 ? 127 : dy;
		ioctl(lf, LISAIOC_INPUT, LIN_MOUSE | (x & 0xff) << 8 | (y & 0xff));
		dx -= x;
		dy -= y;
	}
	return got;
}

/*
 * The display process: the video page on the screen, and the boot's progress in the log.
 * Lisa pixels are about 2:3, so rows are shown 3:2 (one once, the next twice) and
 * columns 1:1, centred.  A host narrower than the Lisa gets it scaled down to fit,
 * aspect kept: Lisa columns (rows) that land on one host column (row) are ORed, so
 * one-pixel black strokes survive.  At 632 wide one column in eight pairs with its neighbour.
 *
 * Rows are compared every REFRESH ms while the screen changes or input comes,
 * less often when it stays the same.  A row is converted by table, a byte at a time,
 * and written to each host line it covers from the table or the converted line.
 */
#define	REFRESH	20		/* ms between compares */
#define	CALM	8		/* unchanged compares before each doubling, up to 8 * REFRESH */

static unsigned long ex[256][2];	/* a Lisa byte at full width: 8 host pixels, black 0 */
static unsigned long (*sxt)[256][2];	/* scaled: per column pattern, a byte ANDed onto up to 8 pixels */
static unsigned long *sline;		/* a scaled row */
static struct sxe {
	unsigned long *pat;		/* a byte column's pattern */
	unsigned long *at;		/* the line from its first host column */
} *sx;
static unsigned char *last, *acc;
static unsigned char *fb;

extern int gettimeofday();

/* ms since the first call */
static long
msnow()
{
	static long t0;
	struct timeval tv;

	gettimeofday(&tv, (void *)0);
	if (t0 == 0)
		t0 = tv.tv_sec;
	return (tv.tv_sec - t0) * 1000L + tv.tv_usec / 1000;
}

/* this process's CPU time, ms */
static long
cpums()
{
	struct tms t;

	times(&t);
	return (t.tms_utime + t.tms_stime) * 1000L / sysconf(_SC_CLK_TCK);
}

/* the scaling tables; 0 if out of memory */
static int
scaling()
{
	unsigned char (*o)[8];
	int j, k, b, p, x, np = 0, nb = lw / 8;

	if ((o = (unsigned char (*)[8])malloc(nb * 8)) == 0)
		return 0;
	for (j = 0; j < nb; j++) {
		x = 8L * j * ow / lw;
		sx[j].at = (unsigned long *)((unsigned char *)sline + x);
		for (k = 0; k < 8; k++)
			o[j][k] = (8L * j + k) * ow / lw - x;
	}
	for (j = 0; j < nb; j++) {
		for (p = 0; p < j && memcmp(o[p], o[j], 8); p++)
			;
		np += p == j;
	}
	if (sxt)
		free((char *)sxt);
	if ((sxt = (unsigned long (*)[256][2])malloc(np * sizeof *sxt)) == 0) {
		free((char *)o);
		return 0;
	}
	for (j = np = 0; j < nb; j++) {
		for (p = 0; p < j && memcmp(o[p], o[j], 8); p++)
			;
		if (p < j) {
			sx[j].pat = sx[p].pat;
			continue;
		}
		sx[j].pat = sxt[np][0];
		for (b = 0; b < 256; b++) {
			sxt[np][b][0] = sxt[np][b][1] = ~0UL;
			for (k = 0; k < 8; k++)
				if (b >> (7 - k) & 1)
					sxt[np][b][o[j][k] / 4] &= ~(0xffUL << (8 * (3 - o[j][k] % 4)));
		}
		np++;
	}
	free((char *)o);
	return 1;
}

/* 1 if n bytes (even, 2-byte aligned) match */
static int
same(a, b, n)
	unsigned char *a, *b;
	long n;
{
	unsigned long *p = (unsigned long *)a, *q = (unsigned long *)b;
	unsigned short *u, *v;

	for (; n >= 16; n -= 16)
		if (*p++ != *q++ || *p++ != *q++ || *p++ != *q++ || *p++ != *q++)
			return 0;
	for (u = (unsigned short *)p, v = (unsigned short *)q; n > 0; n -= 2)
		if (*u++ != *v++)
			return 0;
	return 1;
}

/* Lisa row l at full width onto n host lines from d, rb apart, two at a time */
static void
xrow(l, d, rb, n)
	unsigned char *l, *d;
	long rb;
	int n;
{
	unsigned char *u, *e = l + lw / 8;
	unsigned long *p, *q, *t;

	for (; n > 1; n -= 2, d += 2 * rb)
		for (u = l, p = (unsigned long *)d, q = (unsigned long *)(d + rb); u < e; p += 2, q += 2) {
			t = ex[*u++];
			p[0] = q[0] = t[0];
			p[1] = q[1] = t[1];
		}
	if (n)
		for (u = l, p = (unsigned long *)d; u < e; p += 2) {
			t = ex[*u++];
			p[0] = t[0];
			p[1] = t[1];
		}
}

/*
 * Lisa row l scaled onto n host lines from d.  Pointer loops, bounds in locals and
 * a small function keep the old compiler's loops short: it reloads globals
 * after every store and spills in a big function.
 */
static void
srow(l, d, rb, n)
	unsigned char *l, *d;
	long rb;
	int n;
{
	unsigned long *t, *u, *z, *p;
	unsigned char *e = l + lw / 8;
	int b, w = ow;
	struct sxe *x;

	for (u = sline, z = sline + (w + 3) / 4; u + 4 <= z; u += 4)
		u[0] = u[1] = u[2] = u[3] = ~0UL;
	while (u < z)
		*u++ = ~0UL;
	/* a pattern's longs may start at any byte */
	for (x = sx; l < e; x++)
		if ((b = *l++) != 0) {
			t = x->pat + 2 * b;
			u = x->at;
			u[0] &= t[0];
			u[1] &= t[1];
		}
	for (z = sline + w / 4; n > 0; n--, d += rb) {
		for (t = sline, p = (unsigned long *)d; t + 4 <= z; t += 4, p += 4) {
			p[0] = t[0];
			p[1] = t[1];
			p[2] = t[2];
			p[3] = t[3];
		}
		while (t < z)
			*p++ = *t++;
		memcpy((char *)p, (char *)t, w % 4);
	}
}

/* the screen's changed rows (all of them when all) to fb; the Lisa rows drawn */
static int
draw(scr, rb, all)
	unsigned char *scr;
	long rb;
	int all;
{
	unsigned char *s, *l;
	int y, e, r, j, n, oy, nb = lw / 8, rows = 0;

	if (!all && same(last, scr, (long)lrb * lh))
		return 0;
	/* rows y..e-1 land on host rows oy..oy+n-1 */
	for (y = 0; y < lh; y = e) {
		oy = (long)y * oh / lh;
		for (e = y + 1; e < lh && (long)e * oh / lh == oy; e++)
			;
		n = (long)e * oh / lh - oy;
		s = scr + (long)y * lrb;
		l = last + (long)y * lrb;
		if (!all && same(l, s, (long)(e - y) * lrb))
			continue;
		memcpy(l, s, (e - y) * lrb);
		rows += e - y;
		if (e - y > 1) {
			memcpy(acc, l, nb);
			for (r = 1; r < e - y; r++)
				for (j = 0; j < nb; j++)
					acc[j] |= l[r * lrb + j];
			l = acc;
		}
		if (ow == lw)
			xrow(l, fb + oy * rb, rb, n);
		else
			srow(l, fb + oy * rb, rb, n);
	}
	return rows;
}

/* the screen's size and place on the display as it is now; 0 if it fails */
static int
layout(d)
	struct nh_display *d;
{
	unsigned long size = d->info.fi_size;
	long a = 3L * lh / 2, w, h;

	if (ioctl(d->framebuffer, FBIOGINFO, &d->info) < 0)
		return 0;
	if (d->info.fi_size != size) {
		munmap((caddr_t)d->mapping, size);
		d->mapping = (void *)mmap((caddr_t)0, d->info.fi_size, PROT_READ | PROT_WRITE, MAP_SHARED, d->framebuffer, (off_t)0);
		if (d->mapping == (void *)-1)
			return 0;
	}
	w = d->info.fi_width;
	h = d->info.fi_height;
	if (w >= lw) {
		ow = lw;
		oh = h < a ? h : a;
	} else {
		ow = w;
		oh = a * w / lw;
		if (oh > h) {
			oh = h;
			ow = lw * h / a;
		}
	}
	fb = (unsigned char *)d->mapping + d->info.fi_offset + (h - oh) / 2 * d->info.fi_rowbytes + (w - ow) / 2;
	memset(d->mapping, 0, d->info.fi_size);
	return ow == lw || scaling();
}

/* the display mode nearest the setting */
static void
request(d)
	struct nh_display *d;
{
	struct fbreq rq;
	int w, h;

	if (!strcmp(screen, "lisa")) {
		w = lw;
		h = 3 * lh / 2;
	} else if (sscanf(screen, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
		fprintf(stderr, "startlisa: screen %s: not WxH or lisa\n", screen);
		return;
	}
	memset(&rq, 0, sizeof rq);
	rq.rq_width = w;
	rq.rq_height = h;
	rq.rq_depth = 8;
	if (ioctl(d->framebuffer, FBIOREQMODE, &rq) < 0)
		fprintf(stderr, "startlisa: screen %dx%d: %s\n", w, h, strerror(errno));
}

static void
show(fd)
	int fd;
{
	static unsigned char bits[256];
	struct nh_display d;
	struct fbcmap colors;
	unsigned short grey[256];
	struct lisastat st, was;
	struct fbnote fn;
	unsigned char *scr;
	pid_t pp = getppid();
	unsigned long lit, lastlit = 0, vid = ~0UL;
	long t = 0, tlog = 0, tlit = 0, tin = 0, t0, tdraw = 0, nrows = 0, lrows = 0, lat = 0, nlat = 0, per;
	int i, k, rows, have = 0, calm = 0;
	struct pollfd pf[3];

	close(vfd);
	nh_display_init(&d);
	if (nh_display_open(&d, "/dev/fb0", "Lisa", 1) < 0) {
		fprintf(stderr, "startlisa: display: %s\n", strerror(errno));
		kill(pp, SIGKILL);
		_exit(1);
	}
	if (d.info.fi_depth != 8) {
		fprintf(stderr, "startlisa: the display has %d bits per pixel; the Lisa needs 8\n", (int)d.info.fi_depth);
		kill(pp, SIGKILL);
		_exit(1);
	}
	if (screen)
		request(&d);
	sx = (struct sxe *)malloc(lw / 8 * sizeof *sx);
	sline = (unsigned long *)malloc((lw / 4 + 3) * sizeof *sline);
	last = (unsigned char *)malloc((long)lrb * lh);
	acc = (unsigned char *)malloc(lw / 8);
	if (!sx || !sline || !last || !acc || !layout(&d)) {
		fprintf(stderr, "startlisa: display: %s\n", strerror(errno));
		kill(pp, SIGKILL);
		_exit(1);
	}
	for (i = 0; i < 256; i++) {
		bits[i] = (i & 1) + bits[i / 2];
		grey[i] = i * 257;
		for (k = 0; k < 8; k++)
			if (!(i >> (7 - k) & 1))
				ex[i][k / 4] |= 0xffUL << (8 * (3 - k % 4));
	}
	memset(&colors, 0, sizeof colors);
	colors.cm_count = 256;
	colors.cm_red = colors.cm_green = colors.cm_blue = grey;
	ioctl(d.framebuffer, FBIOPUTCMAP, &colors);
	if (verbose)
		fprintf(stderr, "lisa: display %dx%d, the screen at %dx%d\n", (int)d.info.fi_width, (int)d.info.fi_height, ow, oh);
	memset(&was, 0, sizeof was);
	pf[0].fd = d.keyboard;
	pf[1].fd = d.mouse;
	pf[2].fd = d.framebuffer;
	pf[0].events = pf[1].events = pf[2].events = POLLIN;
	for (per = REFRESH;;) {
		i = t + per - msnow();
		if (poll(pf, 3, i < 0 ? 0 : i > per ? per : i) > 0) {
			if ((pf[0].revents | pf[1].revents) & (POLLHUP | POLLERR)) {
				kill(pp, SIGTERM);	/* the session closed */
				pf[0].fd = pf[1].fd = pf[2].fd = -1;
				continue;
			}
			/* a mode on trial not kept: the previous one is back */
			if (pf[2].revents & POLLIN) {
				if (read(d.framebuffer, (char *)&fn, sizeof fn) != sizeof fn)
					pf[2].fd = -1;
				else if (fn.fn_type == FBN_MODE) {
					if (!layout(&d)) {
						fprintf(stderr, "startlisa: display: %s\n", strerror(errno));
						kill(pp, SIGKILL);
						_exit(1);
					}
					vid = ~0UL;
					if (verbose)
						fprintf(stderr, "lisa: display %dx%d, the screen at %dx%d\n", (int)d.info.fi_width, (int)d.info.fi_height, ow, oh);
				}
			}
			if (events(fd, d.keyboard, d.mouse)) {
				calm = 0;
				if (!tin)
					tin = msnow();
			}
		}
		per = REFRESH << (calm < 3 * CALM ? calm / CALM : 3);
		if (msnow() - t < per)
			continue;
		t = msnow();
		if (kill(pp, 0) < 0)
			_exit(0);
		if (ioctl(fd, LISAIOC_STAT, &st) < 0)
			continue;
		if (verbose && !have++)
			fprintf(stderr, "lisa: running\n");
		if (verbose && (st.ls_flags & LSF_VIDEO) && st.ls_vid != was.ls_vid)
			fprintf(stderr, "lisa: video page %d\n", st.ls_vid);
		if (verbose && (st.ls_flags & LSF_PFREAD) && !(was.ls_flags & LSF_PFREAD))
			fprintf(stderr, "lisa: first volume read\n");
		if (verbose && st.ls_berr != was.ls_berr)
			fprintf(stderr, "lisa: bus error at %#lx, pc %#lx\n", st.ls_berraddr, st.ls_berrpc);
		if (st.ls_hwfault != was.ls_hwfault)
			fprintf(stderr, "lisa: hardware access at %#lx, pc %#lx\n", st.ls_hwaddr, st.ls_hwpc);
		if (verbose && t - tlog >= 4000) {
			tlog = t;
			fprintf(stderr, "lisa: remaps %lu intr %lu blocks %lu last %lu faults %lu switches %lu a-line %lu alias %lu pushes %lu\n",
			    st.ls_remap, st.ls_intr, st.ls_pfreads, st.ls_pfblock,
			    st.ls_hwfault, st.ls_ctxsw, st.ls_aline, st.ls_alias, st.ls_cpush);
			if (nrows != lrows)
				fprintf(stderr, "lisa: drew %ld rows in %ld ms, input to screen %ld ms\n", nrows, tdraw, nlat ? lat / nlat : 0);
			lrows = nrows;
		}
		was = st;
		if (!(st.ls_flags & LSF_VIDEO) || (unsigned long)st.ls_vid * 0x8000 + (unsigned long)lrb * lh > RAMSZ)
			continue;
		scr = (unsigned char *)PHYS + st.ls_vid * 0x8000UL;
		if (verbose && t - tlit >= 500) {
			tlit = t;
			for (lit = i = 0; i < lrb * lh; i++)
				lit += bits[scr[i]];
			if (lit != lastlit) {
				lastlit = lit;
				fprintf(stderr, "lisa: screen %lu pixels lit\n", lit);
			}
		}
		if (verbose && vid == ~0UL) {
			for (k = 0, t0 = cpums(); k < 256 && cpums() - t0 < 500; k++)
				draw(scr, (long)d.info.fi_rowbytes, 1);
			t0 = (cpums() - t0) * 10 / k;
			fprintf(stderr, "lisa: full-screen draw %ld.%ld ms\n", t0 / 10, t0 % 10);
		}
		t0 = msnow();
		rows = draw(scr, (long)d.info.fi_rowbytes, vid != st.ls_vid);
		vid = st.ls_vid;
		tdraw += msnow() - t0;
		nrows += rows;
		if (!rows) {
			if (calm < 3 * CALM)
				calm++;
			continue;
		}
		calm = 0;
		if (tin) {
			lat += msnow() - tin;
			nlat++;
			tin = 0;
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
	char *rompath = 0, x = 0;
	volatile char *q;

	while ((c = getopt(argc, argv, "i:p:r:s:v")) != -1)
		switch (c) {
		case 'i': disk = optarg; break;
		case 'r': rompath = optarg; break;
		case 's': screen = optarg; break;
		case 'p': pmfile = optarg; break;
		case 'v': verbose = 1; break;
		default:
			fprintf(stderr, "usage: startlisa [-e env] -- [-i volume] [-p parameters] [-r rom] [-s WxH|lisa] [-v]\n");
			return 2;
		}
	if ((root = getenv("NATIVEHOST_ROOT")) == 0 || !*root) {
		fprintf(stderr, "startlisa: NATIVEHOST_ROOT names no environment folder\n");
		return 2;
	}
	/* with the loader's blocks in .boot, the ProFile is only for the OS's own I/O */
	if (!disk && (bootopen() < 0 || access(envpath(root, "profile.dc42"), 0) == 0))
		disk = envpath(root, "profile.dc42");
	volread();
	memset((char *)ctramp, 0, sizeof ctramp);	/* in before the guest runs */
	if (nboot)
		bootpats();
	if (!pmfile)
		pmfile = envpath(root, "parameter-memory");
	memset(pm, 0xff, sizeof pm);
	if (pmfile && (fd = open(pmfile, O_RDONLY)) >= 0) {
		read(fd, pm, sizeof pm);
		close(fd);
	}
	if (!screen && (fd = open(envpath(root, "screen"), O_RDONLY)) >= 0) {
		static char sb[32];

		c = read(fd, sb, sizeof sb - 1);
		close(fd);
		sb[c > 0 ? c : 0] = 0;
		sb[strcspn(sb, "\n")] = 0;
		if (*sb)
			screen = sb;
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
	if (disk && (vopen() < 0 || fbufinit() < 0))
		die(disk);
	if (disk && oopen(envpath(root, "profile.writes")) < 0)
		die("profile.writes");
	if ((pid = fork()) < 0)
		die("fork");
	if (pid == 0)
		show(fd);
	/* our pages in before the guest runs: a page first touched inside a host call breaks it */
	for (q = (char *)_start; q < &etext; q += 1024)
		x += *q;
	for (q = (char *)&hc_sp; q < &_end; q += 1024)
		x += *q;
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
		/* the guest's stack is the OS's: handlers run on their own */
		{
			stack_t ss;

			ss.ss_sp = fstack;
			ss.ss_size = sizeof fstack;
			ss.ss_flags = 0;
			sigaltstack(&ss, (stack_t *)0);
			sa.sa_handler = stopreq;
			sa.sa_flags = SA_ONSTACK;
			sigaction(SIGTERM, &sa, (struct sigaction *)0);
			sigaction(SIGINT, &sa, (struct sigaction *)0);
			sigaction(SIGHUP, &sa, (struct sigaction *)0);
			sigaction(SIGALRM, &sa, (struct sigaction *)0);
		}
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
