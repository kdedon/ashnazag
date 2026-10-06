/*
 * Macintosh platform layer: boot record, polled SCC console, VIA1 tick,
 * root and swap selection, halt, and the platform entry points the
 * generic kernel imports.
 *
 * config() runs with the MMU off and before BSS is cleared, so every
 * variable here is initialised (lands in .data).
 */

#define VOL	__volatile__

#define MAC_BI_MAGIC	0x4D616342	/* 'MacB': d0 from the entry shim */
#define AUX_SMR_MAGIC	0x536D7201	/* d0 from A/UX Startup's launch */

/* bootinfo record tags */
#define BI_LAST		0x0000
#define BI_MACHTYPE	0x0001
#define BI_CPUTYPE	0x0002
#define BI_FPUTYPE	0x0003
#define BI_MMUTYPE	0x0004
#define BI_MEMCHUNK	0x0005
#define BI_RAMDISK	0x0006
#define BI_COMMAND_LINE	0x0007
#define BI_MAC_MODEL	0x8000
#define BI_MAC_VADDR	0x8001
#define BI_MAC_VDEPTH	0x8002
#define BI_MAC_VROW	0x8003
#define BI_MAC_VDIM	0x8004
#define BI_MAC_SCCBASE	0x8006
#define BI_MAC_MEMSIZE	0x8009
#define BI_MAC_CPUID	0x800a
#define BI_MAC_ROMBASE	0x800b
#define BI_MAC_VIA1BASE	0x8010
#define BI_MAC_AUXINFO	0x8f00	/* ours: A/UX info block, d0, low memory */

#define MACH_MAC	3
#define MAXCHUNK	4
#define MAC_MAXRAM	0x8000000	/* kvsegmap holds 512 segments of 256 KB */
#define BISIZE		1024

#define VIA1_BASE	0x50F00000
#define VIA2_BASE	0x50F02000
#define SCC_BASE	0x50F0C020

/* 6522 registers are 0x200 apart */
#define VIA_ORB		0x0000
#define VIA_DDRB	0x0400
#define VIA_T1CL	0x0800
#define VIA_T1CH	0x0A00
#define VIA_ACR		0x1600
#define VIA_IFR		0x1A00
#define VIA_IER		0x1C00

/* 783.36 kHz / 60 Hz, less the 2-cycle reload */
#define VIA_TICK	13054

#define via1(r)	(*(VOL unsigned char *)(VIA1_BASE + (r)))
#define via2(r)	(*(VOL unsigned char *)(VIA2_BASE + (r)))

extern char end[], edata[], etext[];
extern char stext[];
extern unsigned long boot_arg0, boot_arg1;
extern unsigned long MAINSTORE, VSIZOFMEM, chipmem;
extern int kernel_load_address;
extern int clock_int(), addupc_clk();

/* boot record copied by the shim; pstart builds tables where it lay */
unsigned char mac_bi[BISIZE] = { 0 };
unsigned long mac_bilen = 1;

unsigned long mac_scc = SCC_BASE;
int mac_scc_ready = 1;

unsigned long mac_machtype = 1, mac_cputype = 1, mac_fputype = 1;
unsigned long mac_mmutype = 1, mac_model = 1, mac_memsize = 1;
unsigned long mac_vaddr = 1, mac_vdepth = 1, mac_vrow = 1, mac_vdim = 1;
unsigned long mac_rombase = 1, mac_via1 = 1;
unsigned long mac_auxinfo[3] = { 1 };
unsigned long mac_chunk[MAXCHUNK][2] = { { 1, 1 } };
int mac_nchunk = 1;

unsigned long mac_spurious[8] = { 1 };
unsigned long mac_ticks = 1;

void mac_puts();
void mac_puthex();
void mac_halt();

/* ---------------------------------------------------------------- SCC */

#define scc_ctl	(*(VOL unsigned char *)(mac_scc + 2))	/* channel A */
#define scc_dat	(*(VOL unsigned char *)(mac_scc + 6))
#define scc_ctlb (*(VOL unsigned char *)(mac_scc + 0))

static void
scc_delay()
{
	register int i;

	for (i = 0; i < 8; i++)
		(void)via1(VIA_IFR);
}

static void
scc_wr(reg, val)
int reg, val;
{
	scc_ctl = reg;
	scc_delay();
	scc_ctl = val;
	scc_delay();
}

