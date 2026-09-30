/*
 * elf2coff -- wrap a fully linked, identity-mapped m68k ELF kernel as an
 * A/UX COFF kernel that A/UX Startup's `launch' will load.
 *
 *	usage:	elf2coff [-p addr] [-s sym] [-e sym] in.elf out.coff
 *		elf2coff -c file.coff
 *
 *	-p addr	physical address of the pstart section (default 0x4000)
 *	-s sym	first byte of the pstart code in the ELF (default aux_pstart)
 *	-e sym	end of the pstart code (default aux_pstart_end)
 *	-c	check an existing COFF file against launch's rules only
 *
 * Output: file magic 0x150, a.out magic 0410, no symbols; sections pstart
 * (the bytes between the two symbols, placed at -p, which is the entry),
 * then .text, .data, .bss at their ELF addresses with s_paddr = s_vaddr.
 * launch puts the info block at pstart - 0x400 and the low-memory copy at
 * pstart - 0x4000; both must stay clear of every section.
 *
 * Reads and writes by explicit byte offsets, never by struct overlay.
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

#define	M68MAGIC  0x150
#define	ZMAGIC_NS 0410		/* a.out magic of A/UX /unix	*/

/* f_flags */
#define	F_RELFLG	0x1
#define	F_EXEC		0x2
#define	F_LNNO		0x4
#define	F_LSYMS		0x8
#define	F_AR32W		0x200

/* s_flags */
#define	STYP_NOTLOAD	0x1F	/* DSECT NOLOAD GROUP PAD COPY	*/
#define	STYP_TEXT	0x20
#define	STYP_DATA	0x40
#define	STYP_BSS	0x80

/* ELF32 */
#define	EHSZ		52
#define	ET_EXEC		2
#define	EM_68K		4
#define	PT_LOAD		1
#define	SHT_SYMTAB	2
#define	SHT_NOBITS	8
#define	SHF_ALLOC	0x2

/* launch's layout */
#define	MAXSCN		8	/* its section-header array	*/
#define	INFOOFF		0x400	/* info block below pstart	*/
#define	INFOMAX		0x400	/* 0xBE + drive-queue copy	*/
#define	LOWOFF		0x4000	/* low-memory copy below pstart	*/
#define	LOWSZ		0x2000
#define	IDMAX		0x40000000 /* kernel runs VA = PA below	*/

#define	MAXSEC		16

char	*progname;

unsigned char	*img;		/* the file being read		*/
long		imglen;

/* One output section. */
struct sec {
	char		name[9];
	unsigned	addr, size, flags;
	long		src;		/* offset in img, -1: none	*/
	long		scnptr;
};

struct sec	sc[MAXSEC];
int		nsc;
unsigned	entry;

unsigned	u16(), u32();
unsigned char	*slurp();
int		errs;

fatal(s, a)
char	*s, *a;
{
	fprintf(stderr, "%s: ", progname);
	fprintf(stderr, s, a);
	fprintf(stderr, "\n");
	exit(1);
}

need(o, n)
long	o, n;
{
	if (o < 0 || n < 0 || o + n > imglen)
		fatal("truncated or corrupt input", (char *)0);
}

unsigned
u16(o)
long	o;
{
	need(o, 2L);
	return ((img[o]<<8) | img[o+1]);
}

unsigned
u32(o)
long	o;
{
	need(o, 4L);
	return (((unsigned)img[o]<<24) | (img[o+1]<<16) | (img[o+2]<<8)
		| img[o+3]);
}

put16(p, v)
unsigned char	*p;
unsigned	v;
{
	p[0] = v>>8; p[1] = v;
}

put32(p, v)
unsigned char	*p;
unsigned	v;
{
	p[0] = v>>24; p[1] = v>>16; p[2] = v>>8; p[3] = v;
}

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

/* A NUL-terminated string at offset o. */
char *
strat(o)
long	o;
{
	long	e;

	for (e = o; ; e++) {
		need(e, 1L);
		if (img[e] == 0)
			break;
	}
	return ((char *)img + o);
}

/*
 * ELF input.
 */
long	shoff;
int	shnum, shentsz;

long
shdr(i)
{
	return (shoff + (long)i * shentsz);
}

/* Value of a symbol from the ELF symbol table. */
unsigned
elfsym(name)
char	*name;
{
	int	i;
	long	h, st, o, n;

	for (i = 0; i < shnum; i++) {
		h = shdr(i);
		if (u32(h+4) != SHT_SYMTAB)
			continue;
		st = u32(shdr((int)u32(h+24)) + 16);
		n = u32(h+20) / 16;
		for (o = u32(h+16); n > 0; n--, o += 16)
			if (u32(o) && u16(o+14) != 0 && strcmp(strat(st + u32(o)), name) == 0)
				return (u32(o+4));
	}
	fatal("symbol %s not in the ELF symbol table", name);
	return (0);
}

