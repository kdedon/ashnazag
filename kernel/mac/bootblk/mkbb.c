/*
 * mkbb -- prepare the kernel and boot blocks for a direct Mac boot.
 *
 *	usage:	mkbb flat in.elf out.bin
 *		mkbb patch bootblk.bin in.elf volume [cmdline]
 *		mkbb check disk.img in.elf
 *
 * flat	 writes the ELF's PT_LOAD segments as one image from the lowest
 *	 address, zero-filled between segments, padded to 512 bytes.
 * patch the flat image must already be in the HFS volume as a file; finds
 *	 it (one run of allocated blocks), writes offset, length, link
 *	 address, end, entry, checksum and the command line into a copy of
 *	 the boot blocks and stores that in blocks 0-1 of the volume.
 * check finds the Apple_HFS partition of a partitioned disk image and
 *	 verifies its boot blocks and the image they point to.
 *
 * Accesses all structures by explicit byte offsets.
 *
 * K&R C.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	BBSIZE		1024
#define	BLK		512

/* boot-block parameters, as in bootblk.s */
#define	P_MAGIC		0x8C
#define	P_KOFS		0x90
#define	P_KLEN		0x94
#define	P_KLOAD		0x98
#define	P_KEND		0x9C
#define	P_KENTRY	0xA0
#define	P_KSUM		0xA4
#define	CMDOFF		(BBSIZE - 128)
#define	CMDMAX		128

/* ELF32 */
#define	PT_LOAD		1

char	*progname;

/* the flat image and what the boot blocks need to know about it */
unsigned char	*flat;
unsigned long	flatlen, kload, kend, kentry, ksum;

void
die(s, a)
char *s, *a;
{
	fprintf(stderr, "%s: ", progname);
	fprintf(stderr, s, a);
	fprintf(stderr, "\n");
	exit(1);
}

unsigned long
be32(p)
unsigned char *p;
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
	    (unsigned long)p[2] << 8 | p[3];
}

unsigned
be16(p)
unsigned char *p;
{
	return p[0] << 8 | p[1];
}

void
put32(p, v)
unsigned char *p;
unsigned long v;
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

/* whole file into memory */
unsigned char *
slurp(name, lenp)
char *name;
long *lenp;
{
	FILE *f;
	unsigned char *b;
	long n;

	if ((f = fopen(name, "rb")) == NULL)
		die("cannot open %s", name);
	fseek(f, 0L, 2);
	n = ftell(f);
	fseek(f, 0L, 0);
	if (n < 0)
		die("cannot size %s", name);
	if ((b = (unsigned char *)malloc(n ? n : 1)) == NULL)
		die("out of memory for %s", name);
	if (fread(b, 1, n, f) != n)
		die("cannot read %s", name);
	fclose(f);
	*lenp = n;
	return b;
}

/* the ELF's PT_LOAD segments -> flat, kload, kend, kentry, ksum */
void
flatten(name)
char *name;
{
	unsigned char *e, *ph;
	long elen;
	unsigned long phoff, phentsz, phnum, i, va, pa, off, fsz, msz, hi;

	e = slurp(name, &elen);
	if (elen < 52 || memcmp(e, "\177ELF", 4) || e[4] != 1 || e[5] != 2 ||
	    be16(e + 16) != 2 || be16(e + 18) != 4)
		die("%s: not a big-endian ELF32 m68k executable", name);
	kentry = be32(e + 24);
	phoff = be32(e + 28);
	phentsz = be16(e + 42);
	phnum = be16(e + 44);
	if (phentsz < 32 || phoff + phnum * phentsz > elen)
		die("%s: bad program headers", name);
	kload = 0xFFFFFFFF;
	hi = kend = 0;
	for (i = 0; i < phnum; i++) {
		ph = e + phoff + i * phentsz;
		if (be32(ph) != PT_LOAD || be32(ph + 20) == 0)
			continue;
		va = be32(ph + 8);
		pa = be32(ph + 12);
		fsz = be32(ph + 16);
		msz = be32(ph + 20);
		if (fsz > msz)
			die("%s: segment file size above memory size", name);
		if (va != pa)
			die("%s: segment with VA != PA", name);
		if (va < kload)
			kload = va;
		if (va + fsz > hi)
			hi = va + fsz;
		if (va + msz > kend)
			kend = va + msz;
	}
	if (hi <= kload)
		die("%s: nothing to load", name);
	if (kentry < kload || kentry >= hi)
		die("%s: entry outside the image", name);
	flatlen = (hi - kload + BLK - 1) / BLK * BLK;
	if ((flat = (unsigned char *)calloc(flatlen, 1)) == NULL)
		die("out of memory for %s", name);
	for (i = 0; i < phnum; i++) {
		ph = e + phoff + i * phentsz;
		if (be32(ph) != PT_LOAD || be32(ph + 20) == 0)
			continue;
		off = be32(ph + 4);
		fsz = be32(ph + 16);
		if (off + fsz > elen)
			die("%s: segment past the end of the file", name);
		memcpy(flat + be32(ph + 8) - kload, e + off, fsz);
	}
	free(e);
	ksum = 0;
	for (i = 0; i < flatlen; i += 4)
		ksum = (ksum + be32(flat + i)) & 0xFFFFFFFF;
}