/* 9600 8N1 on the modem port, no interrupts */
static unsigned char scc_init_tab[] = {
	9, 0xC0,	/* hardware reset */
	4, 0x44,	/* x16 clock, 1 stop, no parity */
	3, 0xC0,
	5, 0x60,
	9, 0x00,
	10, 0x00,
	11, 0x50,	/* Rx/Tx clock = BRG */
	12, 0x0A,	/* 3.6864 MHz / (32 * 9600) - 2 */
	13, 0x00,
	14, 0x01,	/* BRG on, from RTxC */
	3, 0xC1,	/* Rx enable */
	5, 0xEA,	/* DTR, Tx 8 bits, Tx enable, RTS */
	15, 0x00,
	1, 0x00,
	0, 0x10,
	0, 0x10,
};

void
mac_scc_init()
{
	register int i;

	for (i = 0; i < sizeof scc_init_tab; i += 2)
		scc_wr(scc_init_tab[i], scc_init_tab[i+1]);
	mac_scc_ready = 1;
}

/*
 * At least IPL 4 around the ready test and the data write, so the
 * interrupt-driven SCC driver cannot load the transmitter in between.
 * Never lowers the level (config runs at IPL 7).
 */
static int
scc_iplraise()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	if ((s & 0x700) < 0x400)
		__asm__ __volatile__("mov.w %0,%%sr" : :
		    "d" ((s & ~0x700) | 0x400) : "memory");
	return s;
}

static void
scc_putc(c)
int c;
{
	register long n;
	int s;

	s = scc_iplraise();
	for (n = 0; n < 100000; n++)
		if (scc_ctl & 0x04)
			break;
	scc_dat = c;
	scc_delay();
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}

extern void fbcons_kputc(), fbcons_report();
extern int fbcons_biinit();

/* SCC first, then the screen (the SCC still works if the screen faults) */
void
putchar(c)
unsigned char c;
{
	if (c == '\n')
		scc_putc('\r');
	scc_putc(c);
	fbcons_kputc(c);
}

unsigned char
getchar()
{
	unsigned char c;

	while ((scc_ctl & 0x01) == 0)
		;
	c = scc_dat;
	scc_delay();
	return c;
}

void
mac_puts(s)
char *s;
{
	while (*s)
		putchar(*s++);
}

void
mac_puthex(v)
unsigned long v;
{
	register int i;

	mac_puts("0x");
	for (i = 28; i >= 0; i -= 4)
		putchar("0123456789abcdef"[(v >> i) & 0xF]);
}

static void
putkv(k, v)
char *k;
unsigned long v;
{
	mac_puts(k);
	mac_puthex(v);
}

/* ---------------------------------------------------------- boot record */

/*
 * Called by the shim, MMU off, before stext.  Copies the boot record
 * (it lies at the image end, where pstart builds page tables).
 */
