/* host checks: 68030/68040 access fault frames decoded, finished, built */
#include <stdio.h>
#include <string.h>

extern int guest_bfault(), guest_bfcycle(), guest_bframe(), guest_ssw030(), guest_wb040();
extern void guest_bfdone();

static int nfail;
static unsigned char r[64 + 92];

#define	F	(r + 64)
#define	EQ(what, a, b)	((a) == (b) ? 0 : (nfail++, printf("FAIL %s: %lx, want %lx\n", what, (unsigned long)(a), (unsigned long)(b))))

static void
p16(p, v) unsigned char *p; unsigned long v; { p[0] = v >> 8; p[1] = v; }
static void
p32(p, v) unsigned char *p; unsigned long v; { p16(p, v >> 16); p16(p + 2, v); }
static unsigned long
g16(p) unsigned char *p; { return p[0] << 8 | p[1]; }
static unsigned long
g32(p) unsigned char *p; { return g16(p) << 16 | g16(p + 2); }

static void
frame(fv, pc)
	unsigned long fv, pc;
{
	memset(r, 0, sizeof r);
	p32(F + 2, pc);
	p16(F + 6, fv);
}

/* write-backs as put sees them */
static unsigned long wa[4], wv[4];
static int wsz[4], nw, wfail;

static int
put(arg, a, sz, v)
	char *arg;
	unsigned long a, v;
	int sz;
{
	if (nw == wfail)
		return 1;
	wa[nw] = a; wsz[nw] = sz; wv[nw] = v;
	nw++;
	return 0;
}

/* write-back slot 1-3: status, address, data */
static void
wb(i, st, a, d)
	int i;
	unsigned long st, a, d;
{
	p16(F + 20 - 2 * i, st);
	p32(F + 48 - 8 * i, a);
	p32(F + 52 - 8 * i, d);
}