void
report()
{
	printf("image: load 0x%lx length 0x%lx end 0x%lx entry 0x%lx sum 0x%08lx\n",
	    kload, flatlen, kend, kentry, ksum);
}

/*
 * Why [ofs, ofs+len) of HFS volume v is not a run of allocation blocks
 * marked in use, or NULL.  v holds at least the MDB.
 */
char *
inuse(v, vlen, ofs, len)
unsigned char *v;
long vlen, ofs, len;
{
	unsigned char *mdb;
	long ab0, absz, nab, bm, b, e;

	mdb = v + BBSIZE;
	ab0 = be16(mdb + 28) * (long)BLK;
	nab = be16(mdb + 18);
	absz = be32(mdb + 20);
	bm = be16(mdb + 14) * (long)BLK;
	if (absz == 0 || absz % BLK)
		return "bad allocation block size";
	if (ofs < ab0 || (ofs - ab0) % absz || len <= 0 ||
	    ofs + len > ab0 + nab * absz || ab0 + nab * absz > vlen)
		return "not whole allocation blocks inside the allocation area";
	b = (ofs - ab0) / absz;
	e = (ofs + len - ab0 + absz - 1) / absz;
	if (bm + (e + 7) / 8 > vlen)
		return "volume bitmap outside the volume";
	for (; b < e; b++)
		if ((v[bm + b / 8] & 0x80 >> b % 8) == 0)
			return "in free allocation blocks";
	return NULL;
}

/* offset of the flat image in v: allocated, block aligned */
long
findimg(v, vlen)
unsigned char *v;
long vlen;
{
	long o;

	for (o = BBSIZE; o + (long)flatlen <= vlen; o += BLK)
		if (memcmp(v + o, flat, BLK) == 0 &&
		    memcmp(v + o, flat, flatlen) == 0 &&
		    inuse(v, vlen, o, (long)flatlen) == NULL)
			return o;
	return -1;
}

void
bbcheck(bb, what)
unsigned char *bb;
char *what;
{
	if (be16(bb) != 0x4C4B || be16(bb + 2) != 0x6000)
		die("%s: no 'LK' boot blocks with a bra.w entry", what);
	if (bb[6] != 0x44 && (bb[6] & 0xC0) != 0xC0)
		die("%s: bbVersion does not make the ROM run the code", what);
	if (memcmp(bb + P_MAGIC, "UxBB", 4))
		die("%s: not our boot blocks (no UxBB)", what);
}

void
patch(bbname, vname, cmd)
char *bbname, *vname, *cmd;
{
	unsigned char *bb, *v;
	long bblen, vlen, o;
	FILE *f;

	bb = slurp(bbname, &bblen);
	if (bblen != BBSIZE)
		die("%s: not 1024 bytes", bbname);
	bbcheck(bb, bbname);
	if (strlen(cmd) >= CMDMAX)
		die("command line longer than 127 bytes: %s", cmd);
	v = slurp(vname, &vlen);
	if (vlen < 3 * BLK || be16(v + 2 * BLK) != 0x4244)
		die("%s: no HFS master directory block", vname);
	if ((o = findimg(v, vlen)) < 0)
		die("%s: kernel image not found as one contiguous allocated run", vname);
	put32(bb + P_KOFS, (unsigned long)o);
	put32(bb + P_KLEN, flatlen);
	put32(bb + P_KLOAD, kload);
	put32(bb + P_KEND, kend);
	put32(bb + P_KENTRY, kentry);
	put32(bb + P_KSUM, ksum);
	memset(bb + CMDOFF, 0, CMDMAX);
	strcpy((char *)bb + CMDOFF, cmd);
	if ((f = fopen(vname, "r+b")) == NULL)
		die("cannot write %s", vname);
	if (fwrite(bb, 1, BBSIZE, f) != BBSIZE || fclose(f))
		die("cannot write %s", vname);
	printf("boot blocks: image at volume offset 0x%lx (block %ld), cmdline \"%s\"\n",
	    o, o / BLK, cmd);
}

