/*
 * Atari Falcon platform layer: boot record, Videl text console, MFP
 * tick and interrupt routing, memory policy, halt, and the platform
 * entry points the generic kernel imports.
 *
 * config() runs with the MMU off and before BSS is cleared, so every
 * variable here is initialised (lands in .data).
 */
#include "../mac/video/fbcons.h"

#define VOL	__volatile__

#define ATA_BI_MAGIC	0x41747242	/* 'AtrB': d0 from the entry shim */

#define BI_LAST		0x0000
#define BI_MACHTYPE	0x0001
#define BI_CPUTYPE	0x0002
#define BI_FPUTYPE	0x0003
#define BI_MMUTYPE	0x0004
#define BI_MEMCHUNK	0x0005
#define BI_COMMAND_LINE	0x0007
#define BI_ATARI_MCH	0x8000		/* _MCH cookie */

#define MACH_ATARI	2
#define MAXCHUNK	4
#define BISIZE		1024
#define POOLSIZE	0x80000		/* ST-RAM kept from the VM: screen, DMA */

/* Videl, MFP 68901, SCC */
#define IO8(a)		(*(VOL unsigned char *)(a))
#define IO16(a)		(*(VOL unsigned short *)(a))
#define IO32(a)		(*(VOL unsigned long *)(a))
#define MONTYPE		IO8(0xFFFF8006)		/* bits 7-6: 0 mono 1 RGB 2 VGA 3 TV */
#define FAL_PAL(i)	IO32(0xFFFF9800 + 4 * (i))
#define ST_PAL(i)	IO16(0xFFFF8240 + 2 * (i))

#define MFP(r)		IO8(0xFFFFFA01 + (r))
#define MFP_AER		0x02
#define MFP_DDR		0x04
#define MFP_IERA	0x06
#define MFP_IERB	0x08
#define MFP_IPRA	0x0A
#define MFP_IPRB	0x0C
#define MFP_ISRA	0x0E
#define MFP_ISRB	0x10
#define MFP_IMRA	0x12
#define MFP_IMRB	0x14
#define MFP_VR		0x16
#define MFP_TACR	0x18
#define MFP_TBCR	0x1A
#define MFP_TCDCR	0x1C
#define MFP_TADR	0x1E
#define MFP_VBASE	0x40		/* vectors 64-79 */
#define MFP_TIMERA	13		/* channel numbers */
#define MFP_GPIP4	6
#define SCC_CTLA	IO8(0xFFFF8C81)

extern char end[], stext[];
extern unsigned long boot_arg0, boot_arg1;
extern unsigned long MAINSTORE, VSIZOFMEM, chipmem;
extern int kernel_load_address;
extern unsigned long M68Kvec[];
extern void ata_clkint(), ata_aciaint(), ata_mfpstray(), ata_fline();
extern void ata_stop();
extern unsigned long ata_nfid();
extern void ata_nfcall();
extern void ikbd_init(), ikbd_intr();
extern int ikbd_getc();
extern unsigned long ata_ticks;
extern int ata_spltty();
extern void ata_splx();
extern int ata_rd_config();
extern unsigned long rootdev, dumpdev;
extern char swapfile[];

#define DD_BMAJ		18
#define SWAP_NAME	16		/* offset of bo_name in struct bootobj */

unsigned char ata_bi[BISIZE] = { 0 };
unsigned long ata_bilen = 1;

unsigned long ata_machtype = 1, ata_cputype = 1, ata_fputype = 1;
unsigned long ata_mmutype = 1, ata_mch = 1;
unsigned long ata_chunk[MAXCHUNK][2] = { { 1, 1 } };
int ata_nchunk = 1;
unsigned long ata_pool = 1;		/* ST-RAM pool base */
unsigned long ata_screen = 1;
unsigned long ata_nf = 1;		/* debug channel id, 0 off */
int ata_nfwant = 1;
long mac_socktrace = 1;		/* socktrace: log A/UX socket calls */
long ata_rootarg = 1;			/* dd minor from root=, -1 none */

void ata_puts();
void ata_puthex();
void ata_halt();
static void ata_dskname();

/* ------------------------------------------------------------ console */

static char ata_nfbuf[2] = { 1, 0 };

