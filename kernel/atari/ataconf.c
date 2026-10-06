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
#ifdef ATA060
#define BI_ATARI_OSBASE	0x8f00		/* ours: physical start of the TOS image */
#define BI_ATARI_SVIDEL	0x8f01		/* ours: SuperVidel native mode */
#define SV_CTRL		0x80010000
#define SV_MODE		0x40		/* SV_CTRL: the SuperVidel registers set the mode */
#define SV_RESET	0x3		/* SV_CTRL: hold VGA and DVI output in reset */
#define SV_REG(o)	IO32(SV_CTRL + (o))
#define SV_VERSION	0x8001007C	/* low 10 bits: firmware */
#define SV_RAM		0xA1000000	/* graphics DDR; below it mirrors ST-RAM */
#define SV_RAMEND	0xA8000000
#define FASTRAM		0x01000000
#define ATA_MAXRAM	0x8000000	/* kvsegmap holds 512 segments of 256 KB */
#define RAM_VA		0x60000000
#define RAM_WIN		0x1000000
#define CM_CB		0x20		/* page descriptor cache modes */
#define CM_NCS		0x40
#define PTE_SUP		0x99		/* resident, used, modified, supervisor */
#endif

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
#define MFP_GPIP7	15
#define SCC_CTLA	IO8(0xFFFF8C81)

extern char end[], stext[];
extern unsigned long boot_arg0, boot_arg1;
extern unsigned long MAINSTORE, VSIZOFMEM, chipmem;
extern int kernel_load_address;
extern unsigned long M68Kvec[];
extern void ata_clkint(), ata_aciaint(), ata_sndint(), ata_mfpstray(), ata_fline();
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
#ifdef ATA060
unsigned long ata_osbase = 1;
unsigned long ata_sv[6] = { 1 };	/* firmware, screen, width, height, depth, row */
int ata_svfb = 1;			/* the console is on the SuperVidel */
static int ata_nosv = 1;
static int ata_svhw = 1;		/* the loader found the card */
static unsigned long ata_svmode[3] = { 1 };	/* sv=: width, height, depth */
extern long cputype;
extern unsigned long hat_cm_ram;
extern void ata_isp61();
#endif

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

#ifdef ATA060
static void
ata_putdec(v)
unsigned long v;
{
	char b[11];
	register int i = 0;

	do
		b[i++] = '0' + v % 10;
	while ((v /= 10) != 0);
	while (i)
		putchar(b[--i]);
}

/*
 * 60 Hz timings; horizontal values in pixels.  Each line and frame
 * starts with the sync pulse, then the back porch.  pll is clock chip
 * PLL2 bytes 0x27-0x2A (Y5 divider, N, R, Q), pll2 byte 0x2B (Q, P, VCO
 * range), for a 16 MHz reference.
 */
static struct svtim {
	unsigned short st_w, st_h, st_hs, st_hbp, st_ht, st_vs, st_vbp, st_vt;
	unsigned long st_pos;		/* 1 hsync, 2 vsync active high */
	unsigned long st_pll, st_pll2;
} ata_svtim[] = {
	{ 640, 480, 96, 48, 800, 2, 33, 525, 0, 0x04C382BB, 0x28 },	/* 25.175 MHz */
	{ 800, 600, 128, 88, 1056, 4, 23, 628, 3, 0x0400A002, 0x86 },	/* 40 */
	{ 1024, 768, 136, 160, 1344, 6, 29, 806, 0, 0x02082022, 0x05 },	/* 65 */
	{ 1280, 1024, 112, 248, 1688, 3, 38, 1066, 3, 0x0101B003, 0x68 },	/* 108 */
	{ 1920, 1080, 32, 400, 2400, 5, 23, 1112, 1, 0x0100A002, 0x86 },	/* 160 */
};
#define NSVTIM	(sizeof ata_svtim / sizeof ata_svtim[0])

/*
 * Sets a mode on both outputs, screen at SV_RAM.  Any Videl write takes
 * the outputs back to the Videl, so a Videl VGA mode goes first; the
 * clock changes while the outputs are in reset.  Horizontal registers
 * count pairs of pixels.  1 if the SuperVidel took it.
 */
