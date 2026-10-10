/* CP/M-80 .COM loader and guest environment. Load the image at 0x100,
 * build page zero and the fake BDOS/BIOS tables, and initialize registers.
 * A GENCOM-bound image is identified by its 0xc9 header; its RSX modules
 * are relocated into the pages below the furniture and chained. */

#include "z80.h"

/* ------------------------------------------------------------------ */
/* small helpers, on the guest's memory rather than on struct z80, so
 * that the furniture can be planted before any state exists	       */

static int pb(mem, a, v)
char *mem;
int a, v;
{
	mem[(z16)a] = (char)v;
	return (0);
}

/* Plant a three-byte 8080 JMP.  Written out rather than folded into the
 * callers because every one of page zero's fixed addresses is a JMP and
 * the byte order of the target is the one thing to get right. */
static int pjmp(mem, a, dst)
char *mem;
int a;
z16 dst;
{
	pb(mem, a, 0xc3);
	pb(mem, a + 1, dst & 0xff);
	pb(mem, a + 2, (dst >> 8) & 0xff);
	return (0);
}

/* Plant a hook stub: `ED FE nn' then a RET, so that a hook reached by a
 * CALL behaves as a subroutine and the guest resumes after it. */
static int phook(mem, a, no)
char *mem;
int a, no;
{
	pb(mem, a, 0xed);
	pb(mem, a + 1, 0xfe);
	pb(mem, a + 2, no);
	pb(mem, a + 3, 0xc9);
	return (0);
}

/* ------------------------------------------------------------------ */
/* the furniture						       */

/* Plant page-zero JMP vectors, BDOS and exit hooks, and the BIOS JMPs.
 * BIOS table index k targets hook HOOK_BIOS+k; index 1 is warm boot. */
int z80furn(m)
struct z80 *m;
{
	register char *mem;
	register int k;
	int stub;

	mem = m->m;

	/* Page zero.  The warm-boot vector points at BIOS table entry 1,
	 * which is where a real CP/M's does, and it is also where a
	 * program looks when it wants the BIOS base: it reads the WORD
	 * at 0x0001 and subtracts 3. */
	pjmp(mem, PZ_WBOOT, (z16)(FAKEBIOS + 3));
	pb(mem, PZ_IOBYTE, 0);
	pb(mem, PZ_CDISK, 0);
	pjmp(mem, PZ_BDOS, (z16)FAKEBDOS);

	/* The BDOS entry, six bytes above the top of the TPA so that
	 * `LHLD 6' answers with a TPA that really does end where we say
	 * it does.  See the FAKEBDOS comment in z80.h. */
	phook(mem, FAKEBDOS, HOOK_BDOS);

	/* The exit stub, and the word the guest's initial SP points at. */
	phook(mem, FAKEEXIT, HOOK_EXIT);

	/* The BIOS table, and its stubs immediately after it. */
	stub = FAKEBIOS + 3 * NBIOSV;
	for (k = 0; k < NBIOSV; k++) {
		pjmp(mem, FAKEBIOS + 3 * k, (z16)(stub + 4 * k));
		phook(mem, stub + 4 * k, HOOK_BIOS + k);
	}

	/* The CCP's second ask, for an RSX-only image. */
	pb(mem, FAKECCP, 0xe);
	pb(mem, FAKECCP + 1, 59);
	pb(mem, FAKECCP + 2, 0xcd);
	pb(mem, FAKECCP + 3, PZ_BDOS);
	pb(mem, FAKECCP + 4, 0);
	pjmp(mem, FAKECCP + 5, (z16)PZ_WBOOT);
	return (0);
}

/* ------------------------------------------------------------------ */
/* the 0xC9 prefix						       */

static z16 gw(img, a)
char *img;
long a;
{
	return ((z16)((img[a] & 0xff) | ((img[a + 1] & 0xff) << 8)));
}

/*
 * z80rsxhdr -- parse a GENCOM-bound .COM's 256-byte header record.
 *
 * Returns 0 if the image does not begin with one, 1 with *r filled in
 * if it does.  Every field is DRI's own, from loader3.asm:96-100 and
 * :234 -- see the struct comrsx comment in z80.h.
 */