void
putchar(c)
unsigned char c;
{
	if (ata_nf) {
		ata_nfbuf[0] = c;
		ata_nfcall(ata_nf, ata_nfbuf);
	}
	fbcons_kputc(c);
}

static char ata_nfline[65] = { 1 };

/* Console tty output to the debug channel too. */
void
ata_nfwrite(buf, n)
register unsigned char *buf;
register int n;
{
	register int i, s;

	if (!ata_nf)
		return;
	s = ata_spltty();
	while (n > 0) {
		for (i = 0; i < 64 && i < n; i++)
			ata_nfline[i] = buf[i];
		ata_nfline[i] = 0;
		ata_nfcall(ata_nf, ata_nfline);
		buf += i;
		n -= i;
	}
	ata_splx(s);
}

unsigned char
getchar()
{
	register int c;

	while ((c = ikbd_getc()) < 0)
		;
	return c;
}

void
ata_puts(s)
char *s;
{
	while (*s)
		putchar(*s++);
}

void
ata_puthex(v)
unsigned long v;
{
	register int i;

	ata_puts("0x");
	for (i = 28; i >= 0; i -= 4)
		putchar("0123456789abcdef"[(v >> i) & 0xF]);
}

static void
putkv(k, v)
char *k;
unsigned long v;
{
	ata_puts(k);
	ata_puthex(v);
}

/*
 * Two-colour console modes.  Timings: hht hbb hbe hdb hde hss, then
 * vft vbb vbe vdb vde vss.  Colour entries serve RGB and TV; 640x400
 * there is interlaced.
 */
#define MON_MONO	0
#define MON_RGB		1
#define MON_VGA		2
#define MON_TV		3

struct vmode {
	unsigned short	vm_mon;		/* MON_MONO, MON_RGB (RGB, TV) or MON_VGA */
	unsigned short	vm_hz;
	unsigned short	vm_h;		/* lines; width is always 640 */
	unsigned short	vm_vctl;
	unsigned short	vm_t[12];
};

static struct vmode ata_vmodes[] = {
	{ MON_VGA, 60, 480, 0x08, { 0xc6, 0x8d, 0x15, 0x273, 0x50, 0x96,
	  0x419, 0x3ff, 0x3f, 0x3f, 0x3ff, 0x415 } },
	{ MON_RGB, 50, 400, 0x06, { 0x1fe, 0x199, 0x50, 0x3ef, 0xa0, 0x1b2,
	  0x270, 0x265, 0x2f, 0x7e, 0x20e, 0x26b } },
	{ MON_RGB, 60, 400, 0x06, { 0x1ff, 0x197, 0x50, 0x3f0, 0x9f, 0x1b4,
	  0x20c, 0x201, 0x16, 0x4c, 0x1dc, 0x207 } },
	{ MON_RGB, 50, 200, 0x04, { 0x1fe, 0x199, 0x50, 0x3ef, 0xa0, 0x1b2,
	  0x271, 0x265, 0x2f, 0x7f, 0x20f, 0x26b } },
	{ MON_RGB, 60, 200, 0x04, { 0x1ff, 0x197, 0x50, 0x3f0, 0x9f, 0x1b4,
	  0x20d, 0x201, 0x16, 0x4d, 0x1dd, 0x207 } },
	{ MON_MONO, 71, 400, 0x08, { 0x1a, 0, 0, 0x20f, 0xc, 0x14,
	  0x3e9, 0, 0, 0x43, 0x363, 0x3e7 } },
};
#define NVMODE	(sizeof ata_vmodes / sizeof ata_vmodes[0])

/* video=: monitor (-1 detect), lines and refresh (0 default) */
static int ata_vmon = 1, ata_vh = 1, ata_vhz = 1, ata_vbad = 1;

static char *ata_monname[] = { "mono", "rgb", "vga", "tv" };

/* Waits for the vertical counter to restart, at most about a second. */
static void
videl_vsync()
{
	register unsigned short v, n;
	register long i;

	v = IO16(0xFFFF82A0);
	for (i = 0; i < 500000; i++) {
		n = IO16(0xFFFF82A0);
		if (n < v)
			break;
		v = n;
	}
}

