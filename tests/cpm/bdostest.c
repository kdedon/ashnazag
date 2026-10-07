/*
 * bdostest -- the CP/M 3 BDOS on the build host, over a scratch
 * directory, with a scripted console and a memory array as CP/M's.
 *
 *	bdostest dir
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <setjmp.h>
#include "bdos3.h"

#define	MEMSZ	0x400000L
#define	F	0x10000L	/* the FCB */
#define	F2	0x10100L
#define	DMA	0x10200L
#define	BUF	0x10300L
#define	PB	0x10380L
#define	STR	0x10400L

static char *in = "";
static char out[4096];
static int nout, fails, checks;
static jmp_buf wb;
char *b3_mem;
static char dir[1024];

int b3_conin() { return *in ? *in++ : 0x1a; }
int b3_const() { return *in != 0; }
void b3_conout(c) int c; { if (nout < (int)sizeof out - 1) out[nout++] = c; out[nout] = 0; }
void b3_list(c) int c; { }
long b3_bios(fn, d1, d2) long fn, d1, d2; { return 0; }
void b3_wboot() { longjmp(wb, 1); }

static void
check(name, ok)
	char *name;
	int ok;
{
	checks++;
	if (!ok) {
		fails++;
		printf("FAIL %s (output \"%s\")\n", name, out);
	}
}

static long
bdos(fn, d1)
	long fn;
	unsigned long d1;
{
	return cpm_bdos3(fn, d1, 0x1000L);
}

/* a warm boot ends the call; 1 if it came */
static int
booted(fn, d1)
	long fn;
	unsigned long d1;
{
	if (setjmp(wb))
		return 1;
	bdos(fn, d1);
	return 0;
}

static void
fcb(a, drv, n)
	unsigned long a;
	int drv;
	char *n;
{
	memset(B3MEM(a), 0, 36);
	*B3MEM(a) = drv;
	memcpy(B3MEM(a + 1), n, 11);
}

static long
hsize(rel)
	char *rel;
{
	char p[1200];
	struct stat sb;

	sprintf(p, "%s/%s", dir, rel);
	return stat(p, &sb) < 0 ? -1 : (long)sb.st_size;
}

static int
hmode(rel)
	char *rel;
{
	char p[1200];
	struct stat sb;

	sprintf(p, "%s/%s", dir, rel);
	return stat(p, &sb) < 0 ? -1 : sb.st_mode & 07777;
}

static void
hput(rel, n)
	char *rel;
	long n;
{
	char p[1200];
	FILE *fp;

	sprintf(p, "%s/%s", dir, rel);
	fp = fopen(p, "w");
	while (n-- > 0)
		putc((int)(n & 0x7f), fp);
	fclose(fp);
}

