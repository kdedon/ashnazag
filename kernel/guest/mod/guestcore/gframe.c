/*
 * gframe.c -- access fault frames of the 68030 and 68040: decoded to
 * one form, finished by the handler, built for a guest.  r is the
 * saved registers with the exception frame at r + 64.  The SSW is
 * kept in the 68040's layout: RW 0x100, size 0x60, TT/TM 0x1f.  Byte
 * access throughout, so the host checks run the same code.
 */

#define	G16(p)		((p)[0] << 8 | (p)[1])
#define	G32(p)		((unsigned long)G16(p) << 16 | G16((p) + 2))
#define	S16(p, v)	((p)[0] = (v) >> 8 & 0xff, (p)[1] = (v) & 0xff)
#define	S32(p, v)	(S16(p, (v) >> 16), S16((p) + 2, v))

#define	DF		0x100		/* 68030 SSW: data fault, rerun on rte */
#define	RM		0x80		/* read-modify-write */

/*
 * The fault address and SSW of a format 7, $A or $B frame.  0: a data
 * fault, 1: an instruction fetch (TM program space), -1: no access fault.
 */
int
guest_bfault(r, fa, ssw)
	char *r;
	unsigned long *fa;
	int *ssw;
{
	unsigned char *f = (unsigned char *)r + 64;
	int s, fmt = f[6] >> 4;

	if (fmt == 7) {
		*fa = G32(f + 20);
		*ssw = G16(f + 12);
		return (*ssw & 3) == 2;
	}
	if (fmt != 0xa && fmt != 0xb)
		return -1;
	s = G16(f + 10);
	if (s & DF) {
		*fa = G32(f + 16);
		*ssw = (s & 0x40) << 2 | (s & 0x30) << 1 | (s & 7);
		return 0;
	}
	/* stage B, else C: the words after the PC */
	*fa = fmt == 0xb && (s & 0x1000) ? G32(f + 36) : G32(f + 2) + ((s & 0x1000) ? 4 : 2);
	*ssw = 0x100 | (s & 4) | 2;
	return 1;
}

/*
 * 1: a 68030 data fault the handler can finish itself, *v the data of
 * a write.  Reads need the long frame's input buffer.
 */
int
guest_bfcycle(r, v)
	char *r;
	unsigned long *v;
{
	unsigned char *f = (unsigned char *)r + 64;
	int s = G16(f + 10), fmt = f[6] >> 4;

	if ((fmt != 0xa && fmt != 0xb) || !(s & DF) || (s & RM) || ((s & 0x40) && fmt != 0xb))
		return 0;
	*v = G32(f + 24);
	return 1;
}

/* the cycle is done: rte goes on without rerunning it; v is a read's data */
void
guest_bfdone(r, v)
	char *r;
	unsigned long v;
{
	unsigned char *f = (unsigned char *)r + 64;
	int s = G16(f + 10);

	if (s & 0x40)
		S32(f + 44, v);
	S16(f + 10, s & ~DF);
}

/* a 68030 data fault SSW: DF, RW, size, function code */
int
guest_ssw030(ssw)
	int ssw;
{
	return DF | (ssw >> 2 & 0x40) | (ssw >> 1 & 0x30) | (ssw & 7);
}

/*
 * A bus error frame for the guest's vector 2 at f: 68030 format $B
 * (92 bytes) or 68040 format 7 (60).  Its length.
 */
int
guest_bframe(f, sr, pc, fa, ssw, cpu030)
	char *f;
	int sr, ssw, cpu030;
	unsigned long pc, fa;
{
	unsigned char *b = (unsigned char *)f;
	int i, n = cpu030 ? 92 : 60;

	for (i = 0; i < n; i++)
		b[i] = 0;
	S16(b, sr);
	S32(b + 2, pc);
	if (cpu030) {
		S16(b + 6, 0xb008);
		S16(b + 10, guest_ssw030(ssw));
		S32(b + 16, fa);
	} else {
		S16(b + 6, 0x7008);
		S32(b + 8, fa);
		S16(b + 12, ssw & 0x017f);	/* RW, size, TT, TM */
		S32(b + 20, fa);
	}
	return n;
}
