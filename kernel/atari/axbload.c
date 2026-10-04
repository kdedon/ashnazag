/*
 * AXB loader: run by the root sector under TOS 3 or 4.  Offers TOS or
 * Unix, then loads the kernel ELF from the AXB partition, builds the
 * Linux/m68k boot record and enters the kernel with MMU and caches off.
 *
 * AXB layout (512-byte sectors): 0-14 this loader, 15 the command line
 * (text, NUL-terminated), 16 on the kernel ELF.  In the command line,
 * "root=c?" gets the IDE unit of the boot disk.
 */

#define CMDSEC	15
#define KERNSEC	16
#define CHUNK	64		/* sectors per DMAread */
#define BIMAX	1024

#define PHYSTOP	(*(volatile unsigned long *)0x42e)
#define HZ200	(*(volatile unsigned long *)0x4ba)
#define COOKIES	(*(unsigned long **)0x5a0)
#define RAMTOP	(*(unsigned long *)0x5a4)
#define RAMVALID (*(unsigned long *)0x5a8)
#define TTRAM	0x01000000UL

struct go {
	unsigned long cpu, entry, stack, bidst, bisrc, bilen, nseg;
	struct { unsigned long dst, src, filesz, memsz; } seg[4];
};

extern long trap_frame(long trap, unsigned char *frame, long size);
extern char go_kernel[], go_end[];
extern void go(char *, struct go *);
extern unsigned long ax_timeout, ax_ksum;

static unsigned char frame[16];
static int nframe;

static void w16(unsigned long v) { frame[nframe++] = v >> 8; frame[nframe++] = v; }
static void w32(unsigned long v) { w16(v >> 16); w16(v); }
static long sys(long trap) { long n = nframe; nframe = 0; return trap_frame(trap, frame, n); }

static void
putc_(int c)
{
	w16(3); w16(2); w16(c);		/* Bconout(CON, c) */
	sys(13);
}

static void
puts_(const char *s)
{
	for (; *s; s++) {
		if (*s == '\n')
			putc_('\r');
		putc_(*s);
	}
}

static void
putn(unsigned long n)
{
	char b[12];
	int i = 0;

	do
		b[i++] = '0' + n % 10;
	while ((n /= 10) != 0);
	while (i)
		putc_(b[--i]);
}

static long
dmaread(unsigned long sec, int n, void *buf, int dev)
{
	w16(42); w32(sec); w16(n); w32((unsigned long)buf); w16(dev);
	return sys(14);
}

/* ST-RAM block: Mxalloc(n, ST only), Malloc where Mxalloc is missing */
static long
stalloc(long n)
{
	long r;

	w16(0x44); w32(n); w16(0);
	r = sys(1);
	if (r == -32) {
		w16(0x48); w32(n);
		r = sys(1);
	}
	return r;
}

static int
key(void)
{
	w16(1); w16(2);			/* Bconstat(CON) */
	if (sys(13) == 0)
		return -1;
	w16(2); w16(2);			/* Bconin(CON) */
	return sys(13) & 0xff;
}

static unsigned long
cookie(unsigned long id, unsigned long dflt)
{
	unsigned long *p = COOKIES;

	if (p)
		for (; p[0]; p += 2)
			if (p[0] == id)
				return p[1];
	return dflt;
}

/* NVRAM boot preference: none or SysV means Unix; Alternate held skips Unix */
static int
want_unix(void)
{
	unsigned char pref = 0;
	unsigned long t;
	int dflt, c;

	w16(46); w16(0); w16(0); w16(1); w32((unsigned long)&pref);
	if (sys(14) != 0)
		pref = 0;
	w16(11); w16(0xffff);		/* Kbshift(-1) */
	if (sys(13) & 8)
		return 0;
	dflt = pref == 0 || pref == 0x40;
	puts_(dflt ? "\nUnix in " : "\nTOS in ");
	putn(ax_timeout);
	puts_(dflt ? " s.  T: TOS, Return: Unix\n" : " s.  U: Unix, Return: TOS\n");
	for (t = HZ200; HZ200 - t < ax_timeout * 200;)
		switch (c = key()) {
		case 't': case 'T': return 0;
		case 'u': case 'U': return 1;
		case '\r': return dflt;
		}
	return dflt;
}

