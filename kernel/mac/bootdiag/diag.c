/*
 * diag.c -- boot status lines at the bottom of the built-in screen.
 *
 * Built only with BOOTDIAG.  The console gives up its last DIAG_ROWS
 * text rows; these are redrawn every 6 clock ticks, at each boot step,
 * for the first idle entries, and from the SCSI and idle paths when the
 * clock has not advanced.  A frozen band means nothing runs any more.
 *
 * Boot options (words of the command line):
 *   nostop    idle spins with interrupts open instead of STOP
 *   scsipoll  53C96 always polled
 *   nosonic   Ethernet not attached
 *   novbl     display VBL interrupt never enabled
 *   nofpu     FPU treated as absent
 */
#include "../video/fbcons.h"

#define SR_GET(s)	__asm__ __volatile__("movew %%sr,%0" : "=d" (s) : : "memory")
#define SR_SET(s)	__asm__ __volatile__("movew %0,%%sr" : : "d" (s) : "memory")

#define DIAG_ROWS	6
#define DO_ON		0x01
#define DO_NOSTOP	0x02
#define DO_SCSIPOLL	0x04
#define DO_NOSONIC	0x08
#define DO_NOVBL	0x10
#define DO_NOFPU	0x20

/* parsed before BSS is cleared: must be initialised data */
int diag_opts = DO_ON;

extern long lbolt;
extern int ncr_intrmode;
extern long ncr_nintr, ncr_nerr, ncr_nlost, ncr_nnosel, ncr_nnodreq, ncr_nfault;
extern long ncr_nchunk, ncr_ncmd;
extern unsigned long mac_ticks, mac_spurious[8];
extern unsigned long sn_nintr, sn_nslot, ds_nhwvbl;
extern int __amix_vfs_mountroot(), __amix_swapconf(), __amix_exece();
extern int __amix_fpuinit();
extern long fpu_present;
extern char u[];

/* exception vectors (diagvec.s) */
extern unsigned long diag_vcnt[256];
extern unsigned short diag_vlast, diag_fvec;	/* vector offsets */
extern unsigned long diag_fpc;
extern void diag_vecinit();

unsigned long diag_nidle, diag_nstopret, diag_instop;
unsigned long diag_nadb, diag_nca1, diag_nlvl[8];
unsigned long diag_scmd, diag_sstat, diag_sintr, diag_sstep, diag_sstate;
unsigned long diag_sdma, diag_spoll;

static char steps[24];
static int nsteps, drawing, userseen;
static long lastdraw, pokes, execsys;

static char *
dec(p, v)
register char *p;
register unsigned long v;
{
	char b[12];
	register int n = 0;

	do
		b[n++] = '0' + v % 10;
	while ((v /= 10) != 0);
	while (n > 0)
		*p++ = b[--n];
	return p;
}

static char *
hex(p, v, n)
register char *p;
unsigned long v;
register int n;
{
	while (--n >= 0)
		*p++ = "0123456789ABCDEF"[(v >> (4 * n)) & 15];
	return p;
}

static char *
str(p, s)
register char *p, *s;
{
	while (*s)
		*p++ = *s++;
	return p;
}

/* s, n pairs: label then decimal */
static char *
kv(p, s, v)
char *p, *s;
unsigned long v;
{
	return dec(str(p, s), v);
}

/* one text row of the band, inverse video, padded to the full width */
static void
row(r, s, n)
int r, n;
char *s;
{
	register __volatile__ unsigned char *p;
	register unsigned char *g;
	register unsigned long m;
	register int y, b, k, d;
	int x, c;
	unsigned long fg, bg, rb;
	struct fbcons *fc = &fbcons;

	d = fc->fc_m.fm_depth;
	rb = fc->fc_m.fm_row;
	fg = fc->fc_bg;
	bg = fc->fc_fg;
	for (x = 0; x < fc->fc_cols; x++) {
		c = x < n ? s[x] : ' ';
		g = fb_font[c & 0xFF];
		p = (__volatile__ unsigned char *)fc->fc_m.fm_base
		    + (fc->fc_rows + r) * FB_CH * rb + x * FB_CW * d / 8;
		for (y = 0; y < FB_CH; y++, p += rb) {
			b = g[y];
			switch (d) {
			case 1:
				*p = (b & fg) | (~b & bg);
				break;
			case 2:
				m = fc->fc_exp[b >> 4][0] << 8 | fc->fc_exp[b & 15][0];
				m = (m & fg) | (~m & bg);
				p[0] = m >> 8;
				p[1] = m;
				break;
			case 4:
				m = fc->fc_exp[b >> 4][0] << 16 | fc->fc_exp[b & 15][0];
				m = (m & fg) | (~m & bg);
				p[0] = m >> 24; p[1] = m >> 16; p[2] = m >> 8; p[3] = m;
				break;
			default:
				for (k = 0; k < d / 4; k++) {
					m = fc->fc_exp[k < d / 8 ? b >> 4 : b & 15][k % (d / 8)];
					*(__volatile__ unsigned long *)(p + 4 * k) = (m & fg) | (~m & bg);
				}
				break;
			}
		}
	}
}

