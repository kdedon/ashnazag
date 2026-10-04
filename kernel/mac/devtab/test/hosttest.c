/*
 * Host test of the disk root policy: mac_diskpick, mac_dskname,
 * mac_fsprobe, and config()'s mac_root (included as root.c, cut from
 * macconf.c by verify.sh).
 *
 *   hosttest [s5-image]
 */
#include <stdio.h>
#include <string.h>
#include "macroot.h"

unsigned long rootdev, dumpdev;
unsigned long mac_auxinfo[3];
char swapfile[160];
static unsigned char mem[0x400];	/* hand-off block at mem, below "stext" */
#define stext	((char *)mem + sizeof mem)
static int image;
void mac_puts(s) char *s; { }
void putkv(k, v) char *k; unsigned long v; { }
static int halted;
void mac_halt(s) char *s; { halted = 1; }
int mac_rd_config(want) int want;
{
	if (!want || !image)
		return 0;
	rootdev = 20UL << 18; dumpdev = 20UL << 18 | 1;
	strcpy(swapfile + 16, "/dev/swap");
	return 1;
}
#include "root.c"

static int bad;

static void
ok(c, what)
int c; char *what;
{
	printf("%s %s\n", c ? "OK  " : "FAIL", what);
	bad |= !c;
}

/* 'Pigs' block: flags, root (ctrl, drive, part), swap (ctrl, drive, part) */
static unsigned char *
ki(flags, rc, rd, rp, sc, sd, sp)
int flags, rc, rd, rp, sc, sd, sp;
{
	memset(mem, 0, sizeof mem);
	memcpy(mem, "Pigs", 4);
	mem[0x88] = rc >> 8; mem[0x89] = rc;
	mem[0x8A] = rd;
	mem[0xB6] = flags >> 8; mem[0xB7] = flags;
	mem[0xB8] = 1;
	mem[0xB9] = rp;
	mem[0xBA] = sc >> 8; mem[0xBB] = sc;
	mem[0xBC] = sd;
	mem[0xBD] = sp;
	return mem;
}

static void
pick(what, p, lim, how, root, swap)
char *what; unsigned char *p, *lim; int how; long root, swap;
{
	long r, s;
	int h;
	char b[200];

	h = mac_diskpick(p, lim, &r, &s);
	sprintf(b, "diskpick %-40s -> %d root 0x%02lx swap 0x%02lx", what, h, r, s);
	ok(h == how && r == root && s == swap, b);
}

static void
root(what, cmd, info, img, rdev, ddev, swap)
char *what, *cmd, *swap; int info, img; unsigned long rdev, ddev;
{
	char b[200];

	rootdev = 0x00480016; dumpdev = 0x00400004;
	memset(swapfile, 0, sizeof swapfile);
	strcpy(swapfile + 16, "/dev/dsk/c6d0s2");
	mac_rootarg = -1; image = img; halted = 0;
	mac_auxinfo[0] = info ? (unsigned long)mem : 0;
	if (cmd)
		mac_rootparse(cmd);
	mac_root();
	sprintf(b, "mac_root %-34s image %d -> rootdev 0x%08lx dumpdev 0x%08lx swap %s%s",
	    what, img, rootdev, dumpdev, swapfile + 16, halted ? " (halt)" : "");
	ok(rootdev == rdev && dumpdev == ddev && !strcmp(swapfile + 16, swap) &&
	    halted == (rdev == 0x00480016), b);
}

static unsigned char disk[0x4000];

static int
rdmem(arg, blk, n, buf)
char *arg; long blk; int n; unsigned char *buf;
{
	if (arg)
		return 5;
	memcpy(buf, disk + blk * 512, n);
	return 0;
}

