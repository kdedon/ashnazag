/*
 * coff2elf -- convert an AT&T COFF file into ELF32.  The input kind is told
 * by f_magic:
 *
 *   0x14C  Coherent i386 COFF (MWC cc), little-endian.
 *	    Relocatable -> ET_REL/EM_386 with REL relocations.
 *   0x150  m68k COFF (A/UX), big-endian.
 *	    Relocatable -> ET_REL/EM_68K with RELA relocations.
 *	    Executable (F_EXEC) -> ET_EXEC/EM_68K: sections at their COFF
 *	    addresses, a PT_LOAD per loaded section, the a.out entry point.
 *
 * Reads COFF by explicit byte offsets -- never by struct overlay -- so the
 * same source is correct under an LP64 host compiler and an ILP32 native one.
 *
 *	usage:  coff2elf [-u] [-x] [-l] [-R root] in out
 *	  -u	strip one leading underscore from symbol names
 *	  -x	write ET_EXEC even if F_EXEC is clear (e.g. a shared library)
 *	  -l	ET_EXEC only: load the static shared libraries named in the
 *		.lib section as extra sections and PT_LOAD segments
 *	  -R	directory the .lib path names are looked up under
 *
 * K&R C.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* COFF on-disk sizes. */
#define	FILHSZ	20		/* file header			*/
#define	AOUTSZ	28		/* a.out optional header	*/
#define	SCNHSZ	40		/* section header		*/
#define	SYMESZ	18		/* symbol			*/
#define	RELSZ	10		/* relocation			*/
#define	SYMNMLEN 8
#define	LIBNMLEN 64		/* A/UX .lib entry: NUL-padded path */

#define	I386MAGIC 0x14C
#define	M68MAGIC  0x150

/* f_flags */
#define	F_EXEC	0x2

/* s_flags */
#define	STYP_DSECT	0x1
#define	STYP_NOLOAD	0x2
#define	STYP_TEXT	0x20
#define	STYP_DATA	0x40
#define	STYP_BSS	0x80
#define	STYP_LIB	0x800

/* storage classes */
#define	C_EXT		2
#define	C_STAT		3
#define	C_EXTDEF	5
#define	C_LABEL		6

/* section numbers */
#define	N_DEBUG	(-2)
#define	N_ABS	(-1)
#define	N_UNDEF	0

/* relocation types */
#define	R_DIR32		0x06
#define	R_RELBYTE	0x0F
#define	R_RELWORD	0x10
#define	R_RELLONG	0x11
#define	R_PCRBYTE	0x12
#define	R_PCRWORD	0x13
#define	R_PCRLONG	0x14

/* ELF32 */
#define	ET_REL		1
#define	ET_EXEC		2
#define	EM_386		3
#define	EM_68K		4
#define	EV_CURRENT	1
#define	SHT_PROGBITS	1
#define	SHT_SYMTAB	2
#define	SHT_STRTAB	3
#define	SHT_RELA	4
#define	SHT_NOBITS	8
#define	SHT_REL		9
#define	SHF_WRITE	0x1
#define	SHF_ALLOC	0x2
#define	SHF_EXEC	0x4
#define	STB_LOCAL	0
#define	STB_GLOBAL	1
#define	STT_NOTYPE	0
#define	STT_OBJECT	1
#define	STT_FUNC	2
#define	STT_SECTION	3
#define	SHN_UNDEF	0
#define	SHN_ABS		0xFFF1
#define	SHN_COMMON	0xFFF2
#define	PT_LOAD		1
#define	PF_X		1
#define	PF_W		2
#define	PF_R		4
#define	R_386_32	1
#define	R_386_PC32	2
#define	R_68K_32	1
#define	R_68K_16	2
#define	R_68K_8		3
#define	R_68K_PC32	4
#define	R_68K_PC16	5
#define	R_68K_PC8	6

#define	EHSZ	52
#define	PHSZ	32
#define	SHSZ	40
#define	SYMSZ	16
#define	PGSZ	0x1000		/* file offset = address, modulo this	*/

#define	MAXSEC	128
#define	MAXLIB	8

/* A growable byte buffer. */
struct buf {
	unsigned char	*b;
	long		len, cap;
};

