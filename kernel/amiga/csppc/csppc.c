/*
 * SCSI on the CyberStorm PPC and MK III: an NCR 53C770 at a fixed address,
 * in big-endian mode, so its register numbers are the databook's with the
 * byte lane flipped (n ^ 3).  One command at a time, polled, no
 * disconnection, narrow asynchronous transfers; targets 0-7.
 */
#include "sys/types.h"
#include "rico.h"
#include "sd.h"
#include "csppc_script.h"

#define P5PROD		0x21400064	/* Phase 5, CyberStorm MK III and PPC */
#ifndef NCRBASE
#define NCRBASE		0xF40000
#define P5BASE		0xF60000
#endif
#define NCR		((__volatile__ uchar *)NCRBASE)
#define P5REG(n)	(((__volatile__ uchar *)P5BASE)[(n) * 8])
#define P5RESET		0
#define P5IRQ		1
#define P5SET		0x80
#define P5_SCSIRESET	0x01		/* 1: out of reset */
#define P5_SCSIIRQOFF	0x02

#ifndef R
#define R(n)		(NCR[(n) ^ 3])
#endif
#define R32(n)		(*(__volatile__ uint *)(NCR + (n)))

#define SCNTL0		0x00
#define SCNTL1		0x01
#define SCNTL3		0x03
#define SCID		0x04
#define SXFER		0x05
#define DSTAT		0x0C
#define DSA		0x10
#define ISTAT		0x14
#define CTEST3		0x1B
#define DSP		0x2C
#define DSPS		0x30
#define DMODE		0x38
#define DIEN		0x39
#define DWT		0x3A
#define DCNTL		0x3B
#define SIEN0		0x40
#define SIEN1		0x41
#define SIST0		0x42
#define SIST1		0x43
#define STIME0		0x48
#define RESPID0		0x4A
#define STEST1		0x4D
#define STEST3		0x4F

#define ISTAT_ABRT	0x80
#define ISTAT_SRST	0x40
#define ISTAT_SIP	0x02
#define ISTAT_DIP	0x01
#define DSTAT_SIR	0x04
#define SIST0_MA	0x80
#define SIST0_RST	0x02
#define SIST1_STO	0x04
#define SCNTL1_RST	0x08
#define CTEST3_CLF	0x04
#define STEST1_DBLEN	0x08
#define STEST1_DBLSEL	0x04
#define STEST3_HSC	0x20
#define STEST3_CSF	0x02

#define HOSTID		7
#define CLOCK		0x15		/* SCNTL3: 50 MHz doubled */
#define POLLS		3000000		/* x 10 us */

extern int autocon();
extern int printf();
extern ulong vtop();
extern void ide_cpush(), drv_usecwait();

/* what the script reads through DSA */
static struct dsa {
	uint	dev;
	uint	msgoutn, msgout;
	uint	cmdn, cmd;
	uint	datan, data;
	uint	statn, stat;
	uint	msginn, msgin;
	uchar	cdb[12];
	uchar	ident[4];
	uchar	in[16];		/* status byte, message byte */
} ds;

static int found;
static uchar scntl3;
static struct sdcom *head, *tail;
static int csppcbusy;

/* the table's layout is the script's */
#define OFF(f)	((int)&((struct dsa *)0)->f)
typedef char dscheck[OFF(msgoutn) == A_ds_MsgOut && OFF(cmdn) == A_ds_Cmd &&
	OFF(datan) == A_ds_Data && OFF(statn) == A_ds_Status &&
	OFF(msginn) == A_ds_MsgIn ? 1 : -1];

static uint
pa(p)
char *p;
{
	return vtop(p, 0);
}

/* wait for the chip to stop; 0 on timeout, else ISTAT, with *d DSTAT and *s SIST1:SIST0 */
static int
poll(d, s)
int *d, *s;
{
	int i, st;

	for (i = 0; i < POLLS; i++) {
		st = R(ISTAT);
		if (st & (ISTAT_SIP | ISTAT_DIP)) {
			*s = 0;
			*d = 0;
			if (st & ISTAT_SIP) {
				*s = R(SIST0);
				*s |= R(SIST1) << 8;
			}
			if (st & ISTAT_DIP)
				*d = R(DSTAT);
			return st;
		}
		drv_usecwait(10);
	}
	return 0;
}

static void
clear()
{
	int n;

	for (n = 0; n < 4 && (R(ISTAT) & (ISTAT_SIP | ISTAT_DIP)); n++) {
		R(SIST0);
		R(DSTAT);
		R(SIST1);
	}
}