static int
ata_svset(t, d)
register struct svtim *t;
unsigned long d;
{
	register unsigned long hb, vb, row;
	register int o;

	row = t->st_w * d / 8;
	hb = t->st_hs + t->st_hbp;
	vb = t->st_vs + t->st_vbp;
	videl_set(ata_screen, MON_VGA, &ata_vmodes[0]);
	if (d == 8) {
		/* 8 planes, bit 12 chunky: the Videl mode under 8-bit chunky */
		IO16(0xFFFF8210) = 0x140;
		IO16(0xFFFF8266) = 0x1010;
	}
	IO32(SV_CTRL) |= SV_RESET;
	SV_REG(0x0C) = t->st_pll;
	SV_REG(0x10) = t->st_pll2;	/* starts the clock chip update */
	for (o = 0x34; o >= 0x14; o -= 0x20) {	/* DVI, then VGA */
		SV_REG(o + 0x04) = hb / 2 << 16 | (hb + t->st_w) / 2;
		SV_REG(o + 0x08) = (t->st_pos & 1) << 31 | t->st_hs / 2;
		SV_REG(o + 0x0C) = vb << 16 | (vb + t->st_h);
		SV_REG(o + 0x10) = (t->st_pos & 2) << 30 | t->st_vs;
		SV_REG(o + 0x14) = (t->st_ht - 1) / 2 << 16 | (t->st_vt - 1);
		SV_REG(o + 0x18) = row << 16 | (row + 15);
		SV_REG(o + 0x1C) = d == 8 ? 4 : d == 16 ? 5 : 7;	/* chunky, RGB565, xRGB */
	}
	IO32(SV_CTRL) &= ~SV_RESET;
	if (!(IO32(SV_CTRL) & SV_MODE))
		return 0;
	SV_REG(0x14) = SV_RAM;
	SV_REG(0x34) = SV_RAM;
	return 1;
}

/* An sv= the card can't show: say so; 0, the Videl console */
static int
ata_svbad()
{
	if (ata_svmode[0])
		ata_puts("video: sv= not a SuperVidel mode, Videl console\n");
	return 0;
}

/* sv=WxHxD: one of ata_svtim at depth 8, 16 or 32; else the geometry of the TOS mode */
static void
ata_svparse(s)
register char *s;
{
	register char *p;
	register int i;
	unsigned long v[3];

	for (p = s; *p; p++) {
		if ((p != s && p[-1] != ' ') || p[0] != 's' || p[1] != 'v' || p[2] != '=')
			continue;
		p += 3;
		for (i = 0; i < 3; i++, p++) {
			for (v[i] = 0; *p >= '0' && *p <= '9'; p++)
				v[i] = v[i] * 10 + *p - '0';
			if (v[i] == 0 || *p != (i < 2 ? 'x' : *p == ' ' ? ' ' : 0)) {
				ata_svmode[0] = 1;	/* malformed: matches no mode */
				return;
			}
		}
		for (i = 0; i < 3; i++)
			ata_svmode[i] = v[i];
		return;
	}
}

/*
 * Console in SuperVidel RAM: the sv= mode if the table has it, else the
 * native mode the SuperVidel's XBIOS left; otherwise the Videl keeps it.
 * 8 bpp is chunky through the Falcon palette, 16 RGB565, 32 xRGB.  1 if
 * the console is there.
 */