/* An output section. */
struct osec {
	long		name;		/* offset in .shstrtab		*/
	unsigned	type, flags, addr, paddr, size;
	unsigned	link, info, align, entsz;
	long		off;		/* file offset in the output	*/
	unsigned char	*img;		/* bytes come from here ...	*/
	long		src, imglen;	/* ... at this offset		*/
	int		load;		/* ET_EXEC: needs a PT_LOAD	*/
};

/* A PT_LOAD: a leading section plus any NOBITS ones that follow it. */
struct seg {
	int		sec;
	unsigned	memsz, flags;
};

char	*progname;
int	uflag, xflag, lflag;
char	*root = "";

int	bigend;			/* m68k: both files are big-endian	*/
int	m68k;
int	isexec;			/* writing ET_EXEC			*/

/* The COFF image being read (the input, or a library while loading it). */
unsigned char	*cf;
long		cflen;

/* The input file's header fields and per-section data (1-based). */
unsigned char	*mainimg;
long		mainlen;
int		nscns;
long		symptr, nsyms;
long		scnoff;
int		secmap[MAXSEC];		/* COFF section -> ELF section	*/
unsigned	secbase[MAXSEC];	/* COFF address of its start	*/
int		relsec[MAXSEC];		/* ELF .rel(a) section, or 0	*/
int		secsym[MAXSEC];		/* ELF STT_SECTION symbol	*/

/* Per COFF symbol of the input: ELF symbol, and the value the in-place
   relocation field was computed from. */
int		*symmap;
unsigned	*symbase;

/* Libraries loaded by -l. */
int		nlib;
unsigned char	*libimg[MAXLIB];
long		liblen[MAXLIB];
int		libmap[MAXLIB][MAXSEC];

struct osec	os[MAXSEC];
int		nos;
struct seg	sg[MAXSEC];
int		nsg;

struct buf	out, esym, strtab, shstr;
int		nesym;

unsigned char	*slurp();
long		bufadd();
unsigned	u16(), u32(), get32(), getfld();

fatal(s, a)
char	*s, *a;
{
	fprintf(stderr, "%s: ", progname);
	fprintf(stderr, s, a);
	fprintf(stderr, "\n");
	exit(1);
}

/* Die unless n bytes at offset o lie inside the current image. */
need(o, n)
long	o, n;
{
	if (o < 0 || n < 0 || o + n > cflen)
		fatal("truncated or corrupt COFF file", (char *)0);
}

/*
 * Field readers over the current image, in the input's byte order.
 */
unsigned
u16(o)
long	o;
{
	need(o, 2L);
	if (bigend)
		return ((cf[o]<<8) | cf[o+1]);
	return (cf[o] | (cf[o+1]<<8));
}

unsigned
u32(o)
long	o;
{
	need(o, 4L);
	if (bigend)
		return (((unsigned)cf[o]<<24) | (cf[o+1]<<16) | (cf[o+2]<<8)
			| cf[o+3]);
	return ((unsigned)cf[o] | (cf[o+1]<<8) | (cf[o+2]<<16)
		| ((unsigned)cf[o+3]<<24));
}

/* A 16-bit field as a signed section number. */
int
s16(o)
long	o;
{
	int	v;

	v = u16(o);
	if (v & 0x8000)
		v -= 0x10000;
	return (v);
}

/*
 * Accessors for memory in the output's byte order.
 */
put16(p, v)
unsigned char	*p;
unsigned	v;
{
	if (bigend) {
		p[0] = v>>8; p[1] = v;
	} else {
		p[0] = v; p[1] = v>>8;
	}
}

put32(p, v)
unsigned char	*p;
unsigned	v;
{
	if (bigend) {
		p[0] = v>>24; p[1] = v>>16; p[2] = v>>8; p[3] = v;
	} else {
		p[0] = v; p[1] = v>>8; p[2] = v>>16; p[3] = v>>24;
	}
}

unsigned
get32(p)
unsigned char	*p;
{
	if (bigend)
		return (((unsigned)p[0]<<24) | (p[1]<<16) | (p[2]<<8) | p[3]);
	return ((unsigned)p[0] | (p[1]<<8) | (p[2]<<16)
		| ((unsigned)p[3]<<24));
}

/* Read a 1-, 2- or 4-byte field, sign-extended. */
unsigned
getfld(p, w)
unsigned char	*p;
{
	unsigned	v;

	if (w == 1) {
		v = p[0];
		if (v & 0x80)
			v -= 0x100;
	} else if (w == 2) {
		v = bigend ? (p[0]<<8) | p[1] : p[0] | (p[1]<<8);
		if (v & 0x8000)
			v -= 0x10000;
	} else
		v = get32(p);
	return (v);
}