readelf(name, paddr, ssym, esym)
char		*name, *ssym, *esym;
unsigned	paddr;
{
	long		h, ph, shstr;
	int		i, phnum;
	unsigned	s, e, fl;
	char		*nm;
	struct sec	*sp, *tp;

	img = slurp(name, &imglen);
	need(0L, (long)EHSZ);
	if (memcmp((char *)img, "\177ELF", 4) != 0 || img[4] != 1
	    || img[5] != 2)
		fatal("%s: not a big-endian ELF32 file", name);
	if (u16(16L) != ET_EXEC || u16(18L) != EM_68K)
		fatal("%s: not an m68k executable", name);

	/* program headers: identity mapped */
	phnum = u16(44L);
	for (i = 0; i < phnum; i++) {
		ph = u32(28L) + (long)i * u16(42L);
		if (u32(ph) == PT_LOAD && u32(ph+8) != u32(ph+12))
			fatal("PT_LOAD with p_vaddr != p_paddr", (char *)0);
	}

	shoff = u32(32L);
	shentsz = u16(46L);
	shnum = u16(48L);
	shstr = u32(shdr((int)u16(50L)) + 16);

	/* pstart: the stub copied out of the ELF */
	s = elfsym(ssym);
	e = elfsym(esym);
	sp = &sc[nsc++];
	strcpy(sp->name, "pstart");
	sp->addr = paddr;
	sp->size = (e - s + 3) & ~3;
	sp->flags = STYP_TEXT;
	sp->src = -1;
	entry = paddr;
	if (e <= s || e - s > INFOMAX)
		fatal("bad pstart code range", (char *)0);

	for (i = 1; i < shnum; i++) {
		h = shdr(i);
		fl = u32(h+8);
		if (!(fl & SHF_ALLOC) || u32(h+20) == 0)
			continue;
		nm = strat(shstr + u32(h));
		if (strcmp(nm, ".text") && strcmp(nm, ".data")
		    && strcmp(nm, ".bss"))
			fatal("unexpected allocated section %s", nm);
		if (nsc >= MAXSEC)
			fatal("too many sections", (char *)0);
		tp = &sc[nsc++];
		strcpy(tp->name, nm);
		tp->addr = u32(h+12);
		tp->size = u32(h+20);
		if (u32(h+4) == SHT_NOBITS) {
			tp->flags = STYP_BSS;
			tp->src = -1;
		} else {
			tp->flags = strcmp(nm, ".text") ? STYP_DATA : STYP_TEXT;
			tp->src = u32(h+16);
			need(tp->src, (long)tp->size);
		}
		if (strcmp(nm, ".text") == 0) {
			if (s < tp->addr || e > tp->addr + tp->size)
				fatal("pstart code is not inside .text", (char *)0);
			sp->src = tp->src + (s - tp->addr);
		}
	}
	if (sp->src < 0)
		fatal("no .text section", (char *)0);
}

struct sec *
named(nm)
char	*nm;
{
	int	i;

	for (i = 0; i < nsc; i++)
		if (strcmp(sc[i].name, nm) == 0)
			return (&sc[i]);
	return (NULL);
}

writecoff(name)
char	*name;
{
	FILE		*fp;
	unsigned char	*h, *p;
	long		hl, off;
	int		i;
	struct sec	*t, *d, *b;

	t = named(".text");
	d = named(".data");
	b = named(".bss");
	if (t == NULL || d == NULL || b == NULL)
		fatal("need .text, .data and .bss", (char *)0);

	/*
	 * launch reads the loaded sections back to back and skips no gap,
	 * so each one must end on 4 bytes where the next one follows.
	 */
	hl = FILHSZ + AOUTSZ + (long)nsc * SCNHSZ;
	off = (hl + 3) & ~3L;
	for (i = 0; i < nsc; i++) {
		if (sc[i].flags == STYP_BSS) {
			sc[i].scnptr = 0;
			continue;
		}
		if (off & 3)
			fatal("the section before %s does not end on 4 bytes", sc[i].name);
		sc[i].scnptr = off;
		off += sc[i].size;
	}
	off = (off + 3) & ~3L;

	h = (unsigned char *)calloc(1, (int)off);
	if (h == NULL)
		fatal("out of memory", (char *)0);

	put16(h, M68MAGIC);
	put16(h+2, nsc);
	put32(h+4, 0);			/* timestamp: reproducible	*/
	put32(h+8, 0);			/* no symbol table		*/
	put32(h+12, 0);
	put16(h+16, AOUTSZ);
	put16(h+18, F_RELFLG|F_EXEC|F_LNNO|F_LSYMS|F_AR32W);

	p = h + FILHSZ;
	put16(p, ZMAGIC_NS);
	put16(p+2, 0);
	put32(p+4, t->size);
	put32(p+8, d->size);
	put32(p+12, b->size);
	put32(p+16, entry);
	put32(p+20, t->addr);
	put32(p+24, d->addr);

	for (i = 0; i < nsc; i++) {
		p = h + FILHSZ + AOUTSZ + (long)i * SCNHSZ;
		strncpy((char *)p, sc[i].name, 8);
		put32(p+8, sc[i].addr);		/* s_paddr		*/
		put32(p+12, sc[i].addr);	/* s_vaddr		*/
		put32(p+16, sc[i].size);
		put32(p+20, (unsigned)sc[i].scnptr);
		put32(p+36, sc[i].flags);
		if (sc[i].src >= 0) {
			need(sc[i].src, (long)sc[i].size);
			memcpy((char *)h + sc[i].scnptr,
			    (char *)img + sc[i].src, (int)sc[i].size);
		}
	}

	fp = fopen(name, "wb");
	if (fp == NULL || fwrite((char *)h, 1, (int)off, fp) != off
	    || fclose(fp) != 0)
		fatal("cannot write %s", name);
}