static int
ata_svidel()
{
	struct fbmode m;
	register unsigned long *v = ata_sv;
	register struct svtim *t;
	register unsigned long d;

	if (ata_nosv || !ata_svhw || (IO32(SV_VERSION) & 0x3FF) != v[0])
		return 0;
	d = ata_svmode[2];
	for (t = ata_svtim; t < ata_svtim + NSVTIM; t++)
		if (t->st_w == ata_svmode[0] && t->st_h == ata_svmode[1] &&
		    (d == 8 || d == 16 || d == 32))
			break;
	if (t == ata_svtim + NSVTIM) {
		/* the Videl keeps the console unless TOS left the card in a mode */
		t = 0;
		if (v[1] == 0 || !(IO32(SV_CTRL) & SV_MODE))
			return ata_svbad();
	}
	if (t) {
		if (!ata_svset(t, d))
			return 0;
		v[1] = SV_RAM;
		v[2] = t->st_w;
		v[3] = t->st_h;
		v[4] = d;
		v[5] = t->st_w * d / 8;
	} else if (ata_svmode[0]) {
		v[2] = ata_svmode[0];
		v[3] = ata_svmode[1];
		v[4] = ata_svmode[2];
		v[5] = v[2] * v[4] / 8;
	}
	if ((v[4] != 8 && v[4] != 16 && v[4] != 32) || v[1] < SV_RAM ||
	    v[1] >= SV_RAMEND || v[3] == 0 || v[5] < v[2] * v[4] / 8 ||
	    v[5] > (SV_RAMEND - v[1]) / v[3])
		return ata_svbad();
	if (v[4] == 8) {
		FAL_PAL(0) = 0xFFFF00FF;
		FAL_PAL(255) = 0;
	}
	m.fm_base = v[1];
	m.fm_row = v[5];
	m.fm_depth = v[4];
	m.fm_width = v[2];
	m.fm_height = v[3];
	if (!fbcons_attach(&m))
		return 0;
	ata_svfb = 1;
	ata_puts("video: SuperVidel firmware ");
	ata_putdec(v[0]);
	ata_puts(t ? ", set " : ", ");
	ata_putdec(v[2]);
	putchar('x');
	ata_putdec(v[3]);
	putchar('x');
	ata_putdec(v[4]);
	putkv(" at ", v[1]);
	ata_puts("\n");
	return 1;
}
#endif

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
#ifdef ATA060
	if (ata_svidel())
		return;
#endif
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
#ifdef ATA060
	ata_osbase = 0;
	ata_sv[0] = ata_sv[1] = 0;
	ata_svfb = ata_nosv = ata_svhw = 0;
	ata_svmode[0] = 0;
#endif
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
#ifdef ATA060
		case BI_ATARI_OSBASE:	ata_osbase = p[0]; break;
		case BI_ATARI_SVIDEL:
			if (size >= 4 + sizeof ata_sv) {
				register int i;

				for (i = 0; i < 6; i++)
					ata_sv[i] = p[i];
				ata_svhw = 1;
			}
			break;
#endif
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
#ifdef ATA060
			ata_nosv = ata_word((char *)p, "nosv");
			ata_svparse((char *)p);
#endif
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
	M68Kvec[MFP_VBASE + MFP_GPIP7] = (unsigned long)ata_sndint;
#ifndef ATA060
	M68Kvec[11] = (unsigned long)ata_fline;
#else
	M68Kvec[61] = (unsigned long)ata_isp61;
#endif
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
#ifdef ATA060
	if (ata_cputype != 4 && ata_cputype != 8)
		ata_halt("config: this kernel needs a 68040 or 68060");
	cputype = ata_cputype == 8 ? 60 : 40;
#endif

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
#ifdef ATA060
	/* VM in FastRAM: ST-RAM is device memory */
	for (i = 0; i < ata_nchunk; i++)
		if (MAINSTORE && ata_chunk[i][0] == 0)
			chipmem = ata_chunk[i][1];
	/* VM in ST-RAM: uncached like the rest of ST-RAM, so no alias differs */
	if (MAINSTORE < FASTRAM)
		hat_cm_ram = CM_NCS;
	if (MAINSTORE + VSIZOFMEM > ATA_MAXRAM) {
		VSIZOFMEM = ATA_MAXRAM - MAINSTORE;
		ata_puts("config: memory limited to 128 MB\n");
	}
	if (ata_osbase < MAINSTORE + VSIZOFMEM && ata_osbase + 0x100000 > MAINSTORE)
		ata_osbase = 0;
#endif
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
#ifdef ATA060
	putkv(" chipmem ", chipmem);
	putkv(" TOS ", ata_osbase);
	putkv(" cm ", hat_cm_ram);
#endif
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

#ifndef ATA060
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
#endif

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
 * Falcon's TOS 512 KB.  A CT60 runs TOS from a copy in FastRAM; the
 * loader passes where that copy is.
 */