putfld(p, w, v)
unsigned char	*p;
unsigned	v;
{
	if (w == 1)
		p[0] = v;
	else if (w == 2)
		put16(p, v);
	else
		put32(p, v);
}

/*
 * Growable buffers.
 */
grow(bp, n)
struct buf	*bp;
long		n;
{
	if (bp->len + n > bp->cap) {
		bp->cap = (bp->len + n)*2 + 1024;
		bp->b = (unsigned char *)realloc((char *)bp->b, (int)bp->cap);
		if (bp->b == NULL)
			fatal("out of memory", (char *)0);
	}
}

/* Append n bytes (zeros if p is NULL); return where they went. */
long
bufadd(bp, p, n)
struct buf	*bp;
unsigned char	*p;
long		n;
{
	long	at;

	grow(bp, n);
	at = bp->len;
	if (p != NULL)
		memcpy((char *)bp->b + at, (char *)p, (int)n);
	else
		memset((char *)bp->b + at, 0, (int)n);
	bp->len += n;
	return (at);
}

bufput32(bp, v)
struct buf	*bp;
unsigned	v;
{
	long	at;

	at = bufadd(bp, (unsigned char *)NULL, 4L);
	put32(bp->b + at, v);
}

/* Add a NUL-terminated string to a string table. */
long
stradd(bp, s)
struct buf	*bp;
char		*s;
{
	return (bufadd(bp, (unsigned char *)s, (long)strlen(s) + 1));
}

/* Read a whole file. */
unsigned char *
slurp(name, lenp)
char	*name;
long	*lenp;
{
	FILE		*fp;
	unsigned char	*p;
	long		n;

	fp = fopen(name, "rb");
	if (fp == NULL)
		fatal("cannot open %s", name);
	fseek(fp, 0L, 2);
	n = ftell(fp);
	fseek(fp, 0L, 0);
	p = (unsigned char *)malloc(n ? (int)n : 1);
	if (p == NULL || fread((char *)p, 1, (int)n, fp) != n)
		fatal("cannot read %s", name);
	fclose(fp);
	*lenp = n;
	return (p);
}

/*
 * Append one ELF symbol; return its index.
 */
int
esyment(name, value, size, shndx, bind, type)
char		*name;		/* explicit: implicit-int would truncate on LP64 */
unsigned	value, size;
{
	unsigned char	*p;
	unsigned	no;
	long		at;

	no = name ? stradd(&strtab, name) : 0;
	at = bufadd(&esym, (unsigned char *)NULL, (long)SYMSZ);
	p = esym.b + at;
	put32(p, no);
	put32(p+4, value);
	put32(p+8, size);
	p[12] = (bind<<4) | (type&0xF);
	p[13] = 0;
	put16(p+14, shndx);
	return (nesym++);
}

/*
 * Read the COFF symbol name at `o' into buf (at least 256 bytes): an inline
 * 8-char name, or, if the first 4 bytes are zero, an offset into the string
 * table that follows the symbols.
 */
symname(o, buf)
long	o;
char	*buf;
{
	char	*p;
	long	so;
	int	i;

	if (u32(o) == 0) {
		so = u32(8L) + u32(12L)*SYMESZ + u32(o+4);
		need(so, 1L);
		for (i = 0; i < 255 && so+i < cflen && cf[so+i]; i++)
			buf[i] = cf[so+i];
		buf[i] = 0;
	} else {
		for (i = 0; i < SYMNMLEN; i++)
			buf[i] = cf[o+i];
		buf[SYMNMLEN] = 0;
	}
	if (uflag && buf[0] == '_') {
		for (p = buf; (p[0] = p[1]) != 0; p++)
			;
	}
}

/* Offset of COFF section header k (1-based) in the current image. */
long
shoff(k)
{
	return (FILHSZ + u16(16L) + (long)(k-1)*SCNHSZ);
}

/* Is `nm' the name of COFF section k? */
int
issecname(nm, k)
char	*nm;
{
	long	o;

	o = shoff(k);
	need(o, 8L);
	return (strncmp(nm, (char *)cf + o, SYMNMLEN) == 0 && strlen(nm) <= 8);
}