static unsigned long
vsum(a, b)
int a, b;
{
	unsigned long n = 0;

	for (; a <= b; a++)
		n += diag_vcnt[a];
	return n;
}

void
diag_draw()
{
	char l[256];
	register char *p;
	int s, hi;

	if (!fbcons.fc_on || fbcons.fc_cols < 80 || drawing)
		return;
	SR_GET(s);
	hi = s | 0x700;
	SR_SET(hi);
	if (drawing) {
		SR_SET(s);
		return;
	}
	drawing = 1;
	SR_SET(s);
	lastdraw = lbolt;
	pokes = 0;
	if (!userseen && execsys && diag_vcnt[32] > execsys
	    && nsteps < sizeof steps - 1) {
		userseen = 1;
		steps[nsteps++] = 'U';
	}

	p = str(l, "DIAG ");
	p = str(p, steps);
	p = kv(p, " tick:", (unsigned long)lbolt);
	p = kv(p, " idle:", diag_nidle);
	p = kv(p, " stopret:", diag_nstopret);
	p = kv(p, " instop:", diag_instop);
	p = kv(p, " fpu:", (unsigned long)fpu_present);
	p = str(p, " opt:");
	p = hex(p, (unsigned long)diag_opts, 2);
	row(0, l, p - l);

	p = kv(l, "IRQ L1:", diag_vcnt[25]);
	p = kv(p, " L2:", diag_vcnt[26]);
	p = kv(p, " L3:", diag_vcnt[27]);
	p = kv(p, " L4:", diag_vcnt[28]);
	p = kv(p, " L5:", diag_vcnt[29]);
	p = kv(p, " L6:", diag_vcnt[30]);
	p = kv(p, " L7:", diag_vcnt[31]);
	p = kv(p, " spur:", diag_vcnt[24]);
	row(1, l, p - l);

	p = kv(l, "VIA1 T1:", mac_ticks);
	p = kv(p, " ADB:", diag_nadb);
	p = kv(p, " oth:", mac_spurious[1]);
	p = kv(p, "  VIA2 SCSI:", (unsigned long)ncr_nintr);
	p = kv(p, " CA1:", diag_nca1);
	p = kv(p, " SONIC:", sn_nintr);
	p = kv(p, " VBL:", ds_nhwvbl);
	p = kv(p, " slot:", sn_nslot);
	p = kv(p, " oth:", mac_spurious[2]);
	row(2, l, p - l);

	p = str(l, "SCSI ");
	p = str(p, ncr_intrmode == 0 || diag_spoll ? "poll" : "irq ");
	p = kv(p, " cmds:", (unsigned long)ncr_ncmd);
	p = str(p, " cmd:");
	p = hex(p, diag_scmd, 2);
	p = str(p, " stat:");
	p = hex(p, diag_sstat, 2);
	p = str(p, " intr:");
	p = hex(p, diag_sintr, 2);
	p = kv(p, " step:", diag_sstep);
	p = kv(p, " phase:", diag_sstat & 7);
	p = kv(p, " st:", diag_sstate);
	p = kv(p, " dma:", diag_sdma);
	row(3, l, p - l);

	p = kv(l, "SCSI lost:", (unsigned long)ncr_nlost);
	p = kv(p, " err:", (unsigned long)ncr_nerr);
	p = kv(p, " selto:", (unsigned long)ncr_nnosel);
	p = kv(p, " nodrq:", (unsigned long)ncr_nnodreq);
	p = kv(p, " berr:", (unsigned long)ncr_nfault);
	p = kv(p, " chunks:", (unsigned long)ncr_nchunk);
	p = kv(p, "  EXC last:", (unsigned long)diag_vlast / 4);
	p = kv(p, " flt:", (unsigned long)diag_fvec / 4);
	p = str(p, "@");
	p = hex(p, diag_fpc, 8);
	row(4, l, p - l);

	p = kv(l, "EXC 2:", diag_vcnt[2]);
	p = kv(p, " 3:", diag_vcnt[3]);
	p = kv(p, " 4:", diag_vcnt[4]);
	p = kv(p, " 5-9:", vsum(5, 9));
	p = kv(p, " 10:", diag_vcnt[10]);
	p = kv(p, " 11:", diag_vcnt[11]);
	p = kv(p, " 14:", diag_vcnt[14]);
	p = kv(p, " sys:", diag_vcnt[32]);
	p = kv(p, " trap:", vsum(33, 47));
	p = kv(p, " fp:", vsum(48, 55));
	p = kv(p, " mmu:", vsum(56, 63));
	p = kv(p, " hi:", vsum(64, 255));
	row(5, l, p - l);
	drawing = 0;
}