void
mac_shim_main(bi)
unsigned short *bi;
{
	register unsigned char *s, *d;
	register unsigned long n;
	unsigned short tag, size;

	(void)fbcons_biinit((unsigned char *)bi);	/* no-op if aux_entry did it */
	mac_puts("\nAMIX/Mac entry shim: image ");
	mac_puthex((unsigned long)stext);
	mac_puts("..");
	mac_puthex((unsigned long)end);
	putkv(" bootinfo ", (unsigned long)bi);
	mac_puts("\n");

	s = (unsigned char *)bi;
	for (n = 0; n + 4 <= BISIZE; n += size) {
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
	d = mac_bi;
	for (mac_bilen = n; n; n--)
		*d++ = *s++;
}

/* ------------------------------------------------------- root and swap */

/*
 * Root, swap and dump device; the first that applies wins:
 *   root=cNd0sM on the command line: SCSI target N, slice M (dd, block
 *     major 18); swap and dump on slice 2 of the same disk;
 *   a RAM-disk root image (boot-record BI_RAMDISK or linked in): rd0
 *     (block major 20, s5); swap and dump on rd1;
 *   otherwise halt.  A disk is never root without root=: A/UX Startup's
 *   choice (-e/-p or its root disk) and c0d0s1 may be A/UX's own
 *   file systems, so they are only reported.
 * Swap size and root file-system type are probed before the root mount.
 */
#define DD_BMAJ		18
#define DEVNO(maj, min)	((unsigned long)(maj) << 18 | (min))
#define SWAP_NAME	16	/* offset of bo_name in struct bootobj */

extern unsigned long rootdev, dumpdev;
extern char swapfile[];
extern int mac_rd_config(), mac_diskpick();
extern void mac_dskname();

long mac_rootarg = -1;		/* dd minor from root=, -1 none */
long mac_socktrace = 1;		/* socktrace: log A/UX socket calls */
extern long mac_nofpu;

/* w as a word of the boot command line */
static int
mac_word(s, w)
char *s, *w;
{
	register char *p;
	register int i;

	for (p = s; *p; p++) {
		if (p != s && p[-1] != ' ')
			continue;
		for (i = 0; w[i] && p[i] == w[i]; i++)
			;
		if (w[i] == 0 && (p[i] == 0 || p[i] == ' '))
			return 1;
	}
	return 0;
}

/* nofpu as a word of the boot command line: probe no FPU */
static void
mac_fpuparse(s)
char *s;
{
	register char *p;

	for (p = s; *p; p++)
		if ((p == s || p[-1] == ' ') && p[0] == 'n' && p[1] == 'o'
		&& p[2] == 'f' && p[3] == 'p' && p[4] == 'u'
		&& (p[5] == 0 || p[5] == ' '))
			mac_nofpu = 1;
}

/*
 * root=cNd0sM as a word of the boot command line.  Slice 0 (whole disk)
 * and 2 (swap) would overlap the swap slice, so they are refused.
 */
static void
mac_rootparse(s)
char *s;
{
	register char *p;

	for (p = s; *p; p++) {
		if ((p != s && p[-1] != ' ') || p[0] != 'r' || p[1] != 'o'
		|| p[2] != 'o' || p[3] != 't' || p[4] != '=')
			continue;
		if (p[5] == 'c' && p[6] >= '0' && p[6] <= '6'
		&& p[7] == 'd' && p[8] == '0' && p[9] == 's'
		&& p[10] >= '1' && p[10] <= '7' && p[10] != '2'
		&& (p[11] == 0 || p[11] == ' ')) {
			mac_rootarg = (p[10] - '0') << 4 | (p[6] - '0');
			return;
		}
		mac_puts("  root=: not cNd0sM (N 0-6, M 1 or 3-7), ignored\n");
	}
}

static void
mac_root()
{
	static char *why[] = { " (default)", " (A/UX Startup root disk)",
	    " (A/UX Startup -e/-p)", " (root=)" };
	long rm, sm;
	int how;

	how = mac_diskpick((unsigned char *)mac_auxinfo[0],
	    (unsigned char *)stext, &rm, &sm);
	if (mac_rootarg >= 0) {
		rm = mac_rootarg;
		sm = 2 << 4 | (mac_rootarg & 7);
		how = 3;
	}
	if (mac_rd_config(how != 3))
		how = -1;
	else if (how != 3) {
		putkv("config: disk root candidate (dd minor) ", rm);
		mac_puts(why[how]);
		mac_halt("config: no RAM-disk root image; boot with root=cNd0sM");
		return;
	} else {
		rootdev = DEVNO(DD_BMAJ, rm);
		dumpdev = DEVNO(DD_BMAJ, sm);
		mac_dskname(swapfile + SWAP_NAME, sm);
	}
	putkv("config: rootdev ", rootdev);
	putkv(" dumpdev ", dumpdev);
	mac_puts(" swap ");
	mac_puts(swapfile + SWAP_NAME);
	mac_puts(how < 0 ? " (RAM disk)" : why[how]);
	mac_puts("\n");
}

static void
bi_parse()
{
	register unsigned long n;
	register unsigned long *p;
	unsigned short tag, size;

	mac_nchunk = 0;
	mac_machtype = mac_cputype = mac_fputype = mac_mmutype = 0;
	mac_model = mac_memsize = mac_vaddr = mac_vdepth = 0;
	mac_vrow = mac_vdim = mac_rombase = mac_via1 = 0;
	mac_auxinfo[0] = mac_auxinfo[1] = mac_auxinfo[2] = 0;
	mac_socktrace = 0;

	for (n = 0; n + 4 <= mac_bilen; n += size) {
		tag = *(unsigned short *)(mac_bi + n);
		size = *(unsigned short *)(mac_bi + n + 2);
		if (tag == BI_LAST || size < 4 || n + size > mac_bilen)
			break;
		p = (unsigned long *)(mac_bi + n + 4);
		switch (tag) {
		case BI_MACHTYPE:	mac_machtype = p[0]; break;
		case BI_CPUTYPE:	mac_cputype = p[0]; break;
		case BI_FPUTYPE:	mac_fputype = p[0]; break;
		case BI_MMUTYPE:	mac_mmutype = p[0]; break;
		case BI_MAC_MODEL:	mac_model = p[0]; break;
		case BI_MAC_MEMSIZE:	mac_memsize = p[0]; break;
		case BI_MAC_VADDR:	mac_vaddr = p[0]; break;
		case BI_MAC_VDEPTH:	mac_vdepth = p[0]; break;
		case BI_MAC_VROW:	mac_vrow = p[0]; break;
		case BI_MAC_VDIM:	mac_vdim = p[0]; break;
		case BI_MAC_ROMBASE:	mac_rombase = p[0]; break;
		case BI_MAC_VIA1BASE:	mac_via1 = p[0]; break;
		case BI_MAC_SCCBASE:	/* any mirror: fold into the mapped slice */
			mac_scc = VIA1_BASE | (p[0] & 0x3FFFF); break;
		case BI_MAC_AUXINFO:
			mac_auxinfo[0] = p[0];
			mac_auxinfo[1] = p[1];
			mac_auxinfo[2] = p[2];
			break;
		case BI_MEMCHUNK:
			if (mac_nchunk < MAXCHUNK) {
				mac_chunk[mac_nchunk][0] = p[0];
				mac_chunk[mac_nchunk][1] = p[1];
				mac_nchunk++;
			}
			break;
		case BI_COMMAND_LINE:
			mac_puts("  cmdline \"");
			mac_puts((char *)p);
			mac_puts("\"\n");
			mac_rootparse((char *)p);
			mac_fpuparse((char *)p);
			mac_socktrace = mac_word((char *)p, "socktrace");
#ifdef BOOTDIAG
			diag_parse((char *)p);
#endif
			break;
		}
	}
}

static void
via_quiet()
{
	register int b;

	via1(VIA_IER) = 0x7F;
	via1(VIA_IFR) = 0x7F;
	via2(VIA_IER) = 0x7F;
	via2(VIA_IFR) = 0x7F;
	/*
	 * VIA1 PB6 high selects the Mac OS interrupt levels (VIA1 at 1);
	 * low moves VIA1 to level 6.  If the ADB state lines PB4/5 are
	 * still inputs, no ROM has run: latch them as 0, so the ADB
	 * driver's first IDLE is a state change.  Otherwise keep them.
	 */
	b = via1(VIA_ORB);
	if ((via1(VIA_DDRB) & 0x30) != 0x30)
		b &= ~0x30;
	via1(VIA_ORB) = b | 0x40;
	via1(VIA_DDRB) |= 0x40;
}

/*
 * First platform hook, from stext: MMU off, IPL 7, BSS not yet cleared.
 * Picks the RAM chunk holding the kernel and silences every source.
 */
config(arg0, arg1)
unsigned long arg0, arg1;
{
	register int i;
	unsigned long k;

	boot_arg0 = arg0;
	boot_arg1 = arg1;
	kernel_load_address = (int)stext;
	chipmem = 0;

	if (arg0 == AUX_SMR_MAGIC)
		mac_halt("config: A/UX hand-off did not pass through aux_entry");
	if (arg0 != MAC_BI_MAGIC)
		mac_halt("config: unknown boot method");

	bi_parse();
	via_quiet();
	scc_ctlb = 1; scc_delay(); scc_ctlb = 0;	/* ch B: WR1 = 0 */
	scc_wr(1, 0);
	scc_wr(9, 0);					/* MIE off */

	putkv("config: mach ", mac_machtype);
	putkv(" cpu ", mac_cputype);
	putkv(" fpu ", mac_fputype);
	putkv(" mmu ", mac_mmutype);
	putkv(" model ", mac_model);
	putkv(" memsize(MB) ", mac_memsize);
	mac_puts("\n");
	putkv("config: video ", mac_vaddr);
	putkv(" depth ", mac_vdepth);
	putkv(" row ", mac_vrow);
	putkv(" dim ", mac_vdim);
	putkv(" scc ", mac_scc);
	putkv(" via1 ", mac_via1);
	mac_puts("\n");
	fbcons_report();
	if (mac_auxinfo[0]) {
		putkv("config: A/UX Startup info ", mac_auxinfo[0]);
		putkv(" d0 ", mac_auxinfo[1]);
		putkv(" lowmem ", mac_auxinfo[2]);
		mac_puts("\n");
	}

	if (mac_machtype != MACH_MAC)
		mac_halt("config: boot record is not for a Macintosh");

	k = (unsigned long)end;
	MAINSTORE = VSIZOFMEM = 0;
	for (i = 0; i < mac_nchunk; i++) {
		putkv("config: memchunk ", mac_chunk[i][0]);
		putkv(" size ", mac_chunk[i][1]);
		if (k >= mac_chunk[i][0] && k < mac_chunk[i][0] + mac_chunk[i][1]) {
			MAINSTORE = mac_chunk[i][0];
			VSIZOFMEM = mac_chunk[i][1];
			mac_puts(" (kernel)");
		} else
			mac_puts(" (ignored)");
		mac_puts("\n");
	}
	if (VSIZOFMEM == 0)
		mac_halt("config: no memory chunk holds the kernel");
	if (MAINSTORE + VSIZOFMEM > MAC_MAXRAM) {
		VSIZOFMEM = MAC_MAXRAM - MAINSTORE;
		mac_puts("config: memory limited to 128 MB\n");
	}
	mac_root();
	putkv("config: MAINSTORE ", MAINSTORE);
	putkv(" VSIZOFMEM ", VSIZOFMEM);
	putkv(" end ", k);
	mac_puts("\n");
}

/* ------------------------------------------------------------- clock */

hw_clkstart()
{
	via1(VIA_IER) = 0x40;
	via1(VIA_ACR) = (via1(VIA_ACR) & 0x3F) | 0x40;
	via1(VIA_T1CL) = VIA_TICK & 0xFF;
	via1(VIA_T1CH) = VIA_TICK >> 8;
	via1(VIA_IFR) = 0x40;
	via1(VIA_IER) = 0xC0;
}

clkreld()
{
	via1(VIA_IER) = 0x40;
	via1(VIA_ACR) &= 0x3F;
}

/* ------------------------------------------------------------- uname */

#define UTS_MACHINE	0x404		/* utsname.machine: 4 * SYS_NMLN */

extern char utsname[], buildid[];
extern int inituname_orig();

/* machine: "mac68k" plus the build tag " 68040-YYMMDD-NN" */
int
inituname()
{
	register char *d, *s;
	int r;

	r = inituname_orig();
	d = utsname + UTS_MACHINE;
	for (s = "mac68k"; *s; )
		*d++ = *s++;
	for (s = buildid; (*d++ = *s++) != 0; )
		;
	return r;
}

/* ------------------------------------------------------ halt, monitor */

callrom()
{
	mac_puts("callrom: no ROM monitor\n");
}

sysdump()
{
}

/* Never returns: MMU and caches off, message, spin. */
extern void mac_stop();
extern void (*fbcons_panicfn)();

void
haltsys(how)
int how;
{
	void (*f)();

	/*
	 * The console takes the screen from a session in front, as for a
	 * panic: its text otherwise goes to a shadow in mapped memory,
	 * which mac_stop cannot reach with the MMU off.
	 */
	if ((f = fbcons_panicfn) != 0) {
		fbcons_panicfn = 0;
		(*f)();
	}
	if (how == 0)
		mac_stop("The system is halted; you may turn off power.\n");
	else if (how == 1)
		mac_stop("The system is halted.\n");
	mac_stop("");
}

void
rtnfirm()
{
	haltsys(1);
}

void
mac_halt(msg)
char *msg;
{
	mac_puts("\nmac: ");
	mac_puts(msg);
	mac_puts("\n");
	haltsys(2);
}

/* ------------------------------------------------ Amiga-only entries */

/* io_init/io_poll rows of the stock master.d tables */
extern void adb_init();

/* io_init's only row: ADB keyboard and mouse; bounded */
void
parinit()
{
	adb_init();
}

void qlintr() { }
void slpoll() { }

/* Zorro probe: a Mac has no Zorro boards */
autocon() { return 0; }

/* One VIA access takes 1.0-1.5 us (E-clock synchronised): never short. */
delayus(n)
int n;
{
	while (n-- > 0)
		(void)via1(VIA_IFR);
}

/* ------------------------------------------------ static MMU mappings */

extern unsigned long kptr040;
extern unsigned long mac_rd_tc(), mac_rd_itt0(), mac_rd_dtt0();
extern unsigned long mac_rd_dtt1(), mac_rd_srp();

#define PG_IO	0xC1	/* supervisor, noncacheable serialized, resident */
#define PG_ROM	0x85	/* supervisor, write-through, write-protected */

struct iomap {
	unsigned long va, pa, nblk, bits;	/* nblk in 256 KB page tables */
};

struct iomap mac_iomap[] = {
	{ 0x50F00000, 0x50F00000, 4, PG_IO },
	{ 0x52800000, 0x40800000, 4, PG_ROM },
	{ 0x5FFC0000, 0x5FFC0000, 1, PG_IO },
	{ 0, 0, 0, 0 },
};

/* ROMBase's kernel address for n bytes of ROM, 0 if not mapped */
unsigned long
mac_romva(n)
unsigned long n;
{
	return mac_rombase == 0x40800000 && n <= 0x100000 ? 0x52800000 : 0;
}

/* 9 page tables of 64 entries, 256-byte aligned */
unsigned long mac_iopt[10 * 64] = { 1 };

/*
 * Cached window onto the first RAM_WIN bytes of RAM, VA = PA | RAM_VA.
 * The page array lives here (see patch_kvmpages.py): the identity map
 * (DTT0) is noncacheable.  Nothing else reaches these addresses through
 * a cacheable mapping, so the two views do not alias in the cache.
 */
#define RAM_VA	0x60000000
#define RAM_WIN	0x1000000
unsigned long mac_rampt[(RAM_WIN >> 18) * 64 + 64] = { 1 };
extern unsigned long hat_cm_ram;

/* Called by pstart with kptr040 set, before translation is enabled. */
void
mac_iomap_build()
{
	register unsigned long *pt, *kp;
	register struct iomap *m;
	register unsigned long b, i, va, pa;

	pt = (unsigned long *)(((unsigned long)mac_iopt + 255) & ~255);
	kp = (unsigned long *)kptr040;
	for (m = mac_iomap; m->nblk; m++)
		for (b = 0; b < m->nblk; b++) {
			va = m->va + (b << 18);
			pa = m->pa + (b << 18);
			for (i = 0; i < 64; i++)
				pt[i] = (pa + (i << 12)) | m->bits;
			kp[(va - 0x40000000) >> 18] = (unsigned long)pt | 2;
			pt += 64;
		}

	pt = (unsigned long *)(((unsigned long)mac_rampt + 255) & ~255);
	for (pa = 0; pa < RAM_WIN; pa += 1 << 18) {
		for (i = 0; i < 64; i++)
			pt[i] = (pa + (i << 12)) | 0x99 | hat_cm_ram;
		kp[(RAM_VA + pa - 0x40000000) >> 18] = (unsigned long)pt | 2;
		pt += 64;
	}
}

/* Called by pstart after mlsetup: the page array must fit the window. */
extern char *page_hash;
extern int page_hashsz;

void
mac_ramwin_check()
{
	unsigned long e;

	e = (unsigned long)(page_hash + page_hashsz * sizeof (char *));
	if (page_hash < (char *)RAM_VA || e > RAM_VA + RAM_WIN)
		mac_halt("kvm: page array outside the cached RAM window");
}

/* First output through the mapped I/O page. */
void
mac_mmu_report()
{
	putkv("pstart: MMU on, tc ", mac_rd_tc());
	putkv(" itt0 ", mac_rd_itt0());
	putkv(" dtt0 ", mac_rd_dtt0());
	putkv(" dtt1 ", mac_rd_dtt1());
	putkv(" srp ", mac_rd_srp());
	mac_puts("\n");
	putkv("pstart: kptr040 ", kptr040);
	putkv(" VIA1 IER (mapped) ", (unsigned long)via1(VIA_IER));
	mac_puts("\n");
}