/* Add an output section; return its index. */
int
newsec(name, type, flags, addr, size)
char		*name;
unsigned	type, flags, addr, size;
{
	struct osec	*s;

	if (nos >= MAXSEC)
		fatal("too many sections", (char *)0);
	s = &os[nos];
	memset((char *)s, 0, sizeof(*s));
	s->name = name ? stradd(&shstr, name) : 0;
	s->type = type;
	s->flags = flags;
	s->addr = s->paddr = addr;
	s->size = size;
	s->align = 4;
	if (isexec && (addr & 3) != 0)
		s->align = (addr & 1) ? 1 : 2;
	return (nos++);
}

/*
 * Add an output section for COFF section header `o' of the current image.
 * Loaded sections keep their COFF address in an executable.
 */
int
coffsec(o, name)
long	o;
char	*name;
{
	unsigned	sf, addr, size, type, flags;
	long		scnptr;
	int		s;

	sf = u32(o+36);
	size = u32(o+16);
	scnptr = u32(o+20);
	flags = 0;
	if ((sf & (STYP_TEXT|STYP_DATA|STYP_BSS)) && !(sf & STYP_DSECT)
	    && !(!isexec && (sf & STYP_NOLOAD))) {
		flags = SHF_ALLOC;
		if (sf & STYP_TEXT)
			flags |= SHF_EXEC;
		if (sf & (STYP_DATA|STYP_BSS))
			flags |= SHF_WRITE;
	}
	type = SHT_PROGBITS;
	if (scnptr == 0 || (sf & STYP_DSECT)
	    || (sf & (STYP_TEXT|STYP_DATA|STYP_BSS)) == STYP_BSS)
		type = SHT_NOBITS;
	addr = 0;
	if (isexec && flags)
		addr = u32(o+12);
	s = newsec(name, type, flags, addr, size);
	if (isexec && flags)
		os[s].paddr = u32(o+8);
	if (type != SHT_NOBITS && size) {
		need(scnptr, (long)size);
		os[s].img = cf;
		os[s].imglen = cflen;
		os[s].src = scnptr;
	}
	os[s].load = isexec && flags && size && !(sf & STYP_NOLOAD);
	return (s);
}

/*
 * Add the symbols of the COFF image in cf to the ELF symbol table: the
 * locals when glob is 0, else the globals.  map[] turns a COFF section number
 * into an ELF section.  For the input file (main set), record each COFF
 * symbol's ELF symbol and the value its relocations were computed from.
 */
addsyms(glob, map, main)
int	*map;
{
	char		nm[260];
	long		o, sp, ns, i;
	int		sclass, scnum, naux, g, shndx, type, n;
	unsigned	val, size, sf;

	sp = u32(8L);
	ns = u32(12L);
	n = u16(2L);
	for (i = 0; i < ns; i += 1 + naux) {
		o = sp + i*SYMESZ;
		need(o, (long)SYMESZ);
		val = u32(o+8);
		scnum = s16(o+12);
		sclass = cf[o+16];
		naux = cf[o+17];

		if (sclass == C_EXT || sclass == C_EXTDEF)
			g = 1;
		else if (sclass == C_STAT || sclass == C_LABEL)
			g = 0;
		else
			continue;
		if (g != glob || scnum == N_DEBUG || scnum > n)
			continue;
		symname(o, nm);
		if (scnum > 0 && !g && issecname(nm, scnum)) {
			/* a section symbol: relocations use the ELF one */
			if (main && !isexec) {
				symmap[i] = secsym[scnum];
				symbase[i] = secbase[scnum];
			}
			continue;
		}
		if (nm[0] == 0)
			continue;
		size = 0;
		type = STT_NOTYPE;
		if (scnum == N_UNDEF) {
			shndx = SHN_UNDEF;
			if (val != 0 && !isexec) {	/* common: val is its size */
				shndx = SHN_COMMON;
				size = val;
				val = size >= 4 ? 4 : size >= 2 ? 2 : 1;
				type = STT_OBJECT;
			}
		} else if (scnum == N_ABS) {
			shndx = SHN_ABS;
		} else {
			sf = u32(shoff(scnum) + 36);
			shndx = map[scnum];
			if (sf & STYP_DSECT)
				shndx = SHN_ABS;
			if (!isexec)
				val -= secbase[scnum];
			if (sclass != C_LABEL) {
				if (sf & STYP_TEXT)
					type = STT_FUNC;
				else if (sf & (STYP_DATA|STYP_BSS))
					type = STT_OBJECT;
			}
		}
		o = esyment(nm, val, size, shndx, g ? STB_GLOBAL : STB_LOCAL,
			type);
		if (main) {
			symmap[i] = o;
			symbase[i] = u32(sp + i*SYMESZ + 8);
		}
	}
}