/*
 * The rules launch applies to a kernel file, checked on the COFF bytes.
 * Addresses in brackets are launch's (text linked at 0x180000).
 */
bad(s, a)
char		*s;
unsigned	a;
{
	printf("  FAIL ");
	printf(s, a);
	printf("\n");
	errs++;
}

ok(s, a)
char		*s;
unsigned	a;
{
	printf("  ok   ");
	printf(s, a);
	printf("\n");
}

/* Ranges [a, a+n) and [b, b+m) overlap. */
overlap(a, n, b, m)
unsigned	a, n, b, m;
{
	return (n && m && a < b + m && b < a + n);
}

checkcoff(name)
char	*name;
{
	long		o, last;
	int		n, i, j, ps, hasmod;
	unsigned	fl, pa, info, low, ent;
	char		nm[9];
	struct sec	*s, *t;

	img = slurp(name, &imglen);
	errs = 0;
	nsc = 0;
	printf("%s:\n", name);
	if (u16(0L) == M68MAGIC)
		ok("f_magic 0x150 [0x180ad8, 0x180e40]", 0);
	else
		bad("f_magic 0x%x, want 0x150 [0x180ad8]", u16(0L));
	n = u16(2L);
	if (n <= MAXSCN)
		ok("%d sections <= 8 (header array fp-0x170..fp-0x30) [0x180e9a]",
		    (unsigned)n);
	else
		bad("%d sections: overruns launch's 8-entry array [0x180e9a]",
		    (unsigned)n);
	if (u16(16L) == AOUTSZ)
		ok("f_opthdr 28: a.out header fills fp-0x30..fp-0x14 [0x180e72]", 0);
	else
		bad("f_opthdr %d, launch reads it into a 28-byte slot [0x180e72]",
		    u16(16L));
	ent = u32((long)FILHSZ + 16);

	o = FILHSZ + u16(16L);
	ps = -1;
	hasmod = 0;
	for (i = 0; i < n && i < MAXSEC; i++, o += SCNHSZ) {
		need(o, (long)SCNHSZ);
		s = &sc[nsc++];
		memcpy(nm, (char *)img + o, 8);
		nm[8] = 0;
		strcpy(s->name, nm);
		s->addr = u32(o+8);		/* s_paddr		*/
		s->size = u32(o+16);
		s->scnptr = u32(o+20);
		s->flags = u32(o+36);
		if (u32(o+8) != u32(o+12))
			printf("  note %s: s_paddr != s_vaddr (ours must be VA = PA)\n",
			    nm);
		if (strcmp(nm, "MODULES") == 0)
			hasmod = 1;
		if (strcmp(nm, "pstart") == 0)
			ps = i;
		if (!(s->flags & (STYP_NOTLOAD|STYP_BSS)))
			need(s->scnptr, (long)s->size);
	}
	if (hasmod)
		printf("  note MODULES section: checked against the slot boards; a mismatch boots newunix [0x180b34, 0x180c82]\n");
	else
		ok("no MODULES section: autoconfig command = OK [0x180b76 -> 0x180d04]", 0);

	if (ps < 0) {
		bad("no pstart section: info and low memory would go to 0 [0x181046]", 0);
		return;
	}
	pa = sc[ps].addr;
	if (pa == 0x500 || pa == 0x2000) {
		bad("pstart at 0x%x takes the old-kernel layout [0x181064]", pa);
		return;
	}
	info = pa - INFOOFF;
	low = pa - LOWOFF;
	ok("pstart at 0x%x", pa);
	printf("         info block -> 0x%x [0x1810a6], low memory -> 0x%x [0x1810c6]\n",
	    info, low);
	if (pa < LOWOFF)
		bad("pstart below 0x4000: low-memory copy wraps [0x1810c6]", 0);
	if (ent >= pa && ent < pa + sc[ps].size)
		ok("entry 0x%x is inside pstart", ent);
	else
		bad("entry 0x%x is not inside pstart", ent);

	for (i = 0; i < nsc; i++) {
		s = &sc[i];
		if (s->flags & STYP_NOTLOAD)
			continue;
		if (overlap(s->addr, s->size, info, INFOMAX))
			bad("section overlaps the info block at 0x%x", info);
		if (overlap(s->addr, s->size, low, LOWSZ))
			bad("section overlaps the low-memory copy at 0x%x", low);
		if (s->addr + s->size > IDMAX || s->addr + s->size < s->addr)
			printf("  note section ends above 1 GB (0x%x)\n",
			    s->addr + s->size);
		for (j = i + 1; j < nsc; j++) {
			t = &sc[j];
			if (!(t->flags & STYP_NOTLOAD)
			    && overlap(s->addr, s->size, t->addr, t->size))
				bad("sections overlap at 0x%x", t->addr);
		}
	}
	ok("no overlap among sections, info block, low-memory copy", 0);

	/* loaded sections in file order: each starts where the last ended */
	last = -1;
	o = -1;
	n = errs;
	for (;;) {
		s = NULL;
		for (j = 0; j < nsc; j++) {
			t = &sc[j];
			if ((t->flags & (STYP_NOTLOAD|STYP_BSS)) || t->scnptr <= last)
				continue;
			if (s == NULL || t->scnptr < s->scnptr)
				s = t;
		}
		if (s == NULL)
			break;
		if (o >= 0 && s->scnptr != o) {
			bad("gap before a section in the file: launch loads it shifted", 0);
			printf("         %s at 0x%lx, previous section ends at 0x%lx\n",
			    s->name, s->scnptr, o);
		}
		last = s->scnptr;
		o = s->scnptr + s->size;
	}
	if (errs == n)
		ok("loaded sections are contiguous in the file [0x18132a, 0x181568]", 0);

	fl = 0;
	for (i = 0; i < nsc; i++) {
		s = &sc[i];
		if (strcmp(s->name, ".text") == 0)
			fl |= 1;
		else if (strcmp(s->name, ".data") == 0)
			fl |= 2;
		else if (strcmp(s->name, ".bss") == 0) {
			fl |= 4;
			if (!(s->flags & STYP_BSS))
				bad(".bss without STYP_BSS: launch would load it", 0);
		} else if (strcmp(s->name, "pstart") && !(s->flags & STYP_NOTLOAD))
			bad("extra loaded section at 0x%x: loaded but not in the info block",
			    s->addr);
		printf("         %-8s paddr 0x%08x size 0x%06x scnptr 0x%06lx flags 0x%x%s\n",
		    s->name, s->addr, s->size, s->scnptr, s->flags,
		    (s->flags & STYP_NOTLOAD) ? " (not loaded)" :
		    (s->flags & STYP_BSS) ? " (bss: not loaded, not zeroed)" : "");
	}
	if (fl == 7)
		ok(".text/.data/.bss present: info +0x8C..+0xAF filled [0x1810ea..0x1811f6]", 0);
	else
		bad("missing .text/.data/.bss: info block section fields stay 0", 0);
	printf("  %s\n", errs ? "REJECTED" : "ACCEPTED");
}