static unsigned long
be32(unsigned char *p)
{
	return (unsigned long)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static unsigned char bi[BIMAX];
static int nbi;

static void
birec(int tag, const void *d, int len)
{
	int size = (4 + len + 3) & ~3, i;

	if (nbi + size + 4 > BIMAX)
		return;
	bi[nbi] = tag >> 8; bi[nbi + 1] = tag;
	bi[nbi + 2] = size >> 8; bi[nbi + 3] = size;
	for (i = 0; i < size - 4; i++)
		bi[nbi + 4 + i] = i < len ? ((const unsigned char *)d)[i] : 0;
	nbi += size;
}

static void
bilong(int tag, unsigned long v)
{
	birec(tag, &v, 4);
}

static void
mfree(unsigned long p)
{
	w16(0x49); w32(p);
	sys(1);
}

static long
fail(const char *s)
{
	puts_("boot: ");
	puts_(s);
	puts_("; starting TOS\n");
	return 0;
}

long
loader(long dev, unsigned long axb, unsigned char *root)
{
	static unsigned char sec[512], cmd[512];
	static struct go gk;
	unsigned char *ph;
	unsigned long cpu, fpu, kend, fend, fsz, n, i, blk, size, buf, mem[2];
	unsigned long off, va, fs, ms, sum;
	int ent = 0;
	struct go *g;
	char *p, *base;

	(void)root;
	if (!want_unix())
		return 0;
	if (dmaread(axb + CMDSEC, 1, cmd, dev) || dmaread(axb + KERNSEC, 1, sec, dev))
		return fail("read error");
	cmd[255] = 0;
	n = sec[44] << 8 | sec[45];
	if (be32(sec) != 0x7f454c46 || sec[4] != 1 || sec[5] != 2
	    || sec[18] != 0 || sec[19] != 4 || sec[42] != 0 || sec[43] != 32
	    || be32(sec + 28) > 512 || n > (512 - be32(sec + 28)) / 32)
		return fail("no m68k ELF kernel");
	for (p = (char *)cmd; *p; p++)
		if (p[0] == 'r' && p[1] == 'o' && p[2] == 'o' && p[3] == 't'
		    && p[4] == '=' && p[5] == 'c' && p[6] == '?')
			p[6] = dev == 16 || dev == 17 ? '0' + dev - 16 : '?';

	/* every bound below 2 GB, so no sum wraps; entry in a segment */
	kend = fend = 0;
	gk.nseg = 0;
	gk.entry = be32(sec + 24);
	for (i = 0, ph = sec + be32(sec + 28); i < n; i++, ph += 32) {
		if (be32(ph) != 1)
			continue;
		off = be32(ph + 4);
		va = be32(ph + 12);
		fs = be32(ph + 16);
		ms = be32(ph + 20);
		if (gk.nseg == 4 || fs > ms || off >= 0x80000000 || va >= 0x80000000
		    || fs > 0x80000000 - off || ms > 0x80000000 - va)
			return fail("bad kernel segments");
		gk.seg[gk.nseg].dst = va;
		gk.seg[gk.nseg].src = off;
		gk.seg[gk.nseg].filesz = fs;
		gk.seg[gk.nseg].memsz = ms;
		gk.nseg++;
		if (va + ms > kend)
			kend = va + ms;
		if (off + fs > fend)
			fend = off + fs;
		if (gk.entry >= va && gk.entry < va + fs)
			ent = 1;
	}
	if (!ent)
		return fail("kernel entry not in a segment");
	kend = (kend + 3) & ~3UL;
	fsz = (fend + 511) & ~511UL;

	/* load the file at the top of the largest ST-RAM block, the hand-off
	   area below it, all above the kernel's end and boot record */
	size = stalloc(-1);
	if ((long)size <= 0 || fsz + 2048 > size)
		return fail("not enough memory");
	blk = stalloc(size);
	if ((long)blk <= 0)
		return fail("not enough memory");
	buf = (blk + size - fsz) & ~3UL;
	if (kend + BIMAX > buf - 2048) {
		mfree(blk);
		return fail("not enough memory");
	}
	puts_("Loading kernel");
	for (i = 0; i < fsz / 512; i += CHUNK) {
		n = fsz / 512 - i < CHUNK ? fsz / 512 - i : CHUNK;
		if (dmaread(axb + KERNSEC + i, n, (char *)buf + i * 512, dev)) {
			mfree(blk);
			return fail("read error");
		}
		if (i % (16 * CHUNK) == 0)
			putc_('.');
	}
	putc_('\n');
	for (sum = 0, i = 0; i < fend; i += 4)
		sum += *(unsigned long *)(buf + i);
	if (sum != ax_ksum) {
		mfree(blk);
		return fail("kernel checksum");
	}

	/* boot record, as the Linux/m68k bootstrap builds it */
	cpu = cookie(0x5f435055, 30);			/* _CPU */
	fpu = cookie(0x5f465055, 0) >> 16;		/* _FPU */
	bilong(1, 2);					/* MACH_ATARI */
	bilong(2, cpu >= 60 ? 8 : cpu >= 40 ? 4 : 2);
	bilong(3, fpu & 0x10 ? 8 : fpu & 0x08 ? 4 : (fpu & 6) == 6 ? 2
	    : fpu & 6 ? 1 : 0);
	bilong(4, cpu >= 60 ? 8 : cpu >= 40 ? 4 : 2);
	mem[0] = 0;
	mem[1] = PHYSTOP;
	birec(5, mem, 8);
	if (RAMVALID == 0x1357bd13 && RAMTOP > TTRAM) {
		mem[0] = TTRAM;
		mem[1] = RAMTOP - TTRAM;
		birec(5, mem, 8);
	}
	for (n = 0; cmd[n]; n++)
		;
	birec(7, cmd, n + 1);
	bilong(0x8000, cookie(0x5f4d4348, 0));		/* _MCH */
	bilong(0x8001, 0);
	nbi += 2;					/* BI_LAST */

	/* hand-off code, its parameters and the boot record below the file */
	base = (char *)buf - 2048;
	n = go_end - go_kernel;
	for (i = 0; i < n; i++)
		base[i] = go_kernel[i];
	g = (struct go *)(base + ((n + 3) & ~3UL));
	for (i = 0; i < (unsigned long)nbi; i++)
		((char *)(g + 1))[i] = bi[i];
	g->cpu = cpu >= 60 ? 60 : cpu >= 40 ? 40 : 30;
	g->entry = gk.entry;
	g->stack = 0x1000;
	g->bidst = kend;
	g->bisrc = (unsigned long)(g + 1);
	g->bilen = nbi;
	g->nseg = gk.nseg;
	for (i = 0; i < gk.nseg; i++) {
		g->seg[i].dst = gk.seg[i].dst;
		g->seg[i].src = gk.seg[i].src + buf;
		g->seg[i].filesz = gk.seg[i].filesz;
		g->seg[i].memsz = gk.seg[i].memsz;
	}
	go(base, g);
	return 0;
}