/* Pixel 0 white, 1 black; 80 words per line. */
static void
videl_set(base, mon, vm)
unsigned long base;
int mon;
register struct vmode *vm;
{
	register int i;

	IO8(0xFFFF8201) = base >> 16;
	IO8(0xFFFF8203) = base >> 8;
	IO8(0xFFFF820D) = base;
	FAL_PAL(0) = 0xFCFC00FC;
	FAL_PAL(1) = 0;
	ST_PAL(0) = 0xFFF;
	ST_PAL(1) = 0;
	IO8(0xFFFF820A) = vm->vm_hz == 50 || mon == MON_VGA ? 2 : 0;	/* sync */
	videl_vsync();
	for (i = 0; i < 6; i++) {
		IO16(0xFFFF8282 + 2 * i) = vm->vm_t[i];
		IO16(0xFFFF82A2 + 2 * i) = vm->vm_t[6 + i];
	}
	IO8(0xFFFF8260) = 0;
	IO16(0xFFFF820E) = 0;
	IO16(0xFFFF8210) = 0x28;
	IO16(0xFFFF82C2) = vm->vm_vctl;
	IO16(0xFFFF82C0) = mon == MON_MONO ? 0x80 : mon == MON_VGA ? 0x186 :
	    mon == MON_TV ? 0x183 : 0x181;
	IO16(0xFFFF8266) = 0;
	if (mon == MON_MONO) {
		IO8(0xFFFF8260) = 2;
		return;
	}
	/* 2 colours, toggled across a frame or the picture can distort */
	IO16(0xFFFF8266) = 0x400;
	videl_vsync();
	IO16(0xFFFF8266) = 0;
	videl_vsync();
	IO16(0xFFFF8266) = 0x400;
}

/*
 * video=MONLINES[@HZ], MON one of mono rgb vga tv, LINES 640x480,
 * 640x400 or 640x200, HZ 50 or 60; e.g. video=rgb640x400@60.
 */
static void
ata_vparse(s)
register char *s;
{
	register char *p, *q;
	register int i, n;

	for (p = s; *p; p++) {
		if ((p != s && p[-1] != ' ') || p[0] != 'v' || p[1] != 'i'
		|| p[2] != 'd' || p[3] != 'e' || p[4] != 'o' || p[5] != '=')
			continue;
		p += 6;
		ata_vbad = 1;
		for (i = 0; i < 4; i++) {
			for (q = ata_monname[i]; *q && *p == *q; p++, q++)
				;
			if (*q == 0)
				break;
			p -= q - ata_monname[i];
		}
		if (i == 4 || p[0] != '6' || p[1] != '4' || p[2] != '0'
		|| p[3] != 'x')
			return;
		for (p += 4, n = 0; *p >= '0' && *p <= '9'; p++)
			n = n * 10 + *p - '0';
		ata_vhz = 0;
		if (p[0] == '@' && p[1] == '5' && p[2] == '0')
			ata_vhz = 50, p += 3;
		else if (p[0] == '@' && p[1] == '6' && p[2] == '0')
			ata_vhz = 60, p += 3;
		if (n == 0 || (*p && *p != ' '))
			return;
		ata_vmon = i;
		ata_vh = n;
		ata_vbad = 0;
		return;
	}
}

static void
ata_video()
{
	struct fbmode m;
	unsigned long top;
	register struct vmode *vm;
	register int i, mon, cls, hz;

	top = 0;
	for (i = 0; i < ata_nchunk; i++)
		if (ata_chunk[i][0] == 0)
			top = ata_chunk[i][1];
	if (top < 2 * POOLSIZE)
		return;
	ata_pool = top - POOLSIZE;
	ata_screen = ata_pool;
	for (;;) {
		mon = ata_vmon >= 0 ? ata_vmon : MONTYPE >> 6;
		cls = mon == MON_TV ? MON_RGB : mon;
		/* refresh as the ROM left it */
		hz = ata_vhz ? ata_vhz : IO8(0xFFFF820A) & 2 ? 50 : 60;
		vm = 0;
		for (i = NVMODE - 1; i >= 0; i--)
			if (ata_vmodes[i].vm_mon == cls && (ata_vh == 0 ||
			    ata_vmodes[i].vm_h == ata_vh) && (cls != MON_RGB ||
			    ata_vmodes[i].vm_hz == hz))
				vm = &ata_vmodes[i];
		if (vm || ata_vmon < 0)
			break;
		ata_vbad = 1;
		ata_vmon = -1;
		ata_vh = ata_vhz = 0;
	}
	videl_set(ata_screen, mon, vm);
	m.fm_base = ata_screen;
	m.fm_row = 80;
	m.fm_depth = 1;
	m.fm_width = 640;
	m.fm_height = vm->vm_h;
	(void)fbcons_attach(&m);
	ata_puts("video: ");
	ata_puts(ata_monname[mon]);
	ata_puts(vm->vm_h == 480 ? " 640x480 " : vm->vm_h == 400 ? " 640x400 " :
	    " 640x200 ");
	ata_puts(vm->vm_hz == 71 ? "71" : vm->vm_hz == 60 ? "60" : "50");
	ata_puts(" Hz\n");
}