void
check(dname)
char *dname;
{
	unsigned char *d, *e, *bb, *mdb;
	char *why;
	long dlen, i, n, st, cnt, p, ofs, len, bad;
	int hfs, drv;

	d = slurp(dname, &dlen);
	if (dlen < 2 * BLK || be16(d) != 0x4552)
		die("%s: no driver descriptor map", dname);
	if (be32(d + 4) * BLK != dlen)
		die("%s: DDM block count is not the image size", dname);
	hfs = drv = 0;
	st = cnt = 0;
	n = 1;
	for (i = 1; i <= n && (i + 1) * BLK <= dlen; i++) {
		e = d + i * BLK;
		if (be16(e) != 0x504D)
			die("%s: bad partition map entry", dname);
		n = be32(e + 4);
		printf("  map %ld: start %8lu count %8lu  %-16.32s %.32s\n", i,
		    be32(e + 8), be32(e + 12), (char *)e + 16, (char *)e + 48);
		if (strncmp((char *)e + 48, "Apple_Driver", 32) == 0)
			drv++;
		if (strncmp((char *)e + 48, "Apple_HFS", 32) == 0) {
			hfs++;
			st = be32(e + 8);
			cnt = be32(e + 12);
		}
	}
	if (hfs != 1 || drv < 1)
		die("%s: want one Apple_HFS and an Apple_Driver partition", dname);
	if (cnt < 3 || (st + cnt) * BLK > dlen)
		die("%s: HFS partition outside the image or too small", dname);
	p = st * BLK;
	bb = d + p;
	bbcheck(bb, dname);
	mdb = bb + BBSIZE;
	if (be16(mdb) != 0x4244)
		die("%s: no HFS master directory block", dname);
	ofs = be32(bb + P_KOFS);
	len = be32(bb + P_KLEN);
	printf("boot blocks: bbVersion 0x%04x, image offset 0x%lx length 0x%lx load 0x%lx end 0x%lx entry 0x%lx sum 0x%08lx\n",
	    be16(bb + 6), ofs, len, be32(bb + P_KLOAD), be32(bb + P_KEND),
	    be32(bb + P_KENTRY), be32(bb + P_KSUM));
	printf("cmdline \"%.127s\"\n", (char *)bb + CMDOFF);
	bad = 0;
	if ((why = inuse(bb, cnt * BLK, ofs, len)) != NULL) {
		printf("  image %s\n", why);
		bad++;
	}
	if (len != flatlen || be32(bb + P_KLOAD) != kload ||
	    be32(bb + P_KEND) != kend || be32(bb + P_KENTRY) != kentry ||
	    be32(bb + P_KSUM) != ksum) {
		printf("  parameters differ from the ELF\n");
		bad++;
	} else if (ofs + len > cnt * BLK || memcmp(d + p + ofs, flat, len)) {
		printf("  image on disk differs from the ELF\n");
		bad++;
	}
	if (bb[CMDOFF + CMDMAX - 1] != 0) {
		printf("  command line not terminated\n");
		bad++;
	}
	if (bad)
		die("%s: check failed", dname);
	printf("%s: boot blocks and kernel image OK\n", dname);
}

int
main(argc, argv)
int argc;
char **argv;
{
	FILE *f;

	progname = argv[0];
	if (argc == 4 && strcmp(argv[1], "flat") == 0) {
		flatten(argv[2]);
		if ((f = fopen(argv[3], "wb")) == NULL ||
		    fwrite(flat, 1, flatlen, f) != flatlen || fclose(f))
			die("cannot write %s", argv[3]);
		report();
	} else if ((argc == 5 || argc == 6) && strcmp(argv[1], "patch") == 0) {
		flatten(argv[3]);
		report();
		patch(argv[2], argv[4], argc == 6 ? argv[5] : "");
	} else if (argc == 4 && strcmp(argv[1], "check") == 0) {
		flatten(argv[3]);
		report();
		check(argv[2]);
	} else {
		fprintf(stderr, "usage: %s flat in.elf out.bin\n", progname);
		fprintf(stderr, "       %s patch bootblk.bin in.elf volume [cmdline]\n", progname);
		fprintf(stderr, "       %s check disk.img in.elf\n", progname);
		exit(2);
	}
	exit(0);
}