/*
 * Warn when library section s differs in size from the NOLOAD section the
 * executable was linked with at that address.
 */
libcheck(s, path)
char	*path;
{
	int	i;

	for (i = 1; i <= nscns; i++)
		if (os[secmap[i]].flags && !os[secmap[i]].load
		    && os[secmap[i]].addr == os[s].addr
		    && os[secmap[i]].size != os[s].size)
			fprintf(stderr,
			    "%s: warning: %s: section at 0x%x is 0x%x bytes; "
			    "the executable expects 0x%x\n", progname, path,
			    os[s].addr, os[s].size, os[secmap[i]].size);
}

/*
 * Load the static shared libraries named in the .lib section at `o': A/UX
 * writes each as a 64-byte NUL-padded path; SVR3 as entries of (length in
 * words, offset of the path in words, path).  Each library's TEXT/DATA/BSS
 * become loaded sections at their fixed addresses.
 */
loadlibs(o)
long	o;
{
	char		path[1024], nm[16], *base;
	long		p, end, n, q;
	int		k, svr3;

	p = u32(o+20);
	end = p + u32(o+16);
	need(p, end - p);
	svr3 = end - p >= 8 && u32(p) < 0x100;
	while (p < end) {
		if (svr3) {
			n = u32(p) * 4;
			q = p + u32(p+4) * 4;
			if (n < 12 || p + n > end || q >= p + n)
				fatal(".lib entry is corrupt", (char *)0);
		} else {
			n = LIBNMLEN;
			q = p;
			if (p + n > end)
				break;
		}
		strcpy(path, root);
		k = strlen(path);
		while (k < (int)sizeof(path) - 1 && q < p + n && cf[q])
			path[k++] = cf[q++];
		path[k] = 0;
		p += n;
		if (path[strlen(root)] == 0)
			continue;
		if (nlib >= MAXLIB)
			fatal("too many shared libraries", (char *)0);
		base = strrchr(path, '/');
		base = base ? base+1 : path;

		cf = libimg[nlib] = slurp(path, &liblen[nlib]);
		cflen = liblen[nlib];
		if (cflen < FILHSZ || u16(0L) != M68MAGIC)
			fatal("%s: not an m68k COFF file", path);
		for (k = 1; k <= (int)u16(2L); k++) {
			unsigned	sf;
			long		h;
			char		name[128];
			int		s;

			if (k >= MAXSEC)
				fatal("too many sections", (char *)0);
			h = shoff(k);
			need(h, (long)SCNHSZ);
			sf = u32(h+36);
			libmap[nlib][k] = SHN_ABS;
			if (!(sf & (STYP_TEXT|STYP_DATA|STYP_BSS))
			    || (sf & (STYP_DSECT|STYP_NOLOAD)))
				continue;
			memcpy(nm, (char *)cf + h, SYMNMLEN);
			nm[SYMNMLEN] = 0;
			sprintf(name, "%s.%.60s", nm, base);
			s = coffsec(h, name);
			libmap[nlib][k] = s;
			libcheck(s, path);
		}
		nlib++;
		cf = mainimg;
		cflen = mainlen;
	}
}

/*
 * Group the loaded sections into PT_LOAD segments, in address order.  A
 * NOBITS section that starts where a segment ends joins it.
 */
mksegs()
{
	int		ord[MAXSEC], n, i, j, t;
	struct osec	*s;
	struct seg	*g;

	n = 0;
	for (i = 1; i < nos; i++)
		if (os[i].load) {
			for (j = n; j > 0 && os[ord[j-1]].addr > os[i].addr; j--)
				ord[j] = ord[j-1];
			ord[j] = i;
			n++;
		}
	nsg = 0;
	for (i = 0; i < n; i++) {
		s = &os[ord[i]];
		t = PF_R;
		if (s->flags & SHF_EXEC)
			t |= PF_X;
		if (s->flags & SHF_WRITE)
			t |= PF_W;
		if (nsg > 0 && s->type == SHT_NOBITS
		    && os[sg[nsg-1].sec].addr + sg[nsg-1].memsz == s->addr) {
			g = &sg[nsg-1];
			g->memsz += s->size;
			g->flags |= t;
			continue;
		}
		g = &sg[nsg++];
		g->sec = ord[i];
		g->memsz = s->size;
		g->flags = t;
	}
}

