/*
 * The CyberStorm SCSI driver against a model chip that runs its SCRIPTS
 * and a model target.  Built without PIE so every static has a 32-bit
 * address to stand in for a physical one.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static unsigned char regs[0x60] __attribute__((aligned(4))), p5[0x40];
#define NCRBASE ((ulong)regs)
#define P5BASE ((ulong)p5)
#define autocon fake_autocon
#define vtop fake_vtop
#define ide_cpush fake_cpush
#define drv_usecwait fake_wait
#define printf fake_printf
#define R(n) (*fake_reg((n) ^ 3))
#define splhi fake_splhi
#define splset fake_splset
static int fake_splhi() { return 0x2000; }
static void fake_splset(int s) { }

/* reading DSTAT or SIST clears it and its ISTAT bit */
static volatile unsigned char *
fake_reg(int n)
{
	static unsigned char v;

	if (n != 0x0F && n != 0x40 && n != 0x41)
		return &regs[n];
	v = regs[n];
	regs[n] = 0;
	if (n == 0x0F)
		regs[0x17] &= ~1;
	else if (!regs[0x40] && !regs[0x41])
		regs[0x17] &= ~2;
	return &v;
}
#include "../csppc/csppc.c"
#undef printf

/* the target: one command's phases, in order */
enum { DOUT, DIN, CMD, STAT, MOUT = 6, MIN };
static struct { int ph, n; uchar *buf; } tgt[8];
static int nt, present, cur, curoff, atn, chiperr;
static uint halted;
static int cpushes, prints;
static uchar disk[4096], cdbseen[12], ident;

int fake_autocon(uint pn, int c, char **a, char **o) { return pn == 0x21400064 && c == 0; }
ulong fake_vtop(char *p, int q) { return (uint)(ulong)p; }
void fake_cpush(char *a, uint n) { cpushes++; }
int fake_printf(f, a, b, c, d, e, g, h) char *f; long a, b, c, d, e, g, h;
{ prints++; if (getenv("V")) printf(f, a, b, c, d, e, g, h); return 0; }

static uint w(uint a) { return *(uint *)(ulong)a; }

/* SIST/DSTAT the way the driver reads them */
static void
stop(int sist, int dstat, uint dsps, uint dsp)
{
	regs[SIST0 ^ 3] = sist;
	regs[SIST1 ^ 3] = sist >> 8;
	regs[DSTAT ^ 3] = dstat;
	regs[ISTAT ^ 3] = (sist ? ISTAT_SIP : 0) | (dstat ? ISTAT_DIP : 0);
	R32(DSPS) = dsps;
	R32(DSP) = halted = 0xFFFFFFF0;	/* any write restarts */
}

static int phase() { return cur < nt ? tgt[cur].ph : -1; }

/* move n bytes between the target's current phase and memory at a */
static int
xfer(int ph, uint a, int n)
{
	int k;

	if (phase() != ph)
		return -1;
	k = tgt[cur].n - curoff;
	if (k > n)
		k = n;
	if (ph == DIN || ph == STAT || ph == MIN)
		memcpy((char *)(ulong)a, tgt[cur].buf + curoff, k);
	else
		memcpy(tgt[cur].buf + curoff, (char *)(ulong)a, k);
	curoff += k;
	regs[0x0B ^ 3] = tgt[cur].buf[0];	/* SFBR */
	if (curoff == tgt[cur].n) {
		cur++;
		curoff = 0;
	}
	return k == n ? 0 : 1;
}

static void
run_scripts()
{
	uint pc = R32(DSP), dsa = R32(DSA), i0, i1;
	int steps;

	regs[ISTAT ^ 3] = 0;
	for (steps = 0; steps < 100; steps++) {
		i0 = w(pc);
		i1 = w(pc + 4);
		pc += 8;
		if ((i0 >> 30) == 0) {		/* table indirect block move */
			int ph = i0 >> 24 & 7, r;
			uint n = w(dsa + i1) & 0xFFFFFF, a = w(dsa + i1 + 4);
			if (ph == MOUT)
				ident = *(uchar *)(ulong)a;
			if (ph == CMD)
				memcpy(cdbseen, (char *)(ulong)a, n);
			if ((r = xfer(ph, a, n)) != 0) {
				stop(SIST0_MA, 0, 0, pc);
				return;
			}
		} else if ((i0 & 0xFF000000) == 0x47000000) {
			uint d = w(dsa + (i0 & 0xFFFFFF));
			if ((d >> 24) != CLOCK || (d & 0xFFFF)) {
				chiperr++;
				return;
			}
			if (!present) {
				stop(SIST1_STO << 8, 0, 0, pc);
				return;
			}
			atn = 1;
			cur = curoff = 0;
		} else if ((i0 & 0xFF000000) == 0x60000000) {
			if (i0 & 8)
				atn = 0;
		} else if (i0 == 0x7C027F00 || i0 == 0x48000000) {
		} else if ((i0 & 0xFF000000) == 0x98000000) {
			stop(0, DSTAT_SIR, i1, pc);
			return;
		} else if ((i0 & 0xF8000000) == 0x80000000) {
			int t;
			if (i0 & 0x00040000)		/* compare data */
				t = regs[0x0B ^ 3] == (i0 & 0xFF);
			else if (i0 & 0x00020000)	/* compare phase */
				t = phase() == (i0 >> 24 & 7);
			else
				t = 1;
			if (!(i0 & 0x00080000))
				t = !t;
			if (t)
				pc += i1;
		} else {
			chiperr++;
			return;
		}
	}
	chiperr++;
}

