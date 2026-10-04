/*
 * mkfix -- emit a minimal but valid Coherent i386 COFF object, as a test
 * fixture for coff2elf (until the host-built cc2-i386 produces real COFF).
 * The object defines a global `_main` whose code is `mov eax,42; ret`, so the
 * converted+linked program exits 42.  With -r it also emits a .data string and
 * a R_DIR32 relocation in .text referencing it, to exercise the reloc path.
 *
 * Plain host C (test scaffolding only -- not part of the shipped toolchain).
 *	usage: mkfix [-r] out.coff
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char	buf[4096];
int		len;

put8(v)	 { buf[len++] = v; }
put16(v) { buf[len++] = v; buf[len++] = v>>8; }
put32(v) { buf[len++] = v; buf[len++] = v>>8; buf[len++] = v>>16; buf[len++] = v>>24; }

/* write an 8-byte COFF inline name */
name8(s)
char	*s;
{
	int	i, n;

	n = strlen(s);
	for (i = 0; i < 8; i++)
		put8(i < n ? s[i] : 0);
}

/* one 40-byte section header */
scnhdr(nm, size, scnptr, relptr, nreloc, flags)
char	*nm;
unsigned size, scnptr, relptr, flags;
{
	name8(nm);
	put32(0);		/* s_paddr  */
	put32(0);		/* s_vaddr  */
	put32(size);		/* s_size   */
	put32(scnptr);		/* s_scnptr */
	put32(relptr);		/* s_relptr */
	put32(0);		/* s_lnnoptr */
	put16(nreloc);		/* s_nreloc */
	put16(0);		/* s_nlnno  */
	put32(flags);		/* s_flags  */
}

/* one 18-byte symbol entry (inline name) */
syment(nm, value, scnum, sclass)
char	*nm;
unsigned value;
{
	name8(nm);
	put32(value);		/* n_value  */
	put16(scnum);		/* n_scnum  */
	put16(0);		/* n_type   */
	put8(sclass);		/* n_sclass */
	put8(0);		/* n_numaux */
}

#define	STYP_TEXT	0x20
#define	STYP_DATA	0x40
#define	STYP_BSS	0x80
#define	C_EXT		2
#define	C_STAT		3
#define	R_DIR32		0x06

main(argc, argv)
char	**argv;
{
	int	rflag, a;
	FILE	*fp;
	unsigned txtoff, txtsz, datoff, datsz, txtrel, ntxtrel, symptr;

	rflag = 0;
	a = 1;
	if (a < argc && strcmp(argv[a], "-r") == 0) {
		rflag = 1;
		a++;
	}
	if (argc - a != 1) {
		fprintf(stderr, "usage: mkfix [-r] out.coff\n");
		exit(2);
	}

	/*
	 * Lay out offsets.  Header(20) + 3 section headers(3*40=120) = 140.
	 * .text code follows, then (for -r) a .data string, then relocs,
	 * then the symbol table, then a 4-byte (empty) string table.
	 */
	txtoff = 140;
	if (rflag) {
		/* mov eax, [imm32]; ret  -> A1 <reloc32> C3  (5+1 = 6 bytes) */
		txtsz = 6;
		datoff = txtoff + txtsz;		/* .data: "Hi" + NUL */
		datsz = 3;
		txtrel = datoff + datsz;		/* one RELOC (10 bytes) */
		ntxtrel = 1;
		symptr = txtrel + 10;
	} else {
		/* mov eax, 42; ret -> B8 2A 00 00 00 C3 (6 bytes) */
		txtsz = 6;
		datoff = 0;
		datsz = 0;
		txtrel = 0;
		ntxtrel = 0;
		symptr = txtoff + txtsz;
	}

	/* --- file header (20) --- */
	put16(0x14C);			/* f_magic  */
	put16(3);			/* f_nscns  */
	put32(0);			/* f_timdat */
	put32(symptr);			/* f_symptr */
	put32(rflag ? 4 : 1);		/* f_nsyms (-r: .data sect sym + _main + ...) */
	put16(0);			/* f_opthdr */
	put16(0);			/* f_flags  */

	/* --- section headers (3 x 40) --- */
	scnhdr(".text", txtsz, txtoff, txtrel, ntxtrel, STYP_TEXT);
	scnhdr(".data", datsz, datoff, 0, 0, STYP_DATA);
	scnhdr(".bss",  0,     0,      0, 0, STYP_BSS);

	/* --- .text code --- */
	if (rflag) {
		put8(0xA1); put32(0); put8(0xC3);	/* mov eax,[disp32]; ret */
	} else {
		put8(0xB8); put32(42); put8(0xC3);	/* mov eax,42; ret */
	}

	/* --- .data + relocs (only for -r) --- */
	if (rflag) {
		put8('H'); put8('i'); put8(0);		/* .data: "Hi\0" */
		/* RELOC: r_vaddr=1 (the disp32 in .text), r_symndx=.data sym, R_DIR32 */
		put32(1);			/* r_vaddr  */
		put32(1);			/* r_symndx -> symbol index 1 (.data) */
		put16(R_DIR32);			/* r_type   */
	}

	/* --- symbol table --- */
	if (rflag) {
		syment(".data", 0, 2, C_STAT);	/* idx 0: .data section symbol */
		syment(".data", 0, 2, C_STAT);	/* idx 1: alias referenced by reloc */
		syment("_main", 0, 1, C_EXT);	/* idx 2 */
		syment(".text", 0, 1, C_STAT);	/* idx 3 */
	} else {
		syment("_main", 0, 1, C_EXT);	/* idx 0 */
	}

	/* --- string table: just the 4-byte length (no long names) --- */
	put32(4);

	fp = fopen(argv[a], "wb");
	if (fp == NULL) {
		perror("mkfix");
		exit(1);
	}
	fwrite((char *)buf, 1, len, fp);
	fclose(fp);
	return (0);
}