/* ---------------------------------------------------------- boot record */

static int
ata_word(s, w)
register char *s, *w;
{
	register char *p, *q;

	for (p = s; *p; p++) {
		if (p != s && p[-1] != ' ')
			continue;
		for (q = w; *q && *p == *q; p++, q++)
			;
		if (*q == 0 && (*p == 0 || *p == ' '))
			return 1;
		if (*p == 0)
			break;
	}
	return 0;
}

/*
 * root=cNd0sM: IDE unit N (0 master, 1 slave), slice M; swap is slice 2
 * of the same unit, so M may not be 0 or 2.
 */
static void
ata_rootparse(s)
char *s;
{
	register char *p;

	for (p = s; *p; p++) {
		if ((p != s && p[-1] != ' ') || p[0] != 'r' || p[1] != 'o'
		|| p[2] != 'o' || p[3] != 't' || p[4] != '=')
			continue;
		if (p[5] == 'c' && (p[6] == '0' || p[6] == '1')
		&& p[7] == 'd' && p[8] == '0' && p[9] == 's'
		&& p[10] >= '1' && p[10] <= '7' && p[10] != '2'
		&& (p[11] == 0 || p[11] == ' ')) {
			ata_rootarg = (p[10] - '0') << 4 | (p[6] - '0');
			return;
		}
		ata_puts("config: root=: not cNd0sM (N 0-1, M 1 or 3-7), ignored\n");
	}
}

static void
bi_parse()
{
	register unsigned long n;
	register unsigned long *p;
	unsigned short tag, size;

	ata_nchunk = 0;
	ata_machtype = ata_cputype = ata_fputype = ata_mmutype = ata_mch = 0;
	ata_nfwant = 0;
	mac_socktrace = 0;
	ata_rootarg = -1;
	ata_vmon = -1;
	ata_vh = ata_vhz = ata_vbad = 0;
	for (n = 0; n + 4 <= ata_bilen; n += size) {
		tag = *(unsigned short *)(ata_bi + n);
		size = *(unsigned short *)(ata_bi + n + 2);
		if (tag == BI_LAST || size < 4 || n + size > ata_bilen)
			break;
		if (size < (tag == BI_MEMCHUNK ? 12 : 8))
			continue;
		p = (unsigned long *)(ata_bi + n + 4);
		switch (tag) {
		case BI_MACHTYPE:	ata_machtype = p[0]; break;
		case BI_CPUTYPE:	ata_cputype = p[0]; break;
		case BI_FPUTYPE:	ata_fputype = p[0]; break;
		case BI_MMUTYPE:	ata_mmutype = p[0]; break;
		case BI_ATARI_MCH:	ata_mch = p[0]; break;
		case BI_MEMCHUNK:
			if (ata_nchunk < MAXCHUNK) {
				ata_chunk[ata_nchunk][0] = p[0];
				ata_chunk[ata_nchunk][1] = p[1];
				ata_nchunk++;
			}
			break;
		case BI_COMMAND_LINE:
			ata_nfwant = ata_word((char *)p, "nfcons");
			mac_socktrace = ata_word((char *)p, "socktrace");
			ata_rootparse((char *)p);
			ata_vparse((char *)p);
			break;
		}
	}
}

/*
 * Called by the shim, MMU off, before stext.  Copies the boot record
 * (it lies at the image end, where pstart builds page tables), then
 * brings up the screen.
 */