int z80rsxhdr(img, n, r)
char *img;
long n;
struct comrsx *r;
{
	register int i, k;
	long d;

	r->comlen = 0;
	r->rsxonly = 0;
	r->n = 0;
	if (n < (long)RSX_HDRLEN || (img[0] & 0xff) != 0xc9)
		return (0);
	r->comlen = gw(img, 1L);
	/* `lda module / cpi ret' (loader3.asm:253): a 0xC9 at the image
	 * base -- file offset 0x100, guest 0x100 -- means there is no
	 * program here at all, only RSXes.  SAVE.COM is exactly that.
	 *
	 * The guard above admits n == RSX_HDRLEN, an image that is the
	 * header record and nothing else, and for that image byte
	 * RSX_HDRLEN is not part of the image: `n' is its length.  So
	 * the length is tested before the byte is read, because the
	 * caller's buffer may be exactly n bytes long and reading past
	 * it would answer this question out of somebody else's memory. */
	r->rsxonly = (n > (long)RSX_HDRLEN &&
		      (img[RSX_HDRLEN] & 0xff) == 0xc9);
	for (i = 0; i < RSX_NDESC; i++) {
		d = (long)RSX_DESC0 + (long)i * RSX_DSTRIDE;
		if (d + RSX_DSTRIDE > n)
			break;
		if (gw(img, d) == 0)		/* loader3.asm:244	*/
			break;
		r->off[i] = gw(img, d);
		r->len[i] = gw(img, d + 2);
		r->nbank[i] = (z8)(img[d + 4] & 0xff);
		for (k = 0; k < 8; k++)
			r->name[i][k] = img[d + 6 + k];
		r->name[i][8] = '\0';
		r->n++;
	}
	return (1);
}

/* ------------------------------------------------------------------ */
/* the RSX chain						       */

z16 z80rsxbase[RSX_NDESC];
int z80nrsx;
int z80rsxonly;
z16 z80rsxtop;
char z80rsxwho[9];

/*
 * rsxplace -- copy one PRL module to `dest', relocate it, and make it
 * the head of the chain.
 *
 * `dest' is a page boundary, because the whole chain is addressed by
 * page: a link holds only the PAGE of the link below it and pairs it
 * with the constant 6, which is the offset of the entry JMP in every
 * prefix and of the entry itself in the BDOS above them.
 *
 * The bitmap follows the image and carries one bit per image byte, most
 * significant bit first; a marked byte is the HIGH half of an address
 * and takes the bias.  The bias is the destination page less one and not
 * the destination page, because a PRL module is linked at 0x0100 --
 * loader3.asm's relocator says so (`dcr e ... base address is now 100h')
 * and the five bound programs agree: for a module of 0x0440 bytes every
 * marked byte holds 0x01 through 0x05, never 0x00.
 */
static int rsxplace(mem, img, off, len, dest)
char *mem, *img;
long off, len;
z16 dest;
{
	register long k;
	long map;
	int bias, head, b;

	map = off + len;
	bias = (int)((dest >> 8) & 0xff) - 1;
	for (k = 0; k < len; k++) {
		b = img[off + k] & 0xff;
		if (img[map + k / RSX_BITS] & (0x80 >> (int)(k % RSX_BITS)))
			b = (b + bias) & 0xff;
		mem[(z16)(dest + k)] = (char)b;
	}

	/* The chain head is whatever the BDOS vector points at now: the
	 * BDOS hook itself for the first module, the module before it for
	 * the rest.  Both are a page plus 6, so one store each way links
	 * them (loader3.asm fixchain). */
	head = (mem[PZ_BDOS + 2] & 0xff) << 8;

	/* Six bytes of serial number on a real CP/M.  We have none, and a
	 * module that read them would read our exit stub, so they are
	 * zeroed rather than copied down from the link above. */
	for (k = 0; k < 6; k++)
		pb(mem, (int)(dest + RSXP_SERIAL + k), 0);
	pb(mem, dest + RSXP_END, 0);
	pb(mem, dest + RSXP_PREV, RSX_HEADPREV & 0xff);
	pb(mem, dest + RSXP_PREV + 1, (RSX_HEADPREV >> 8) & 0xff);

	/* The link above chains back to our NEXT field's high byte, which
	 * is the address removal will store a page into. */
	pb(mem, head + RSXP_PREV, RSXP_NEXTHI);
	pb(mem, head + RSXP_PREV + 1, (dest >> 8) & 0xff);

	pb(mem, dest + RSXP_NEXTLO, RSXP_ENTRY);
	pb(mem, dest + RSXP_NEXTHI, (head >> 8) & 0xff);

	/* The BDOS vector now names us, so `CALL 5' enters this module and
	 * `LHLD 6' answers a TPA ending where we begin. */
	pjmp(mem, PZ_BDOS, (z16)(dest + RSXP_ENTRY));
	return (0);
}