/*
 * Translate the relocations of COFF section k into the ELF relocation
 * section relsec[k].  COFF holds the addend in place, computed against the
 * symbol's COFF value (and, when PC-relative, against the field's address);
 * rebase it to ELF's section-relative symbols.  The i386 REL form keeps the
 * addend in place; the m68k RELA form moves it into r_addend.
 */
dorel(k)
{
	struct buf	rb;
	struct osec	*s;
	long		o, rp, i, n, si;
	unsigned	rva, roff, a, et;
	unsigned char	*f;
	int		ct, w, pc;

	o = shoff(k);
	rp = u32(o+24);
	n = u16(o+32);
	need(rp, n*RELSZ);
	s = &os[secmap[k]];
	rb.b = NULL;
	rb.len = rb.cap = 0;
	for (i = 0; i < n; i++) {
		o = rp + i*RELSZ;
		rva = u32(o);
		si = u32(o+4);
		ct = u16(o+8);
		w = 4;
		pc = 0;
		if (!m68k) {
			switch (ct) {
			case R_DIR32:	et = R_386_32; break;
			case R_PCRLONG:	et = R_386_PC32; pc = 1; break;
			default:	fatal("unsupported COFF reloc type", (char *)0);
			}
		} else {
			switch (ct) {
			case R_DIR32:
			case R_RELLONG:	et = R_68K_32; break;
			case R_RELWORD:	et = R_68K_16; w = 2; break;
			case R_RELBYTE:	et = R_68K_8; w = 1; break;
			case R_PCRLONG:	et = R_68K_PC32; pc = 1; break;
			case R_PCRWORD:	et = R_68K_PC16; w = 2; pc = 1; break;
			case R_PCRBYTE:	et = R_68K_PC8; w = 1; pc = 1; break;
			default:	fatal("unsupported COFF reloc type", (char *)0);
			}
		}
		roff = rva - secbase[k];
		if (roff + w > s->size || s->type == SHT_NOBITS)
			fatal("relocation outside its section", (char *)0);
		if (si < 0 || si >= nsyms || symmap[si] < 0)
			fatal("relocation against an unconverted symbol",
				(char *)0);
		f = out.b + s->off + roff;
		a = getfld(f, w) - symbase[si];
		if (pc)
			a += rva;
		if (m68k) {
			putfld(f, w, 0);
			bufput32(&rb, roff);
			bufput32(&rb, ((unsigned)symmap[si]<<8) | et);
			bufput32(&rb, a);
		} else {
			putfld(f, w, a);
			bufput32(&rb, roff);
			bufput32(&rb, ((unsigned)symmap[si]<<8) | et);
		}
	}
	s = &os[relsec[k]];
	s->img = rb.b;
	s->imglen = rb.len;
	s->src = 0;
	s->size = rb.len;
}

/* Copy section i's bytes into the output. */
emitsec(i)
{
	struct osec	*s;
	long		pad;

	s = &os[i];
	if (s->type == SHT_NOBITS || s->img == NULL) {
		s->off = out.len;
		if (s->load)		/* keep p_offset = p_vaddr mod PGSZ */
			s->off += (s->addr - out.len) & (PGSZ-1);
		return;
	}
	if (s->load)
		pad = (s->addr - out.len) & (PGSZ-1);
	else
		pad = (-out.len) & 3;
	bufadd(&out, (unsigned char *)NULL, pad);
	s->off = out.len;
	if (s->src < 0 || s->src + (long)s->size > s->imglen)
		fatal("truncated or corrupt COFF file", (char *)0);
	bufadd(&out, s->img + s->src, (long)s->size);
}