void
ata_shim_main(bi)
unsigned char *bi;
{
	register unsigned char *s, *d;
	register unsigned long n;
	unsigned short tag, size;

	ata_nf = 0;
	s = bi;
	n = 0;
	if (s)
		for (; n + 4 <= BISIZE; n += size) {
			tag = *(unsigned short *)(s + n);
			size = *(unsigned short *)(s + n + 2);
			if (tag == BI_LAST) {
				n += 4;
				break;
			}
			if (size < 4 || (size & 1))
				break;
		}
	if (n > BISIZE)
		n = BISIZE;
	d = ata_bi;
	for (ata_bilen = n; n; n--)
		*d++ = *s++;
	bi_parse();
	if (ata_nfwant)
		ata_nf = ata_nfid("NF_STDERR");
	ata_video();
	if (ata_vbad)
		ata_puts("video=: unknown mode, using the default\n");
	ata_puts("\nAMIX/Atari entry shim: image ");
	ata_puthex((unsigned long)stext);
	ata_puts("..");
	ata_puthex((unsigned long)end);
	putkv(" bootinfo ", (unsigned long)bi);
	ata_puts("\n");
	if (bi == 0)
		ata_halt("entry: no boot record after the image");
}

/* ------------------------------------------------------------ hardware */

/* Every MFP source off; vectors at 64, software end of interrupt. */
static void
mfp_quiet()
{
	MFP(MFP_IERA) = 0;
	MFP(MFP_IERB) = 0;
	MFP(MFP_IPRA) = 0;
	MFP(MFP_IPRB) = 0;
	MFP(MFP_ISRA) = 0;
	MFP(MFP_ISRB) = 0;
	MFP(MFP_TACR) = 0;
	MFP(MFP_TBCR) = 0;
	MFP(MFP_TCDCR) = 0;
	MFP(MFP_VR) = MFP_VBASE | 0x08;
	MFP(MFP_DDR) = 0;
	MFP(MFP_AER) &= ~0x10;		/* GPIP4 (ACIA IRQ): falling edge */
	(void)SCC_CTLA;			/* register pointer back to 0 */
	(void)MFP(MFP_IPRA);		/* SCC recovery time */
	SCC_CTLA = 9;
	(void)MFP(MFP_IPRA);
	SCC_CTLA = 0xC0;		/* hardware reset: interrupts off */
}

static void
ata_vectors()
{
	register int i;

	for (i = 0; i < 16; i++)
		M68Kvec[MFP_VBASE + i] = (unsigned long)ata_mfpstray;
	M68Kvec[MFP_VBASE + MFP_TIMERA] = (unsigned long)ata_clkint;
	M68Kvec[MFP_VBASE + MFP_GPIP4] = (unsigned long)ata_aciaint;
	M68Kvec[11] = (unsigned long)ata_fline;
}

/*
 * First platform hook, from stext: MMU off, IPL 7, BSS not yet cleared.
 * Picks the RAM region, silences every source, routes the MFP vectors.
 */