int
main(argc, argv)
int argc; char **argv;
{
	unsigned char *lim = mem + sizeof mem;
	unsigned char buf[2048];
	char name[16];
	FILE *f;

	pick("no hand-off", (unsigned char *)0, lim, PICK_DEFAULT, 0x10L, 0x20L);
	ki(0, 3, 0, 0, 0, 0, 0); memcpy(mem, "Pigz", 4);
	pick("bad magic", mem, lim, PICK_DEFAULT, 0x10L, 0x20L);
	pick("block past the limit", ki(0, 3, 0, 0, 0, 0, 0), mem + 0x80, PICK_DEFAULT, 0x10L, 0x20L);
	pick("root disk ID 3", ki(0, 3, 0, 5, 1, 0, 6), lim, PICK_KIROOT, 0x13L, 0x23L);
	pick("root disk ID 3, flags 0x11 (-v -S)", ki(0x11, 3, 0, 5, 1, 0, 6), lim, PICK_KIROOT, 0x13L, 0x23L);
	pick("root disk ID 0", ki(0, 0, 0, 0, 0, 0, 0), lim, PICK_KIROOT, 0x10L, 0x20L);
	pick("root disk ID 7 (host)", ki(0, 7, 0, 0, 0, 0, 0), lim, PICK_DEFAULT, 0x10L, 0x20L);
	pick("root disk ID -1", ki(0, 0xffff, 0, 0, 0, 0, 0), lim, PICK_DEFAULT, 0x10L, 0x20L);
	pick("root LUN 1", ki(0, 2, 1, 0, 0, 0, 0), lim, PICK_DEFAULT, 0x10L, 0x20L);
	pick("-e/-p: root 2/0 swap 4/1", ki(8, 2, 0, 0, 4, 0, 1), lim, PICK_KIEXPLICIT, 0x12L, 0x24L);
	pick("-e/-p: root 5/3 swap 5/6", ki(8, 5, 0, 3, 5, 0, 6), lim, PICK_KIEXPLICIT, 0x45L, 0x75L);
	pick("-e/-p: root partition 7", ki(8, 5, 0, 7, 5, 0, 1), lim, PICK_DEFAULT, 0x10L, 0x20L);
	pick("-e/-p: bad swap -> root disk s2", ki(8, 1, 0, 0, 9, 0, 1), lim, PICK_KIEXPLICIT, 0x11L, 0x21L);
	pick("-e/-p: swap LUN 2 -> root disk s2", ki(8, 6, 0, 2, 6, 2, 1), lim, PICK_KIEXPLICIT, 0x36L, 0x26L);

	mac_dskname(name, 0x13L);
	ok(!strcmp(name, "/dev/dsk/c3d0s1"), "dskname 0x13 -> /dev/dsk/c3d0s1");
	mac_dskname(name, 0x76L);
	ok(!strcmp(name, "/dev/dsk/c6d0s7"), "dskname 0x76 -> /dev/dsk/c6d0s7");

	memset(disk, 0, sizeof disk);
	ok(!strcmp(mac_fsprobe(rdmem, (char *)0, buf), ""), "fsprobe blank slice -> \"\"");
	ok(!strcmp(mac_fsprobe(rdmem, (char *)1, buf), ""), "fsprobe read error -> \"\"");
	memcpy(disk + 16 * 512 + 1372, "\0\1\31\124", 4);
	ok(!strcmp(mac_fsprobe(rdmem, (char *)0, buf), "ufs"), "fsprobe fs_magic 0x011954 at 8192+1372 -> ufs");
	memcpy(disk + 512 + 504, "\375\30\176\40", 4);
	ok(!strcmp(mac_fsprobe(rdmem, (char *)0, buf), "s5"), "fsprobe s_magic 0xfd187e20 at 512+504 -> s5");
	if (argc > 1 && (f = fopen(argv[1], "rb")) != 0) {
		memset(disk, 0, sizeof disk);
		fread(disk, 1, sizeof disk, f);
		fclose(f);
		ok(!strcmp(mac_fsprobe(rdmem, (char *)0, buf), "s5"), "fsprobe RAM-disk root image -> s5");
	}

	/* no disk root without root=: those cases halt (rootdev left 0x00480016) */
	root("no hand-off, no image", (char *)0, 0, 0, 0x00480016, 0x00400004, "/dev/dsk/c6d0s2");
	root("no hand-off, image", (char *)0, 0, 1, 0x00500000, 0x00500001, "/dev/swap");
	ki(0, 3, 0, 0, 0, 0, 0);
	root("A/UX root disk 3, image", (char *)0, 1, 1, 0x00500000, 0x00500001, "/dev/swap");
	root("A/UX root disk 3, no image", (char *)0, 1, 0, 0x00480016, 0x00400004, "/dev/dsk/c6d0s2");
	ki(8, 2, 0, 0, 4, 0, 1);
	root("A/UX -e/-p root 2/0 swap 4/1, image", (char *)0, 1, 1, 0x00500000, 0x00500001, "/dev/swap");
	root("A/UX -e/-p root 2/0 swap 4/1, no image", (char *)0, 1, 0, 0x00480016, 0x00400004, "/dev/dsk/c6d0s2");
	root("root=c5d0s3 over -e/-p, image", "root=c5d0s3", 1, 1, 0x00480035, 0x00480025, "/dev/dsk/c5d0s2");
	root("root=c1d0s1, image", "root=c1d0s1", 0, 1, 0x00480011, 0x00480021, "/dev/dsk/c1d0s2");
	root("bad root=, no image", "root=c9d0s1", 0, 0, 0x00480016, 0x00400004, "/dev/dsk/c6d0s2");
	root("root= on swap slice, no image", "root=c0d0s2", 0, 0, 0x00480016, 0x00400004, "/dev/dsk/c6d0s2");
	return bad;
}