void
fake_wait(int us)
{
	if (R32(DSP) != halted && !(regs[0x17] & 3))
		run_scripts();
}

static int done;
static void intr(struct sdcom *cp) { done++; }

static uchar st0[1] = { 0 }, st2[1] = { 2 }, cc[1] = { 0 }, cmdbuf[12], tdata[4096];

static int
cmd(struct sdcom *c, int unit, int op, char *buf, uint n, int reading)
{
	memset(c, 0, sizeof *c);
	c->cdb[0] = op;
	c->unit = unit;
	c->addr = buf;
	c->nbyte = n;
	c->reading = reading;
	c->intr = intr;
	done = 0;
	csppcqueue(0, c);
	return done == 1;
}

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } } while (0)

int
main()
{
	static struct sdcom c;
	static char buf[4096];
	int i;

	CHECK(csppcpresent(), "present");
	CHECK(p5[P5IRQ * 8] == (P5SET | P5_SCSIIRQOFF), "board interrupt off");
	CHECK(p5[P5RESET * 8] == (P5SET | P5_SCSIRESET), "SCSI out of reset");
	CHECK(regs[SCID ^ 3] == HOSTID && regs[SCNTL3 ^ 3] == CLOCK, "chip init");
	/* the lane flip against the big-endian register map */
	CHECK((ISTAT ^ 3) == 0x17 && (DSTAT ^ 3) == 0x0F && (SIST1 ^ 3) == 0x40 &&
		(SIEN0 ^ 3) == 0x43 && (STEST3 ^ 3) == 0x4C && (DCNTL ^ 3) == 0x38 &&
		(DMODE ^ 3) == 0x3B && (STIME0 ^ 3) == 0x4B && (CTEST3 ^ 3) == 0x18, "register map");

	present = 1;
	for (i = 0; i < 1024; i++)
		tdata[i] = i * 7;
	nt = 0;
	tgt[nt].ph = MOUT; tgt[nt].n = 1; tgt[nt++].buf = &ident;
	tgt[nt].ph = CMD; tgt[nt].n = 10; tgt[nt++].buf = cmdbuf;
	tgt[nt].ph = DIN; tgt[nt].n = 1024; tgt[nt++].buf = tdata;
	tgt[nt].ph = STAT; tgt[nt].n = 1; tgt[nt++].buf = st0;
	tgt[nt].ph = MIN; tgt[nt].n = 1; tgt[nt++].buf = cc;
	cpushes = 0;
	CHECK(cmd(&c, 6, 0x28, buf, 1024, 1), "read completes");
	CHECK(c.okay && c.status == 0 && !memcmp(buf, tdata, 1024), "read data");
	CHECK(ident == 0x80 && cmdbuf[0] == 0x28, "identify and CDB");
	CHECK(cpushes == 2, "cache pushed before and after");

	tgt[2].ph = DOUT;
	tgt[2].n = 512;
	memset(tdata, 0, 512);
	memset(buf, 0x5A, 512);
	CHECK(cmd(&c, 6, 0x2A, buf, 512, 0) && c.okay, "write");
	CHECK(tdata[0] == 0x5A && tdata[511] == 0x5A, "write data");

	/* short read: phase mismatch, resumed at status */
	tgt[2].ph = DIN;
	tgt[2].n = 512;
	tgt[3].buf = st2;
	CHECK(cmd(&c, 6, 0x28, buf, 1024, 1) && c.okay && c.status == 2, "short read takes status");

	/* no data phase */
	tgt[1].n = 6;
	tgt[2] = tgt[3];
	tgt[3] = tgt[4];
	nt = 4;
	tgt[2].buf = st0;
	CHECK(cmd(&c, 6, 0x00, 0, 0, 0) && c.okay && c.status == 0, "test unit ready");
	CHECK(cmdbuf[0] == 0, "six-byte CDB");

	/* no target */
	present = 0;
	prints = 0;
	CHECK(cmd(&c, 3, 0x00, 0, 0, 0) && !c.okay && !prints, "selection timeout");
	present = 1;
	CHECK(cmd(&c, 6, 0x00, 0, 0, 0) && c.okay, "after timeout");

	/* target goes back to data after the transfer */
	nt = 0;
	tgt[nt].ph = MOUT; tgt[nt].n = 1; tgt[nt++].buf = &ident;
	tgt[nt].ph = CMD; tgt[nt].n = 10; tgt[nt++].buf = cmdbuf;
	tgt[nt].ph = DIN; tgt[nt].n = 1024; tgt[nt++].buf = tdata;
	tgt[nt].ph = STAT; tgt[nt].n = 1; tgt[nt++].buf = st0;
	CHECK(cmd(&c, 6, 0x28, buf, 512, 1) && !c.okay, "overrun fails");
	CHECK(!chiperr, "model chip");
	if (fails)
		return 1;
	printf("PASS csppc SCRIPTS: read, write, short read, no data, timeout, overrun\n");
	return 0;
}