void
config(arg0, arg1)
unsigned long arg0, arg1;
{
	register int i;
	unsigned long k;

	boot_arg0 = arg0;
	boot_arg1 = arg1;
	kernel_load_address = (int)stext;
	chipmem = 0;

	if (arg0 != ATA_BI_MAGIC)
		ata_halt("config: unknown boot method");

	mfp_quiet();
	ata_vectors();
	ikbd_init();

	putkv("config: mach ", ata_machtype);
	putkv(" cpu ", ata_cputype);
	putkv(" fpu ", ata_fputype);
	putkv(" mmu ", ata_mmutype);
	putkv(" _MCH ", ata_mch);
	putkv(" monitor ", (unsigned long)(MONTYPE >> 6));
	ata_puts("\n");
	if (ata_machtype != MACH_ATARI)
		ata_halt("config: boot record is not for an Atari");

	k = (unsigned long)end;
	MAINSTORE = VSIZOFMEM = 0;
	for (i = 0; i < ata_nchunk; i++) {
		putkv("config: memchunk ", ata_chunk[i][0]);
		putkv(" size ", ata_chunk[i][1]);
		if (k >= ata_chunk[i][0] && k < ata_chunk[i][0] + ata_chunk[i][1]) {
			MAINSTORE = ata_chunk[i][0];
			VSIZOFMEM = ata_chunk[i][1];
			ata_puts(" (kernel)");
		} else
			ata_puts(" (ignored)");
		ata_puts("\n");
	}
	if (VSIZOFMEM == 0)
		ata_halt("config: no memory chunk holds the kernel");
	if (MAINSTORE == 0 && VSIZOFMEM == ata_pool + POOLSIZE)
		VSIZOFMEM = ata_pool;
	if (ata_rootarg >= 0) {
		(void)ata_rd_config(0);
		rootdev = DD_BMAJ << 18 | ata_rootarg;
		dumpdev = DD_BMAJ << 18 | 2 << 4 | (ata_rootarg & 7);
		ata_dskname(swapfile + SWAP_NAME, dumpdev & 0xFF);
	} else if (!ata_rd_config(1))
		ata_halt("config: no RAM-disk root image");
	putkv("config: MAINSTORE ", MAINSTORE);
	putkv(" VSIZOFMEM ", VSIZOFMEM);
	putkv(" end ", k);
	putkv(" pool ", ata_pool);
	ata_puts("\n");
	putkv("config: rootdev ", rootdev);
	putkv(" dumpdev ", dumpdev);
	ata_puts(" swap ");
	ata_puts(swapfile + SWAP_NAME);
	ata_puts("\n");
}

/* "/dev/dsk/cNd0sM" for dd minor m */
static void
ata_dskname(d, m)
register char *d;
long m;
{
	register char *s;

	for (s = "/dev/dsk/c"; *s; )
		*d++ = *s++;
	*d++ = '0' + (m & 7);
	*d++ = 'd';
	*d++ = '0';
	*d++ = 's';
	*d++ = '0' + (m >> 4 & 7);
	*d = 0;
}

/* ------------------------------------------------------------- clock */

#define TA_COUNT	160
#define TA_PERIOD_US	4167		/* TA_COUNT counts at 38.4 kHz */

unsigned long ata_dlyms = 1000;		/* delayus loops per ms */

/*
 * Count MFP reads during one timer A period.  Each loop reads the
 * counter once, as delayus reads the MFP once per loop.
 */
static void
ata_dlycal()
{
	register unsigned long n, done, lim;
	register int v, last;

	lim = 5000000;
	last = MFP(MFP_TADR);
	for (n = 0; n < lim && MFP(MFP_TADR) == last; n++)
		;
	last = MFP(MFP_TADR);
	done = 0;
	for (n = 0; n < lim && done < TA_COUNT; n++) {
		v = MFP(MFP_TADR);
		if (v != last) {
			done += last > v ? last - v : last + TA_COUNT - v;
			last = v;
		}
	}
	if (n < lim && n >= TA_PERIOD_US / 1000)
		ata_dlyms = n * 1000 / TA_PERIOD_US;
	putkv("clock: delayus loops/ms ", ata_dlyms);
	ata_puts("\n");
}

/* MFP timer A: 2.4576 MHz / 64 / 160 = 240 Hz */
void
hw_clkstart()
{
	MFP(MFP_TACR) = 0;
	MFP(MFP_TADR) = TA_COUNT;
	MFP(MFP_TACR) = 5;
	ata_dlycal();
	MFP(MFP_IERA) |= 0x20;
	MFP(MFP_IMRA) |= 0x20;
}

void
clkreld()
{
	MFP(MFP_IERA) &= ~0x20;
	MFP(MFP_TACR) = 0;
}

void
delayus(n)
int n;
{
	register unsigned long k;

	if (n <= 0)
		return;
	k = (unsigned long)n / 1000 * ata_dlyms +
	    ((unsigned long)n % 1000 * ata_dlyms + 999) / 1000;
	while (k-- > 0)
		(void)MFP(MFP_IPRA);
}

/* ---------------------------------------------------------------- FPU */

extern int fpu_present;
extern void __amix_fpu_setup();

/*
 * sendsig resets the FPU state without asking whether there is one.
 * The emulator's fpu_setup handles the emulated case and chains here.
 */
void
fpu_setup_fpe_orig()
{
	if (fpu_present)
		__amix_fpu_setup();
}