static int
nsearch(pat, drvbyte, ents)
	char *pat;
	int drvbyte;
	unsigned char *ents;
{
	int n = 0;
	long r;

	fcb(F2, drvbyte, pat);
	*B3MEM(F2 + 12) = '?';
	for (r = bdos(17L, F2); r == 0; r = bdos(18L, 0L), n++)
		if (ents && n < 4)
			memcpy(ents + 32 * n, B3MEM(DMA), 32);
	return n;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	unsigned char e[128];
	long r, i;

	if (argc != 2)
		return 2;
	umask(022);
	if ((b3_mem = malloc(MEMSZ)) == 0)
		return 1;
	memset(b3_mem, 0, MEMSZ);
	strcpy(b3_root, argv[1]);
	sprintf(dir, "%s/A", b3_root);
	mkdir(b3_root, 0755);
	if (hf_mkdist(dir, "/nonexistent", "\x60\x1a", "exit.68k", 2) < 0) {
		printf("FAIL mkdist\n");
		return 1;
	}
	hput("Mixed.Doc", 10L);
	hput("toolongname.txt", 10L);
	hput(".hid.txt", 10L);
	b3_init(0x2000L, 0x30000L, MEMSZ, 0x8000L);
	bdos(45L, 0xffL);

	check("version", bdos(12L, 0L) == 0x2031);
	bdos(13L, 0L);
	check("drive_a", bdos(25L, 0L) == 0 && bdos(24L, 0L) == 1);

	/* make, write, read */
	fcb(F, 0, "TEST    TXT");
	check("make", bdos(22L, F) == 0 && hsize("test.txt") == 0);
	check("make_exists", bdos(22L, F) == 0x08ff);
	bdos(26L, DMA);
	for (i = 0; i < 3; i++) {
		memset(B3MEM(DMA), 'a' + (int)i, 128);
		check("write", bdos(21L, F) == 0);
	}
	check("close", bdos(16L, F) == 0 && hsize("test.txt") == 384);
	fcb(F, 1, "TEST    TXT");
	check("open", bdos(15L, F) == 0 && B3MEM(F)[15] == 3);
	for (i = 0; i < 3; i++)
		check("read", bdos(20L, F) == 0 && B3MEM(DMA)[127] == 'a' + i);
	check("read_eof", bdos(20L, F) == 1);
	B3MEM(F)[33] = 1;
	check("random_read", bdos(33L, F) == 0 && B3MEM(DMA)[0] == 'b');
	check("size", bdos(35L, F) == 0 && B3MEM(F)[33] == 3);

	/* names */
	check("search_txt", nsearch("????????TXT", 0, e) == 2);
	check("search_entry", nsearch("TEST    TXT", 0, e) == 1 && e[15] == 3 && e[12] == 0 &&
	    e[0] == 0 && (e[16] | e[17]));
	check("sfcb", B3MEM(DMA)[96] == 0x21);
	check("mixed_case", nsearch("MIXED   DOC", 0, 0) == 1);
	check("long_hidden", nsearch("????????TXT", 0, 0) == 2);
	check("sys", nsearch("HID     TXT", 0, e) == 1 && (e[10] & 0x80));
	fcb(F, 0, "TEST    TXT");
	memcpy(B3MEM(F + 17), "NEW     TXT", 11);
	check("rename", bdos(23L, F) == 0 && hsize("new.txt") == 384 && hsize("test.txt") < 0);

	/* attributes */
	fcb(F, 0, "NEW     TXT");
	B3MEM(F)[9] |= 0x80;
	check("set_ro", bdos(30L, F) == 0 && (hmode("new.txt") & 0222) == 0);
	check("ro_write", bdos(15L, F) == 0 && (B3MEM(F)[9] & 0x80) && bdos(21L, F) == 0x03ff);
	check("ro_delete", bdos(19L, F) == 0x03ff && hsize("new.txt") == 384);
	fcb(F, 0, "NEW     TXT");
	B3MEM(F)[11] |= 0x80;
	check("archive", bdos(30L, F) == 0 && (hmode("new.txt") & 0201) == 0201);
	bdos(15L, F);
	check("write_unarchives", bdos(21L, F) == 0 && (hmode("new.txt") & 1) == 0);
	fcb(F, 0, "NEW     TXT");
	B3MEM(F)[12] = XP_READ;
	check("protect", bdos(103L, F) == 0 && (hmode("new.txt") & 077) == 0);
	check("stamps", bdos(102L, F) == 0 && B3MEM(F)[12] == (char)XP_READ &&
	    (B3MEM(F)[28] | B3MEM(F)[29]));
	B3MEM(F)[12] = 0;
	check("unprotect", bdos(103L, F) == 0 && (hmode("new.txt") & 044) == 044);

	/* last record byte count */
	hput("odd.dat", 130L);
	fcb(F, 0, "ODD     DAT");
	B3MEM(F)[32] = 0xff;
	check("lrbc_open", bdos(15L, F) == 0 && B3MEM(F)[32] == 2);
	B3MEM(F)[32] = 5;
	B3MEM(F)[6] |= 0x80;
	check("lrbc_set", bdos(30L, F) == 0 && hsize("odd.dat") == 133);
	fcb(F, 0, "ODD     DAT");
	bdos(15L, F);
	bdos(20L, F);
	check("pad", bdos(20L, F) == 0 && B3MEM(DMA)[5] == 0x1a);

	/* user areas */
	bdos(32L, 5L);
	fcb(F, 0, "U5      TXT");
	check("user_make", bdos(22L, F) == 0 && hsize("5/u5.txt") == 0);
	check("user_get", bdos(32L, 0xffL) == 5);
	bdos(32L, 0L);
	check("user_hidden", nsearch("U5      TXT", 0, 0) == 0);
	check("user_all", nsearch("U5      TXT", '?', e) > 1);
	fcb(F2, '?', "XXXXXXXXXXX");
	for (r = bdos(17L, F2); r == 0 && B3MEM(DMA)[0] != 5; r = bdos(18L, 0L))
		;
	check("user_all_5", r == 0 && memcmp(B3MEM(DMA + 1), "U5      TXT", 11) == 0);

	/* a file larger than one directory entry */
	hput("big.dat", 300L * 1024);
	check("big_entries", nsearch("BIG     DAT", 0, e) == 3 && e[12] == 7 && e[15] == 128 &&
	    e[32 + 12] == 15 && e[64 + 12] == 18 && e[64 + 15] == 96);
	fcb(F, 0, "BIG     DAT");
	B3MEM(F)[33] = 2000 & 0xff;
	B3MEM(F)[34] = 2000 >> 8;
	check("big_random", bdos(15L, F) == 0 && bdos(33L, F) == 0 && B3MEM(F)[12] == 15 &&
	    B3MEM(DMA)[0] == (char)((300L * 1024 - 1 - 2000L * 128) & 0x7f));
	check("big_size", bdos(35L, F) == 0 && B3MEM(F)[33] == (2400 & 0xff) && B3MEM(F)[34] == 2400 >> 8);
	B3MEM(F)[33] = 9;
	B3MEM(F)[34] = 0;
	check("truncate", bdos(99L, F) == 0 && hsize("big.dat") == 1280);

	/* host changes show at once */
	hput("gone.txt", 5L);
	fcb(F, 0, "GONE    TXT");
	check("cached", bdos(15L, F) == 0);
	{
		char path[1200];

		sprintf(path, "%s/gone.txt", dir);
		unlink(path);
	}
	check("host_unlink", bdos(15L, F) == 0xff);

	/* delete */
	fcb(F, 0, "????????DAT");
	check("delete", bdos(19L, F) == 0 && hsize("odd.dat") < 0 && hsize("big.dat") < 0);
	check("delete_none", bdos(19L, F) == 0xff);

	/* disk parameters, free space */
	r = bdos(31L, 0L);
	check("dpb", r == 0x8000 && B3MEM(r)[2] == 7 && B3MEM(r)[4] == 7);
	check("free", bdos(46L, 0L) == 0 && b3_get32(DMA) > 0);

	/* console */
	nout = 0;
	in = "ab\bc\r";
	B3MEM(BUF)[0] = 20;
	bdos(10L, BUF);
	check("readline", B3MEM(BUF)[1] == 2 && memcmp(B3MEM(BUF + 2), "ac", 2) == 0);
	nout = 0;
	strcpy(B3MEM(BUF), "HI\tX$");
	bdos(9L, BUF);
	check("print", strcmp(out, "HI      X") == 0);
	in = "q";
	check("rawio", bdos(6L, 0xffL) == 'q' && bdos(6L, 0xfeL) == 0);
	strcpy(B3MEM(DMA), "DIR B:");
	check("chain", booted(47L, 0L));
	B3MEM(BUF)[0] = 20;
	bdos(10L, BUF);
	check("chain_line", B3MEM(BUF)[1] == 6 && memcmp(B3MEM(BUF + 2), "DIR B:", 6) == 0);
	bdos(26L, DMA);

	/* parse */
	strcpy(B3MEM(STR), "b:foo.bar;pw rest");
	b3_put32(PB, STR);
	b3_put32(PB + 4, (unsigned long)F2);
	r = bdos(152L, PB);
	check("parse", r == STR + 12 && B3MEM(F2)[0] == 2 && memcmp(B3MEM(F2 + 1), "FOO     BAR", 11) == 0 &&
	    memcmp(B3MEM(F2 + 16), "PW      ", 8) == 0 && B3MEM(F2)[26] == 2);
	strcpy(B3MEM(STR), "*.c");
	check("parse_star", bdos(152L, PB) == 0 && memcmp(B3MEM(F2 + 1), "????????C  ", 11) == 0);

	/* SCB */
	memcpy(B3MEM(PB), "\x05\0\0\0", 4);
	check("scb_version", bdos(49L, PB) == 0x31);
	memcpy(B3MEM(PB), "\x1a\xff\x4f\0", 4);
	check("scb_width", bdos(49L, PB) == 0 && b3_conwidth == 80);

	/* program load: text a long pointing 4 bytes into itself */
	{
		static unsigned char prg[] = {
			0x60, 0x1a, 0, 0, 0, 8, 0, 0, 0, 4, 0, 0, 0, 16, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0x4e, 0x75, 0, 0, 0, 0, 0, 4, 0xaa, 0xbb, 0xcc, 0xdd,
			0, 0, 0, 0, 0, 5, 0, 2, 0, 0, 0, 0
		};
		char path[1200];
		int fd;

		sprintf(path, "%s/prog.68k", dir);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		write(fd, prg, sizeof prg);
		close(fd);
		fcb(F, 0, "PROG    68K");
		bdos(15L, F);
		b3_put32(PB, (unsigned long)F);
		b3_put32(PB + 4, 0x30000L);
		b3_put32(PB + 8, MEMSZ);
		check("load", bdos(59L, PB) == 0 && b3_get32(PB + 12) == 0x30000 &&
		    b3_get32(0x30008L) == 0x30100 && b3_get32(0x30104L) == 0x30104 &&
		    b3_get32(0x30108L) == 0xaabbccdd && b3_get32(0x30018L) == 0x3010c &&
		    b3_get32(b3_get32(PB + 16)) == 0x30000);
	}

	/* errors in the default mode end the program */
	bdos(45L, 0L);
	nout = 0;
	fcb(F, 3, "X       Y  ");
	check("err_drive", booted(15L, F) && strstr(out, "Invalid Drive") != 0);
	check("exit_link", nsearch("EXIT    68K", 0, 0) == 1);

	printf("bdostest: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