/*
 * rsxload -- place every module a GENCOM-bound image declares.
 *
 * Each goes in the pages directly below the link above it, which for the
 * first module is the page GUESTTOP begins: FAKEDPB, FAKEALV, the BIOS
 * table and the SCB image are all above GUESTTOP and stay there.  The
 * floor is the end of the .COM half, and a module that will not fit
 * between the two is refused by name rather than written over a program.
 */
static int rsxload(mem, img, n, r)
char *mem, *img;
long n;
struct comrsx *r;
{
	register int i, k;
	long off, len, end, head;
	z16 dest;
	int pages;

	for (i = 0; i < r->n; i++) {
		off = (long)r->off[i];
		len = (long)r->len[i];
		if (len == 0)
			continue;
		end = off + len + (len + RSX_BITS - 1) / RSX_BITS;
		if (off < (long)RSX_HDRLEN || end > n)
			return (CL_RSX);
		head = (long)(mem[PZ_BDOS + 2] & 0xff) << 8;
		pages = (int)((len - 1) / 256) + 1;
		if (head < (long)pages * 256
		 || head - (long)pages * 256
			< (long)COM_ORG + (long)r->comlen) {
			for (k = 0; k < 9; k++)
				z80rsxwho[k] = r->name[i][k];
			return (CL_RSXFIT);
		}
		dest = (z16)(head - (long)pages * 256);
		rsxplace(mem, img, off, len, dest);
		z80rsxbase[z80nrsx++] = dest;
		z80rsxtop = dest;
	}
	return (CL_OK);
}

/*
 * z80rsxwboot -- the warm-boot half of the chain's life.
 *
 * Walk it from the head and unlink every module whose remove flag is
 * 0xFF, which is what the CCP does on every warm start (loader3.asm
 * rsx$chain).  The memory is not reclaimed: the links above and below a
 * removed module simply stop naming it, and the BDOS vector rises again
 * when the head goes.  Returns the number removed.
 *
 * DRI's walk stops on its own LOADER module's flag at RSXP_END; ours
 * stops on the BDOS page, because that is what our chain ends at.
 */
int z80rsxwboot(m)
struct z80 *m;
{
	register char *mem;
	register int cur, nxt;
	int prev, gone, k;

	mem = m->m;
	gone = 0;
	cur = (mem[PZ_BDOS + 2] & 0xff) << 8;
	for (k = 0; k <= RSX_NDESC; k++) {
		if ((cur >> 8) == (GUESTTOP >> 8))
			break;
		nxt = (mem[(z16)(cur + RSXP_NEXTHI)] & 0xff) << 8;
		if ((mem[(z16)(cur + RSXP_WARM)] & 0xff) == 0xff) {
			prev = (mem[(z16)(cur + RSXP_PREV)] & 0xff)
			     | ((mem[(z16)(cur + RSXP_PREV + 1)] & 0xff) << 8);
			/* PREV addresses a high byte, and its low
			 * neighbour is the 6 that goes with it.  For the
			 * head that pair is 0x0007 and 0x0006, so page
			 * zero's BDOS vector is re-pointed by the same
			 * two stores as any other link. */
			pb(mem, prev, (nxt >> 8) & 0xff);
			pb(mem, prev - 1, RSXP_ENTRY);
			pb(mem, nxt + RSXP_PREV, prev & 0xff);
			pb(mem, nxt + RSXP_PREV + 1, (prev >> 8) & 0xff);
			gone++;
		}
		cur = nxt;
	}
	return (gone);
}

/* ------------------------------------------------------------------ */
/* the command tail and the two default FCBs			       */