unsigned long
ata_romva(n)
unsigned long n;
{
	if (ata_machtype != MACH_ATARI || n > (ata_cputype == 8 ? 0x100000 : 0x80000))
		return 0;
#ifdef ATA060
	if (ata_osbase)
		return ata_osbase;
#endif
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
#ifdef ATA060
	extern unsigned long ata_isp61_n;

	if (ata_isp61_n)
		putkv("isp: 64-bit multiplies emulated in the kernel: ", ata_isp61_n);
#endif
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
#ifdef ATA060
	for (s = "Atari Falcon"; (*d++ = *s++) != 0; )
#else
	for (s = "Atari Falcon030"; (*d++ = *s++) != 0; )
#endif
		;
	return r;
}

#ifdef ATA060
/* ------------------------------------------------- FastRAM, RAM window */

#define ATA_LOAD	0x1000		/* link address of the image */
#define RELTAB_MAGIC	0x52544142	/* 'RTAB' */

extern char edata[];

/*
 * Called by the shim before ata_shim_main.  The VM region is the RAM
 * chunk holding the kernel, so with FastRAM present the image moves
 * there: copy it and the boot record, clear BSS, add the distance at
 * every site in the relocation table.  Returns the distance, 0 to stay.
 *
 * The table ends at edata: its bytes, padding to a long, their count,
 * 'RTAB'.  Per site, the gap from the previous one in words, one byte
 * (1-127) or three (0x80 | high 7 bits, then 16 bits); 0 ends it.
 */
unsigned long
ata_reloc(bi)
unsigned char *bi;
{
	register unsigned char *s, *d, *t;
	register unsigned long n, o, delta;
	unsigned long base, size, e, *p;
	unsigned short tag, sz;

	p = (unsigned long *)edata;
	if (bi == 0 || (unsigned long)end >= FASTRAM || p[-1] != RELTAB_MAGIC)
		return 0;
	t = (unsigned char *)(p - 2) - ((p[-2] + 3) & ~3UL);
	base = size = 0;
	for (n = 0; ; n += sz) {
		if (n + 4 > BISIZE)
			return 0;
		tag = *(unsigned short *)(bi + n);
		sz = *(unsigned short *)(bi + n + 2);
		if (tag == BI_LAST) {
			n += 4;
			break;
		}
		if (sz < 4 || (sz & 1))
			return 0;
		p = (unsigned long *)(bi + n + 4);
		if (tag == BI_MEMCHUNK && sz >= 12 && p[0] >= FASTRAM
		&& p[1] > size) {
			base = p[0];
			size = p[1];
		}
	}
	delta = base;
	e = (((unsigned long)end + 1) & ~1UL) + delta;
	if (size < 0x400000 || (base & 0xfff) || e + n + 0x100000 > base + size)
		return 0;
	for (s = (unsigned char *)ATA_LOAD, d = s + delta; s < (unsigned char *)edata; )
		*d++ = *s++;
	while (d < (unsigned char *)e)
		*d++ = 0;
	for (s = bi; n; n--)
		*d++ = *s++;
	d = (unsigned char *)ATA_LOAD + delta;
	for (o = 0; (n = *t++) != 0; ) {
		if (n & 0x80) {
			n = (n & 0x7f) << 16 | t[0] << 8 | t[1];
			t += 2;
		}
		o += n << 1;
		*(unsigned long *)(d + o) += delta;
	}
	return delta;
}

/* 64 page tables of 64 entries, 256-byte aligned */
unsigned long ata_rampt[(RAM_WIN >> 18) * 64 + 64] = { 1 };
extern unsigned long kptr040, kroot040, kptbl;
extern char ata_mmu_buf[];
extern void ata_idcm(), bzero();

/* FastRAM identity map: leaf tables for [ata_idlo, ata_idhi) at ata_idpt */
unsigned long *ata_idpt = (unsigned long *)1;
unsigned long ata_idlo = 1, ata_idhi = 1;
static unsigned long ata_idptr = 1, ata_idleaf = 1;

/* Leaf entry for supervisor address a, adding tables from the pool. */
static unsigned long *
ata_idpte(a)
unsigned long a;
{
	register unsigned long *d;

	d = (unsigned long *)kroot040 + (a >> 25);
	if (!(*d & 2)) {
		*d = ata_idptr | 0x0a;
		ata_idptr += 512;
	}
	d = (unsigned long *)(*d & ~0x1ffUL) + (a >> 18 & 0x7f);
	if (!(*d & 2)) {
		*d = ata_idleaf | 0x0a;
		ata_idleaf += 256;
	}
	return (unsigned long *)(*d & ~0xffUL) + (a >> 12 & 0x3f);
}