int
main()
{
	unsigned long fa, v;
	int ssw, n;
	char f[92];

	/* 68040: word write to $FF8240 */
	frame(0x7008, 0x1000);
	p16(F + 12, 0x0045);
	p32(F + 20, 0xff8240);
	EQ("040 kind", guest_bfault((char *)r, &fa, &ssw), 0);
	EQ("040 fa", fa, 0xff8240);
	EQ("040 ssw", ssw, 0x45);
	EQ("040 no 030 cycle", guest_bfcycle((char *)r, &v), 0);
	p16(F + 12, 0x0106);			/* supervisor code read */
	EQ("040 fetch", guest_bfault((char *)r, &fa, &ssw), 1);

	/* 68030 $B: byte read of $FFFC02, user data, DF */
	frame(0xb008, 0x2000);
	p16(F + 10, 0x0100 | 0x40 | 0x10 | 1);
	p32(F + 16, 0xfffc02);
	EQ("030 kind", guest_bfault((char *)r, &fa, &ssw), 0);
	EQ("030 fa", fa, 0xfffc02);
	EQ("030 ssw", ssw, 0x100 | 0x20 | 1);
	EQ("030 ssw back", guest_ssw030(ssw), 0x151);
	EQ("030 read cycle", guest_bfcycle((char *)r, &v), 1);
	guest_bfdone((char *)r, 0xa5UL);
	EQ("030 input buffer", g32(F + 44), 0xa5);
	EQ("030 DF cleared", g16(F + 10), 0x51);

	/* 68030 $A: long write, data in the output buffer */
	frame(0xa008, 0x2000);
	p16(F + 10, 0x0100 | 0x00 | 5);
	p32(F + 16, 0xff8200);
	p32(F + 24, 0x12345678);
	EQ("030A kind", guest_bfault((char *)r, &fa, &ssw), 0);
	EQ("030A ssw", ssw, 5);
	EQ("030A write", guest_bfcycle((char *)r, &v), 1);
	EQ("030A data", v, 0x12345678);
	guest_bfdone((char *)r, v);
	EQ("030A DF cleared", g16(F + 10), 5);
	p16(F + 10, 0x0100 | 0x40 | 5);		/* a read needs the long frame */
	EQ("030A read", guest_bfcycle((char *)r, &v), 0);
	p16(F + 10, 0x0100 | 0x80 | 5);		/* read-modify-write */
	EQ("030A rmw", guest_bfcycle((char *)r, &v), 0);

	/* 68030 instruction fetch: stage B fault */
	frame(0xb008, 0x3000);
	p16(F + 10, 0x1000 | 0x5);
	p32(F + 36, 0x3004);
	EQ("030 fetch", guest_bfault((char *)r, &fa, &ssw), 1);
	EQ("030 fetch fa", fa, 0x3004);
	EQ("030 fetch tm", ssw & 7, 6);
	frame(0x0008, 0);
	EQ("format 0", guest_bfault((char *)r, &fa, &ssw), -1);

	/* frames for the guest */
	n = guest_bframe(f, 0x2700, 0x4000UL, 0xffff8000UL, 0x125, 1);
	EQ("B len", n, 92);
	EQ("B fv", g16((unsigned char *)f + 6), 0xb008);
	EQ("B ssw", g16((unsigned char *)f + 10), 0x155);
	EQ("B fa", g32((unsigned char *)f + 16), 0xffff8000);
	EQ("B pc", g32((unsigned char *)f + 2), 0x4000);
	n = guest_bframe(f, 0x2700, 0x4000UL, 0xffff8000UL, 0x125, 0);
	EQ("7 len", n, 60);
	EQ("7 fv", g16((unsigned char *)f + 6), 0x7008);
	EQ("7 ssw", g16((unsigned char *)f + 12), 0x125);
	EQ("7 fa", g32((unsigned char *)f + 20), 0xffff8000);
	EQ("7 ea", g32((unsigned char *)f + 8), 0xffff8000);

	/* 68040 write-backs: WB1 first, its data in the lanes of its address */
	{
		static struct { unsigned long st, a, d, v; } lane[] = {
			{ 0xa1, 0x100, 0xab000000, 0xab },	/* byte */
			{ 0xa1, 0x101, 0x00ab0000, 0xab },
			{ 0xa1, 0x102, 0x0000ab00, 0xab },
			{ 0xa1, 0x103, 0x000000ab, 0xab },
			{ 0xc1, 0x100, 0x12340000, 0x1234 },	/* word */
			{ 0xc1, 0x101, 0x00123400, 0x1234 },
			{ 0xc1, 0x102, 0x00001234, 0x1234 },
			{ 0xc1, 0x103, 0x34000012, 0x1234 },
			{ 0x81, 0x100, 0x12345678, 0x12345678 },	/* long */
			{ 0x81, 0x101, 0x78123456, 0x12345678 },
			{ 0x81, 0x102, 0x56781234, 0x12345678 },
			{ 0x85, 0x103, 0x34567812, 0x12345678 },
		};
		int i;

		for (i = 0; i < sizeof lane / sizeof lane[0]; i++) {
			frame(0x7008, 0x1000);
			wb(1, lane[i].st, lane[i].a, lane[i].d);
			nw = 0; wfail = -1;
			EQ("wb1 count", guest_wb040((char *)r, put, (char *)0), 1);
			EQ("wb1 addr", wa[0], lane[i].a);
			EQ("wb1 size", wsz[0], lane[i].st == 0xa1 ? 1 : lane[i].st == 0xc1 ? 2 : 4);
			EQ("wb1 data", wv[0], lane[i].v);
		}

		/* word write to $FF8240 with a word and a byte behind it, right-justified */
		frame(0x7008, 0x1000);
		wb(3, 0xa5, 0x2001, 0xffffff5a);
		wb(2, 0xc1, 0x2002, 0xffff0102);
		wb(1, 0xc1, 0xff8240, 0x07770000);
		nw = 0; wfail = -1;
		EQ("wb order count", guest_wb040((char *)r, put, (char *)0), 3);
		EQ("wb order 1", wa[0], 0xff8240);
		EQ("wb order 1 data", wv[0], 0x777);
		EQ("wb order 2", wa[1], 0x2002);
		EQ("wb order 2 data", wv[1], 0x102);
		EQ("wb order 3", wa[2], 0x2001);
		EQ("wb order 3 data", wv[2], 0x5a);

		/* nothing pending; a write that fails; MOVE16, line and code write-backs */
		frame(0x7008, 0x1000);
		nw = 0; wfail = -1;
		EQ("wb none", guest_wb040((char *)r, put, (char *)0), 0);
		wb(3, 0x81, 0x3000, 1);
		wb(1, 0x81, 0x2000, 2);
		nw = 0; wfail = 1;
		EQ("wb put fails", guest_wb040((char *)r, put, (char *)0), -1);
		EQ("wb put fails after one", nw, 1);
		frame(0x7008, 0x1000);
		wb(2, 0x89, 0x2000, 0);
		nw = 0; wfail = -1;
		EQ("wb move16", guest_wb040((char *)r, put, (char *)0), -1);
		wb(2, 0xe1, 0x2000, 0);
		EQ("wb line", guest_wb040((char *)r, put, (char *)0), -1);
		wb(2, 0x82, 0x2000, 0);
		EQ("wb code", guest_wb040((char *)r, put, (char *)0), -1);
		EQ("wb none written", nw, 0);
	}
	printf("%s t_gframe\n", nfail ? "[FAIL]" : "[OK]");
	return nfail != 0;
}