/*
 * Parse one blank-delimited token into a 16-byte FCB image: drive byte,
 * eight name characters, three type characters, all blank-padded and
 * upper-cased, with `*' expanded into `?' the way the CCP does.  The
 * remaining bytes are left alone, because FCB2 at 0x6C overlaps FCB1's
 * tail and the CCP writes them in this order for that reason.
 *
 * Returns the index of the first character it did not consume.
 */
static int fcb1(mem, at, s, i)
char *mem;
int at;
char *s;
int i;
{
	register int c, k;
	int drv, dot;

	for (k = 0; k < 11; k++)	/* eight name, three type	*/
		pb(mem, at + 1 + k, ' ');
	pb(mem, at, 0);
	while (s[i] == ' ')
		i++;
	if (s[i] == '\0')
		return (i);
	/* A drive letter is `X:' and nothing else: one letter, then a
	 * colon.  1 means A, which is the CP/M convention and the reason
	 * 0 can mean "the default drive". */
	drv = 0;
	c = s[i] & 0xff;
	if (c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	if (c >= 'A' && c <= 'Z' && s[i + 1] == ':') {
		drv = c - 'A' + 1;
		i += 2;
	}
	pb(mem, at, drv);
	k = 0;
	dot = 0;
	while (s[i] != '\0' && s[i] != ' ' && s[i] != '=' && s[i] != ',') {
		c = s[i] & 0xff;
		i++;
		if (c >= 'a' && c <= 'z')
			c -= 'a' - 'A';
		if (c == '.') {
			if (dot)
				break;
			dot = 1;
			k = 8;
			continue;
		}
		if (c == '*') {
			/* `*' is not stored: it fills the rest of its
			 * field with `?', which is what a directory match
			 * actually compares against. */
			while (k < (dot ? 11 : 8))
				pb(mem, at + 1 + k++, '?');
			continue;
		}
		if (k < (dot ? 11 : 8))
			pb(mem, at + 1 + k++, c);
	}
	return (i);
}

/*
 * z80tail -- build the command tail at 0x0080 and the two default FCBs.
 *
 * The tail is stored the way the CCP stores it: a length byte, then the
 * characters, then a NUL.  The CCP upper-cases the whole command line
 * before it gets here, and programs rely on that -- PIP parses its own
 * tail rather than using the FCBs, and it compares against upper case.
 *
 * Returns the tail length.
 */
int z80tail(m, tail)
struct z80 *m;
char *tail;
{
	register char *mem;
	register int i, c;
	int n;

	mem = m->m;
	n = 0;
	for (i = 0; tail[i] != '\0' && n < 127; i++) {
		c = tail[i] & 0xff;
		if (c >= 'a' && c <= 'z')
			c -= 'a' - 'A';
		pb(mem, PZ_DMA + 1 + n, c);
		n++;
	}
	pb(mem, PZ_DMA, n);
	pb(mem, PZ_DMA + 1 + n, 0);

	/* FCB1 first and in full: its 36 bytes run from 0x5C to 0x7F, so
	 * zeroing it clears FCB2's area as well.  Then FCB2's 16 bytes
	 * are written over the middle of it, which is exactly what the
	 * CCP does and why the second FCB has no extent or record
	 * fields of its own. */
	for (i = 0; i < 36; i++)
		pb(mem, PZ_FCB1 + i, 0);
	i = fcb1(mem, PZ_FCB1, (char *)mem + PZ_DMA + 1, 0);
	while (mem[PZ_DMA + 1 + i] == ' ' || mem[PZ_DMA + 1 + i] == '='
	    || mem[PZ_DMA + 1 + i] == ',')
		i++;
	fcb1(mem, PZ_FCB2, (char *)mem + PZ_DMA + 1, i);
	return (n);
}

/* ------------------------------------------------------------------ */
/* the load itself						       */

char *z80lerr(e)
int e;
{
	static char fit[64];
	static char pre[] = "this RSX does not fit under the TPA ceiling: ";
	register int i, k;

	switch (e) {
	case CL_OK:	return ("ok");
	case CL_EMPTY:	return ("the file is empty");
	case CL_BIG:	return ("the image does not fit under the TPA ceiling");
	case CL_RSX:	return ("a GENCOM-bound .COM whose RSX runs off the end of the file");
	case CL_NOTCOM:	return ("the first byte is 00 or FF: this file was never written");
	case CL_RSXFIT:
		/* Built by hand rather than with sprintf(): this file is
		 * compiled for the target as well, where the loader has no
		 * business dragging in stdio. */
		for (i = 0; pre[i] != '\0'; i++)
			fit[i] = pre[i];
		for (k = 0; k < 8 && z80rsxwho[k] != '\0'; k++)
			fit[i++] = z80rsxwho[k];
		fit[i] = '\0';
		return (fit);
	}
	return ("unknown");
}

/*
 * z80load -- bind 64 KB, plant the furniture, place the image, and set
 * the registers a .COM starts with.
 *
 * `mem' is 65,536 bytes the caller owns.  On the target it is a whole
 * host segment, and the guest's 16-bit address is its offset with no
 * arithmetic at all; on the host it is an array, and the (z16) casts in
 * z80exec.c give it the same wraparound.
 */
int z80load(m, mem, img, n)
struct z80 *m;
char *mem, *img;
long n;
{
	register long i;
	long len;
	int e;
	struct comrsx r;

	for (i = 0; i < 4; i++)
		m->rp[i] = 0;
	m->a = 0;
	m->f = F_ONE;			/* bit 1 reads back as one	*/
	m->pc = 0;
	m->ix = 0;
	m->iy = 0;
	for (i = 0; i < 3; i++)
		m->arp[i] = 0;
	m->aa = 0;
	m->af = F_ONE;
	m->iff = 0;
	m->lz = LZ_NONE;
	m->lc = 0;
	m->la = 0;
	m->lb = 0;
	m->lr = 0;
	m->halt = 0;
	m->m = mem;

	segclr(mem);

	z80nrsx = 0;
	z80rsxonly = 0;
	z80rsxtop = GUESTTOP;
	z80rsxwho[0] = '\0';

	if (n <= 0)
		return (CL_EMPTY);
	if (z80rsxhdr(img, n, &r)) {
		/* The .COM half is comlen bytes behind the header record,
		 * and comlen is the field to trust: what follows it is the
		 * first RSX, not more program. */
		z80rsxonly = r.rsxonly;
		len = (long)r.comlen;
		if (len > n - (long)RSX_HDRLEN)
			len = n - (long)RSX_HDRLEN;
		if ((long)COM_ORG + len > (long)GUESTTOP)
			return (CL_BIG);
		for (i = 0; i < len; i++)
			mem[COM_ORG + i] = img[RSX_HDRLEN + i];
		z80furn(m);
		/* After the furniture, because the first module is placed
		 * below whatever the BDOS vector names and that vector is
		 * what z80furn() has just planted. */
		e = rsxload(mem, img, n, &r);
		if (e != CL_OK)
			return (e);
	} else {
		if ((img[0] & 0xff) == 0x00 || (img[0] & 0xff) == 0xff) {
			/* Refuse leading 0x00/0xff as an unwritten-image
			 * heuristic. */
			return (CL_NOTCOM);
		}
		if (n > (long)(GUESTTOP - COM_ORG))
			return (CL_BIG);

		for (i = 0; i < n; i++)
			mem[COM_ORG + i] = img[i];

		z80furn(m);
	}

	/* The stack the CCP hands over: SP two below the top of the TPA,
	 * with the exit stub's address on top, so that a program
	 * terminating with a plain RET -- the documented CP/M-80 way --
	 * lands on a hook and not in the middle of our furniture.  With
	 * RSXes loaded the top of the TPA is the lowest of them, and the
	 * stack has to come down with it or the guest's first PUSH would
	 * land inside a module. */
	m->rp[P_SP] = (z16)(z80rsxtop - 2);
	mem[(z16)(z80rsxtop - 2)] = (char)(FAKEEXIT & 0xff);
	mem[(z16)(z80rsxtop - 1)] = (char)((FAKEEXIT >> 8) & 0xff);
	/* An RSX-only image starts at the CCP stub rather than at the RET
	 * the .COM half consists of: the modules are the program, and
	 * they are entered by the second function 59. */
	m->pc = z80rsxonly ? (z16)FAKECCP : (z16)COM_ORG;
	return (CL_OK);
}