/* ---------------------------------------------------------- backtrace */

#define UTS_RELEASE	514
#define UTS_VERSION	771
#define KSTK_LO		0x40000000	/* u-area and kernel stack */
#define KSTK_HI		0x40010000

extern char utsname[], etext[];
extern int printf();
static int bt_count = -1;

/*
 * Panic backtrace: the call before each return address on the kernel
 * stack's frame chain.  Stops at a return address outside the kernel
 * text: the outermost frame has none.
 */
void
backtrace()
{
	register unsigned long *fp, ret;
	register unsigned short *pc;
	unsigned long to;

	if (++bt_count != 0)
		return;
	printf("%s %s Backtrace:\n", utsname + UTS_RELEASE, utsname + UTS_VERSION);
	for (fp = (unsigned long *)__builtin_frame_address(0);
	    (unsigned long)fp >= KSTK_LO && (unsigned long)fp < KSTK_HI;
	    fp = (unsigned long *)(fp[0] > (unsigned long)fp ? fp[0] : 0)) {
		printf("%x: ", (unsigned)fp);
		ret = fp[1];
		if (ret < (unsigned long)stext + 6 || ret > (unsigned long)etext) {
			printf("%x->???\n", (unsigned)ret);
			break;
		}
		pc = (unsigned short *)(ret - 6);
		if (pc[0] == 0x4EB9)				/* jsr abs.l */
			to = *(unsigned long *)(pc + 1);
		else if (pc[0] == 0x61FF)			/* bsr.l */
			to = (unsigned long)(pc + 1) + *(long *)(pc + 1);
		else if (pc[1] == 0x4EBA || pc[1] == 0x6100)	/* jsr/bsr d16 */
			to = (unsigned long)(pc + 2) + (short)pc[2];
		else if ((pc[2] & 0xFF00) == 0x6100)		/* bsr.s */
			to = (unsigned long)(pc + 3) +
			    (((pc[2] & 0xFF) ^ 0x80) - 0x80);
		else {
			printf((pc[2] & 0xFFF0) == 0x4E90 ? "%x->indir\n" :
			    "%x->???\n", (unsigned)(pc + 2));
			continue;
		}
		printf("%x->%x\n", (unsigned)pc, (unsigned)to);
	}
	printf("\n");
}

/*
 * The kernel address of the first n bytes of the machine's ROM, 0 if
 * beyond it.  DTT0 maps it one to one.  A CT60's flash is 1 MB, a
 * Falcon's TOS 512 KB.
 */
unsigned long
ata_romva(n)
unsigned long n;
{
	if (ata_machtype != MACH_ATARI || n > (ata_cputype == 8 ? 0x100000 : 0x80000))
		return 0;
	return 0xe00000;
}

/* ------------------------------------------------------ halt, monitor */

void
callrom()
{
	ata_puts("callrom: no ROM monitor\n");
}

void
sysdump()
{
}

void
haltsys(how)
int how;
{
	if (how == 0)
		ata_stop("The system is halted; you may turn off power.\n");
	else if (how == 1)
		ata_stop("The system is halted.\n");
	ata_stop("");
}

void
rtnfirm()
{
	haltsys(1);
}

void
ata_halt(msg)
char *msg;
{
	ata_puts("\natari: ");
	ata_puts(msg);
	ata_puts("\n");
	haltsys(2);
}

/* ------------------------------------------------ Amiga-only entries */

/*
 * io_init's only row: start keyboard interrupts.  GPIP4 is edge
 * triggered, so drain a byte that arrived while it was off.
 */
void
parinit()
{
	MFP(MFP_IERB) |= 0x40;
	MFP(MFP_IMRB) |= 0x40;
	ikbd_intr();
}

void qlintr() { }
void slpoll() { }

/* Zorro probe: none on an Atari */
int autocon() { return 0; }

/* ------------------------------------------------------------- uname */

#define UTS_MACHINE	0x404		/* utsname.machine: 4 * SYS_NMLN */

extern char utsname[];
extern int __amix_inituname();

int
inituname()
{
	register char *d, *s;
	int r;

	r = __amix_inituname();
	d = utsname + UTS_MACHINE;
	for (s = "Atari Falcon030"; (*d++ = *s++) != 0; )
		;
	return r;
}
