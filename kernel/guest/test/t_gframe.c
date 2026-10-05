/* host checks: 68030/68040 access fault frames decoded, finished, built */
#include <stdio.h>
#include <string.h>

extern int guest_bfault(), guest_bfcycle(), guest_bframe(), guest_ssw030();
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
	printf("%s t_gframe\n", nfail ? "[FAIL]" : "[OK]");
	return nfail != 0;
}