/* clock tick, after clock_int */
void
diag_tick()
{
	if (lbolt - lastdraw >= 6)
		diag_draw();
}

/* SCSI and idle paths: redraw only when the clock looks stuck */
void
diag_poke()
{
	if (lbolt == lastdraw && ++pokes >= 2000)
		diag_draw();
}

void
diag_step(c)
int c;
{
	if (nsteps < sizeof steps - 2)
		steps[nsteps++] = c;
	diag_draw();
}

/* called from idle before STOP */
void
diag_idle()
{
	if (diag_nidle <= 16)
		diag_draw();
	else
		diag_poke();
}

/* nostop: a short spin with interrupts open */
void
diag_spin()
{
	register int i;

	for (i = 0; i < 2000; i++)
		;
	diag_poke();
}

/* scsi: registers of the last chip interrupt */
void
diag_scsi(stat, step, intr, state)
int stat, step, intr, state;
{
	diag_sstat = stat & 0xFF;
	diag_sstep = step;
	diag_sintr = intr & 0xFF;
	diag_sstate = state;
}

static int
word(s, w)
register char *s, *w;
{
	register char *p, *q;

	for (; *s; s++) {
		if (*s == ' ')
			continue;
		for (p = s, q = w; *q && *p == *q; p++, q++)
			;
		if (*q == 0 && (*p == 0 || *p == ' '))
			return 1;
		while (*s && *s != ' ')
			s++;
		if (*s == 0)
			break;
	}
	return 0;
}

/* boot command line, from config before BSS is cleared */
void
diag_parse(s)
char *s;
{
	if (word(s, "nostop"))
		diag_opts |= DO_NOSTOP;
	if (word(s, "scsipoll")) {
		diag_opts |= DO_SCSIPOLL;
		ncr_intrmode = 0;
	}
	if (word(s, "nosonic"))
		diag_opts |= DO_NOSONIC;
	if (word(s, "novbl")) {
		diag_opts |= DO_NOVBL;
		/* DAFB: VBL interrupt off and cleared (MMU still off here) */
		*(__volatile__ unsigned long *)0xF9800104 = 0;
		*(__volatile__ unsigned long *)0xF980010C = 0;
	}
	if (word(s, "nofpu"))
		diag_opts |= DO_NOFPU;
}

int
vfs_mountroot(a, b, c, d)
long a, b, c, d;
{
	int r;

	diag_vecinit();
	diag_step('M');
	r = __amix_vfs_mountroot(a, b, c, d);
	diag_step(r ? '!' : 'm');
	return r;
}

int
swapconf(a, b, c, d)
long a, b, c, d;
{
	int r;

	diag_step('S');
	r = __amix_swapconf(a, b, c, d);
	diag_step('s');
	return r;
}

int
exece(a, b, c, d)
long a, b, c, d;
{
	int r;
	static int n;

	if (n++ == 0)
		diag_step('E');
	r = __amix_exece(a, b, c, d);
	if (n == 1) {
		execsys = diag_vcnt[32] ? diag_vcnt[32] : 1;
		diag_step(r ? 'x' : 'X');
	}
	return r;
}

/* nofpu: undo detection; the FPU is left in its reset state */
int
fpuinit(a, b, c, d)
long a, b, c, d;
{
	int r;

	r = __amix_fpuinit(a, b, c, d);
	if (diag_opts & DO_NOFPU) {
		fpu_present = 0;
		*(long *)(u + 0x10c) = 0;
	}
	return r;
}