main(argc, argv)
int	argc;
char	**argv;
{
	unsigned	paddr;
	char		*ssym, *esym;
	int		i, cflag;

	progname = argv[0];
	paddr = 0x4000;
	ssym = "aux_pstart";
	esym = "aux_pstart_end";
	cflag = 0;
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-c") == 0)
			cflag = 1;
		else if (i + 1 >= argc)
			fatal("option %s needs a value", argv[i]);
		else if (strcmp(argv[i], "-p") == 0)
			paddr = strtoul(argv[++i], (char **)0, 0);
		else if (strcmp(argv[i], "-s") == 0)
			ssym = argv[++i];
		else if (strcmp(argv[i], "-e") == 0)
			esym = argv[++i];
		else
			fatal("unknown option %s", argv[i]);
	}
	if (cflag) {
		if (i + 1 != argc)
			fatal("usage: elf2coff -c file.coff", (char *)0);
		checkcoff(argv[i]);
		exit(errs ? 1 : 0);
	}
	if (i + 2 != argc)
		fatal("usage: elf2coff [-p addr] [-s sym] [-e sym] in.elf out.coff",
		    (char *)0);
	readelf(argv[i], paddr, ssym, esym);
	writecoff(argv[i+1]);
	checkcoff(argv[i+1]);
	exit(errs ? 1 : 0);
}