static void
ata_idnc(a, e)
register unsigned long a, e;
{
	register unsigned long *pte;

	for (; a < e; a += 0x1000) {
		pte = ata_idpte(a);
		*pte = (*pte & ~0x60UL) | CM_NCS;
	}
}

/*
 * Supervisor page tables for FastRAM below 1 GB, built in the pool at p
 * by pstart with the MMU off; returns the end of the pool.  The VM
 * region is copyback, like every other mapping of its pages; the pool,
 * the boot tables and the TOS copy are noncacheable.  Used and modified
 * are preset, so the table walk never writes these entries.  ST-RAM is
 * left to DTT0.
 */
unsigned long
ata_idmap_build(p)
unsigned long p;
{
	register unsigned long a;
	unsigned long os, n;

	p = (p + 0xfff) & ~0xfffUL;
	ata_idpt = 0;
	if (MAINSTORE < FASTRAM)
		return p;
	ata_idlo = MAINSTORE & ~0x3ffffUL;
	ata_idhi = (MAINSTORE + VSIZOFMEM + 0x3ffff) & ~0x3ffffUL;
	os = ata_osbase >= FASTRAM && ata_osbase < 0x3ff00000 ?
	    ata_osbase & ~0xfffUL : 0;
	/* one page of pointer tables, then the leaf tables */
	n = 0x1000 + ((ata_idhi - ata_idlo) >> 10) + (os ? 0x500 : 0);
	bzero(p, n);
	ata_idptr = p;
	ata_idleaf = p + 0x1000;
	ata_idpt = (unsigned long *)ata_idleaf;
	for (a = ata_idlo; a < ata_idhi; a += 0x1000)
		*ata_idpte(a) = a >= MAINSTORE && a < MAINSTORE + VSIZOFMEM ?
		    a | PTE_SUP | CM_CB : 0;
	for (a = os; a && a < os + 0x100000; a += 0x1000)
		*ata_idpte(a) = a | PTE_SUP | CM_NCS;
	a = (ata_idleaf + 0xfff) & ~0xfffUL;
	ata_idnc(p, a);
	ata_idnc((unsigned long)ata_mmu_buf, (unsigned long)ata_mmu_buf + 0x8000);
	return a;
}

/*
 * Window onto the first RAM_WIN bytes of the VM region, VA = PA |
 * RAM_VA, for the page array.  With FastRAM it shares the identity
 * leaf tables, so both see the same cache mode.  Called by pstart with
 * kptr040 set, MMU off.
 */
void
ata_iomap_build()
{
	register unsigned long *pt, *kp;
	register unsigned long i, pa;

	pt = (unsigned long *)(((unsigned long)ata_rampt + 255) & ~255);
	kp = (unsigned long *)kptr040;
	for (pa = MAINSTORE; pa < MAINSTORE + RAM_WIN; pa += 1 << 18) {
		if (ata_idpt) {
			if (pa < ata_idhi)
				kp[((RAM_VA | pa) - 0x40000000) >> 18] =
				    (unsigned long)(ata_idpt + ((pa - ata_idlo) >> 12)) | 2;
			continue;
		}
		for (i = 0; i < 64; i++)
			pt[i] = (pa + (i << 12)) | PTE_SUP | hat_cm_ram;
		kp[((RAM_VA | pa) - 0x40000000) >> 18] = (unsigned long)pt | 2;
		pt += 64;
	}
}

extern char *page_hash;
extern int page_hashsz;

/* Called by pstart after mlsetup: the page array must fit the window. */
void
ata_ramwin_check()
{
	unsigned long e;

	e = (unsigned long)(page_hash + page_hashsz * sizeof (char *));
	if (page_hash < (char *)(RAM_VA | MAINSTORE) || e > (RAM_VA | MAINSTORE) + RAM_WIN)
		ata_halt("kvm: page array outside the cached RAM window");
	ata_idcm(kptbl, CM_NCS);		/* kernel leaf tables */
}
#endif