static void
chipinit()
{
	R(ISTAT) = ISTAT_SRST;
	drv_usecwait(10);
	R(ISTAT) = 0;
	R(STEST1) = STEST1_DBLEN;
	drv_usecwait(20);
	R(STEST3) = STEST3_HSC;
	R(SCNTL3) = CLOCK;
	R(STEST1) = STEST1_DBLEN | STEST1_DBLSEL;
	R(STEST3) = 0;
	scntl3 = R(SCNTL3);
	R(SCNTL0) = 0xC0;		/* full arbitration */
	R(DCNTL) = 0x20;
	R(DMODE) = 0xC0;
	R(SCID) = HOSTID;
	R(RESPID0) = 1 << HOSTID;
	R(SXFER) = 0;
	R(DWT) = 0;
	R(STIME0) = 0x0C;		/* selection timeout 204.8 ms */
	R(SIEN0) = 0x8F;		/* phase mismatch, gross error, disconnect, reset, parity */
	R(SIEN1) = SIST1_STO;
	R(DIEN) = 0x75;			/* parity, bus fault, abort, INT, illegal instruction */
	R(SCNTL1) = SCNTL1_RST;
	drv_usecwait(50);
	R(SCNTL1) = 0;
	drv_usecwait(250000);
	clear();
}

static void
recover()
{
	int d, s;

	R(ISTAT) = ISTAT_ABRT;
	if (poll(&d, &s))
		R(ISTAT) = 0;
	chipinit();
}

static void
flush()
{
	R(CTEST3) |= CTEST3_CLF;
	R(STEST3) |= STEST3_CSF;
	drv_usecwait(1);
	R(CTEST3) &= ~CTEST3_CLF;
	R(STEST3) &= ~STEST3_CSF;
}

/* one command; nonzero when the target returned a status byte */
static int
run(cp)
struct sdcom *cp;
{
	int i, st, d, s, lost;

	for (i = 0; i < sizeof ds.cdb; i++)
		ds.cdb[i] = cp->cdb[i];
	ds.ident[0] = 0x80;
	ds.dev = scntl3 << 24 | cp->unit << 16;
	ds.msgoutn = 1;
	ds.msgout = pa(ds.ident);
	ds.cmdn = (cp->cdb[0] >> 5) == 0 ? 6 : (cp->cdb[0] >> 5) == 5 ? 12 : 10;
	ds.cmd = pa(ds.cdb);
	ds.datan = cp->nbyte;
	ds.data = (uint)cp->addr;
	ds.statn = 1;
	ds.stat = pa(&ds.in[0]);
	ds.msginn = 1;
	ds.msgin = pa(&ds.in[1]);
	ds.in[0] = 0xFF;
	ide_cpush(cp->addr, cp->nbyte);
	R32(DSA) = pa(&ds);
	R32(DSP) = pa(csppc_script) + Ent_start;
	for (lost = 0;;) {
		st = poll(&d, &s);
		if (st == 0) {
			printf("csppc: unit %d: no answer\n", cp->unit);
			recover();
			return 0;
		}
		if ((d & DSTAT_SIR) && R32(DSPS) == A_ok) {
			ide_cpush(cp->addr, cp->nbyte);
			cp->status = ds.in[0];
			return 1;
		}
		if ((d & DSTAT_SIR) && R32(DSPS) == A_lost && ++lost < 4) {
			R32(DSP) = pa(csppc_script) + Ent_start;
			continue;
		}
		if (s & SIST0_MA && !(d & ~DSTAT_SIR)) {
			/* short transfer: drop the residue and take status */
			flush();
			R32(DSP) = pa(csppc_script) + Ent_afterdata;
			continue;
		}
		if (s & SIST1_STO << 8)
			flush();
		else {
			printf("csppc: unit %d: ISTAT %x DSTAT %x SIST %x DSPS %x\n",
				cp->unit, st, d, s, R32(DSPS));
			recover();
		}
		ide_cpush(cp->addr, cp->nbyte);
		return 0;
	}
}

/*
 * Commands queued from a completion callback run in the outer call's
 * loop rather than nesting.
 */
#ifndef splhi
static int
splhi()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s));
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s;
}

static void
splset(s)
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}
#endif

/* the chip polls at level 2 so a stray SCSI interrupt waits for csppcintr */
bool
csppcqueue(c, cp)
struct sdcom *cp;
{
	int s, s2;

	s = splhi();
	cp->okay = FALSE;
	cp->next = 0;
	if (tail)
		tail->next = cp;
	else
		head = cp;
	tail = cp;
	if (csppcbusy) {
		splset(s);
		return TRUE;
	}
	csppcbusy = 1;
	s2 = (s & 0x700) < 0x200 ? (s & ~0x700) | 0x200 : s;
	while (cp = head) {
		if (!(head = cp->next))
			tail = 0;
		splset(s2);
		if (run(cp))
			cp->okay = TRUE;
		(*cp->intr)(cp);
		splhi();
	}
	csppcbusy = 0;
	splset(s);
	return TRUE;
}

/* nothing is in flight here; clear what an interrupt left */
void
csppcintr()
{
	if (found && !csppcbusy)
		clear();
}

int
csppcpresent()
{
	char *a, *o;

	if (!autocon(P5PROD, 0, &a, &o))
		return 0;
	if (!found) {
		found = 1;
		P5REG(P5IRQ) = P5SET | P5_SCSIIRQOFF;
		P5REG(P5RESET) = P5SET | P5_SCSIRESET;
		chipinit();
		printf("csppc: 53C770 rev %d\n", R(CTEST3) >> 4);
	}
	return 1;
}