main(argc, argv)
char	**argv;
{
	char		*inf, *outf, nm[16], rname[32];
	FILE		*fp;
	int		i, a, firstrel, isym, istr, ishstr, firstglobal;
	int		libsec;
	unsigned	entry, fflags;
	unsigned char	*p;
	long		o;

	progname = argv[0];
	for (a = 1; a < argc && argv[a][0] == '-'; a++) {
		if (strcmp(argv[a], "-u") == 0)
			uflag = 1;
		else if (strcmp(argv[a], "-x") == 0)
			xflag = 1;
		else if (strcmp(argv[a], "-l") == 0)
			lflag = 1;
		else if (strcmp(argv[a], "-R") == 0 && a+1 < argc)
			root = argv[++a];
		else
			break;
	}
	if (argc - a != 2) {
		fprintf(stderr, "usage: %s [-u] [-x] [-l] [-R root] in out\n",
			progname);
		exit(2);
	}
	inf = argv[a];
	outf = argv[a+1];

	cf = mainimg = slurp(inf, &mainlen);
	cflen = mainlen;
	if (cflen < FILHSZ)
		fatal("not a COFF file", (char *)0);
	if ((cf[0] | (cf[1]<<8)) == I386MAGIC)
		bigend = m68k = 0;
	else if (((cf[0]<<8) | cf[1]) == M68MAGIC)
		bigend = m68k = 1;
	else
		fatal("not an i386 or m68k COFF file (bad magic)", (char *)0);

	nscns = u16(2L);
	symptr = u32(8L);
	nsyms = u32(12L);
	fflags = u16(18L);
	scnoff = FILHSZ + u16(16L);
	entry = u16(16L) >= AOUTSZ ? u32(FILHSZ + 16L) : 0;
	isexec = (fflags & F_EXEC) || xflag;
	if (nscns >= MAXSEC)
		fatal("too many sections", (char *)0);
	need(scnoff, (long)nscns*SCNHSZ);
	need(symptr, nsyms*SYMESZ);

	/*
	 * Sections: [0] null, [1..nscns] the COFF sections, libraries,
	 * relocations, .symtab, .strtab, .shstrtab.
	 */
	stradd(&shstr, "");
	stradd(&strtab, "");
	newsec((char *)0, 0, 0, 0, 0);
	libsec = 0;
	for (i = 1; i <= nscns; i++) {
		o = shoff(i);
		memcpy(nm, (char *)cf + o, SYMNMLEN);
		nm[SYMNMLEN] = 0;
		secmap[i] = coffsec(o, nm);
		secbase[i] = u32(o+12);		/* s_vaddr */
		if (strcmp(nm, ".lib") == 0 || u32(o+36) == STYP_LIB)
			libsec = i;
	}
	if (lflag && isexec && libsec)
		loadlibs(shoff(libsec));
	firstrel = nos;
	for (i = 1; i <= nscns; i++) {
		relsec[i] = 0;
		o = shoff(i);
		if (isexec || u16(o+32) == 0)
			continue;
		memcpy(nm, (char *)cf + o, SYMNMLEN);
		nm[SYMNMLEN] = 0;
		sprintf(rname, "%s%s", m68k ? ".rela" : ".rel", nm);
		relsec[i] = newsec(rname, m68k ? SHT_RELA : SHT_REL, 0, 0, 0);
		os[relsec[i]].info = secmap[i];
		os[relsec[i]].entsz = m68k ? 12 : 8;
	}
	isym = newsec(".symtab", SHT_SYMTAB, 0, 0, 0);
	istr = newsec(".strtab", SHT_STRTAB, 0, 0, 0);
	ishstr = newsec(".shstrtab", SHT_STRTAB, 0, 0, 0);
	for (i = firstrel; i < isym; i++)
		os[i].link = isym;

	/*
	 * MWC i386 COFF gives addresses as offsets in the combined
	 * text+data+bss image; a section's base is its section symbol's value.
	 */
	symmap = (int *)malloc((nsyms ? (int)nsyms : 1) * sizeof(int));
	symbase = (unsigned *)malloc((nsyms ? (int)nsyms : 1) * sizeof(unsigned));
	if (symmap == NULL || symbase == NULL)
		fatal("out of memory", (char *)0);
	for (o = 0; o < nsyms; o++) {
		symmap[o] = -1;
		symbase[o] = 0;
	}
	if (!m68k) {
		for (o = 0; o < nsyms; o += 1 + cf[symptr + o*SYMESZ + 17]) {
			char	sn[260];
			long	so;
			int	k;

			so = symptr + o*SYMESZ;
			k = s16(so+12);
			symname(so, sn);
			if (cf[so+16] == C_STAT && k >= 1 && k <= nscns
			    && issecname(sn, k))
				secbase[k] = u32(so+8);
		}
	}

	/* symbols: null, section symbols, locals, then globals */
	esyment((char *)0, 0, 0, SHN_UNDEF, STB_LOCAL, STT_NOTYPE);
	if (!isexec)
		for (i = 1; i <= nscns; i++)
			secsym[i] = esyment((char *)0, 0, 0, secmap[i],
				STB_LOCAL, STT_SECTION);
	for (a = 0; a < 2; a++) {
		addsyms(a, secmap, 1);
		for (i = 0; i < nlib; i++) {
			cf = libimg[i];
			cflen = liblen[i];
			addsyms(a, libmap[i], 0);
		}
		cf = mainimg;
		cflen = mainlen;
		if (a == 0)
			firstglobal = nesym;
	}
	os[isym].img = esym.b;
	os[isym].imglen = os[isym].size = esym.len;
	os[isym].link = istr;
	os[isym].info = firstglobal;
	os[isym].entsz = SYMSZ;
	os[istr].img = strtab.b;
	os[istr].imglen = os[istr].size = strtab.len;
	os[istr].align = 1;
	os[ishstr].img = shstr.b;
	os[ishstr].imglen = os[ishstr].size = shstr.len;
	os[ishstr].align = 1;

	if (isexec)
		mksegs();

	/* ELF header, program headers (filled in later), then sections */
	bufadd(&out, (unsigned char *)NULL, (long)EHSZ);
	p = out.b;
	p[0] = 0x7f; p[1] = 'E'; p[2] = 'L'; p[3] = 'F';
	p[4] = 1;			/* ELFCLASS32 */
	p[5] = bigend ? 2 : 1;		/* ELFDATA2MSB : ELFDATA2LSB */
	p[6] = EV_CURRENT;
	put16(p+16, isexec ? ET_EXEC : ET_REL);
	put16(p+18, m68k ? EM_68K : EM_386);
	put32(p+20, EV_CURRENT);
	put32(p+24, isexec ? entry : 0);
	put32(p+28, nsg ? EHSZ : 0);	/* e_phoff */
	put16(p+40, EHSZ);
	put16(p+42, nsg ? PHSZ : 0);
	put16(p+44, nsg);
	put16(p+46, SHSZ);
	put16(p+48, nos);
	put16(p+50, ishstr);
	bufadd(&out, (unsigned char *)NULL, (long)nsg*PHSZ);

	for (i = 1; i < firstrel; i++)
		emitsec(i);
	for (i = 1; i <= nscns; i++)
		if (relsec[i])
			dorel(i);
	for (i = firstrel; i < nos; i++)
		emitsec(i);

	for (i = 0; i < nsg; i++) {
		struct osec	*s;

		s = &os[sg[i].sec];
		p = out.b + EHSZ + i*PHSZ;
		put32(p, PT_LOAD);
		put32(p+4, (unsigned)s->off);
		put32(p+8, s->addr);
		put32(p+12, s->paddr);
		put32(p+16, s->type == SHT_NOBITS ? 0 : s->size);
		put32(p+20, sg[i].memsz);
		put32(p+24, sg[i].flags);
		put32(p+28, PGSZ);
	}

	bufadd(&out, (unsigned char *)NULL, (-out.len) & 3);
	put32(out.b + 32, (unsigned)out.len);	/* e_shoff */
	for (i = 0; i < nos; i++) {
		struct osec	*s;

		s = &os[i];
		o = bufadd(&out, (unsigned char *)NULL, (long)SHSZ);
		p = out.b + o;
		if (i == 0)
			continue;
		put32(p, (unsigned)s->name);
		put32(p+4, s->type);
		put32(p+8, s->flags);
		put32(p+12, s->addr);
		put32(p+16, (unsigned)s->off);
		put32(p+20, s->size);
		put32(p+24, s->link);
		put32(p+28, s->info);
		put32(p+32, s->align);
		put32(p+36, s->entsz);
	}

	fp = fopen(outf, "wb");
	if (fp == NULL)
		fatal("cannot open %s", outf);
	if (fwrite((char *)out.b, 1, (int)out.len, fp) != out.len)
		fatal("write error", (char *)0);
	fclose(fp);
	return (0);
}

/* end of coff2elf.c */
