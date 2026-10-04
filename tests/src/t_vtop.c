/*
 * t_vtop.c -- raw disk and /proc transfers to user buffers below 1 GB.
 *
 * The kernel translates such buffers with vtop(va, proc).  Each buffer
 * is placed at the virtual address equal to the physical address of the
 * RAM-disk block it transfers (rd_unit[0].base + offset), so a kernel
 * that takes the user address for a physical one only copies that block
 * onto itself: the test sees the error and nothing else is touched.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

#define PG	4096
#define RAWDEV	"/dev/rdsk/rd0"
#define DECOY	"/tmp/vtop.dsk"
#define MAPF	"/tmp/vtop.map"
#define DDNODE	"/tmp/vtop.dd"
#define FSZ	8192
#define S_DECOY	61
#define S_MAP	62
#define S_WRITE	63
#define S_PROC	64
#define FILL	0x5a

static long rdbase;		/* physical = kernel address of RAM-disk offset 0 */
static long bn0;		/* first block of DECOY */
static int rawfd = -1;
static char *lva;		/* rdbase + bn0 * 1024: low user VA of that block */
static char *lpg;		/* its page */
static char hbuf[FSZ], hbuf2[FSZ];

static int
rawio(fd, wr, off, buf, n)
int fd, wr;
long off, n;
char *buf;
{
	if (lseek(fd, (off_t)off, 0) == -1)
		return 0;
	return (wr ? write(fd, buf, (unsigned)n) : read(fd, buf, (unsigned)n)) == n;
}

/* first index where p differs from the pattern, -1 if none */
static long
patdiff(p, n, base, seed)
char *p;
long n, base;
int seed;
{
	long i;

	for (i = 0; i < n; i++)
		if (p[i] != (char)t_pattern(base + i, seed))
			return i;
	return -1;
}

/* n bytes of anonymous memory at va exactly, filled with FILL */
static char *
lowmap(va, n)
char *va;
long n;
{
	char *m;
	int fd = open("/dev/zero", O_RDWR);

	if (fd < 0)
		return 0;
	m = mmap(va, (size_t)n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, (off_t)0);
	close(fd);
	if (m != va)
		return 0;
	memset(m, FILL, (size_t)n);
	return m;
}

static int
setup()
{
	struct stat sb, rb;
	struct statvfs v;
	char ino[512];
	unsigned char *a;
	long j;
	int fd;

	if (stat("/", &sb) == -1 || stat("/dev/dsk/rd0", &rb) == -1 || sb.st_dev != rb.st_rdev ||
	    statvfs("/", &v) == -1 || v.f_bsize != 1024) {
		t_skip("setup", "root is not rd0 with 1 KB blocks");
		return 0;
	}
	rdbase = t_kmem("rd_unit");
	if (rdbase == -1 || rdbase <= 0 || rdbase >= 0x30000000) {
		t_skip("setup", "RAM-disk base unknown (rd_unit 0x%lx)", rdbase);
		return 0;
	}
	fd = open(DECOY, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || t_fill(fd, FSZ, S_DECOY) != FSZ || fsync(fd) == -1 || fstat(fd, &sb) == -1) {
		t_fail("setup", "%s: %s", DECOY, T_ERR);
		return 0;
	}
	close(fd);
	fd = open(MAPF, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || t_fill(fd, FSZ, S_MAP) != FSZ || fsync(fd) == -1) {
		t_fail("setup", "%s: %s", MAPF, T_ERR);
		return 0;
	}
	close(fd);
	sync();
	rawfd = open(RAWDEV, O_RDWR);
	if (rawfd < 0) {
		t_fail("setup", "open " RAWDEV ": %s", T_ERR);
		return 0;
	}
	/* s5: 64-byte inodes from block 2, 3-byte block numbers at offset 12 */
	j = 2 * 1024 + (sb.st_ino - 1) * 64;
	if (!rawio(rawfd, 0, j & ~511L, ino, 512L)) {
		t_fail("setup", "inode read: %s", T_ERR);
		return 0;
	}
	a = (unsigned char *)ino + (j & 511) + 12;
	bn0 = (long)a[0] << 16 | a[1] << 8 | a[2];
	if (bn0 == 0 || !rawio(rawfd, 0, bn0 * 1024, hbuf, 1024L) ||
	    patdiff(hbuf, 1024L, 0L, S_DECOY) >= 0) {
		t_fail("setup", "block %ld of " DECOY " not found on disk", bn0);
		return 0;
	}
	lva = (char *)(rdbase + bn0 * 1024);
	lpg = (char *)((long)lva & ~(PG - 1L));
	t_info("setup", "RAM disk at 0x%lx, block %ld, user buffer at 0x%lx", rdbase, bn0, (long)lva);
	return 1;
}

/* raw read into three separately allocated low pages */
static void
test_raw_read()
{
	long off = bn0 * 1024, i;

	if (!rawio(rawfd, 0, off, hbuf, (long)FSZ)) {
		t_fail("raw_read_lowva", "read into a normal buffer: %s", T_ERR);
		return;
	}
	if (!lowmap(lpg, 3L * PG)) {
		t_fail("raw_read_lowva", "mmap at 0x%lx: %s", (long)lpg, T_ERR);
		return;
	}
	if (!rawio(rawfd, 0, off, lva, (long)FSZ))
		t_fail("raw_read_lowva", "read: %s", T_ERR);
	else if (memcmp(lva, hbuf, FSZ) == 0)
		t_pass("raw_read_lowva");
	else {
		for (i = 0; i < FSZ && lva[i] == hbuf[i]; i++)
			;
		t_fail("raw_read_lowva", "buffer differs at %ld (0x%02x): data went to physical 0x%lx",
		    i, lva[i] & 0xff, (long)lva + i);
	}
	munmap(lpg, 3L * PG);
}

/* raw write of DECOY's first block from a low page */
static void
test_raw_write()
{
	long off = bn0 * 1024, i, d;

	if (!lowmap(lpg, (long)PG)) {
		t_fail("raw_write_lowva", "mmap at 0x%lx: %s", (long)lpg, T_ERR);
		return;
	}
	for (i = 0; i < 1024; i++)
		lva[i] = t_pattern(i, S_WRITE);
	if (!rawio(rawfd, 1, off, lva, 1024L) || !rawio(rawfd, 0, off, hbuf, 1024L))
		t_fail("raw_write_lowva", "%s", T_ERR);
	else if ((d = patdiff(hbuf, 1024L, 0L, S_WRITE)) < 0)
		t_pass("raw_write_lowva");
	else
		t_fail("raw_write_lowva", "disk differs at %ld%s", d,
		    patdiff(hbuf, 1024L, 0L, S_DECOY) < 0 ?
		    ": it got physical memory, not the buffer" : "");
	munmap(lpg, (long)PG);
	/* put DECOY's content back for the /proc tests */
	for (i = 0; i < 1024; i++)
		hbuf[i] = t_pattern(i, S_DECOY);
	rawio(rawfd, 1, off, hbuf, 1024L);
}

/*
 * A child maps MAPF at lpg (resident: touched; else untouched) and waits.
 * how: 0 read, 1 write through /proc; the child checks a write itself.
 */
static void
test_proc(name, how, resident)
char *name;
int how, resident;
{
	int up[2], down[2], fd, st, n;
	long o = lva - lpg, d;
	pid_t pid;
	char c, path[32], *m;

	if (pipe(up) == -1 || pipe(down) == -1) {
		t_fail(name, "pipe: %s", T_ERR);
		return;
	}
	pid = fork();
	if (pid == 0) {
		fd = open(MAPF, O_RDONLY);
		m = mmap(lpg, (size_t)PG, PROT_READ | (how ? PROT_WRITE : 0),
		    (how ? MAP_PRIVATE : MAP_SHARED) | MAP_FIXED, fd, (off_t)0);
		if (m != lpg)
			_exit(3);
		c = resident || how ? m[o] : 0;
		write(up[1], &c, 1);
		if (read(down[0], &c, 1) != 1)
			_exit(4);
		if (how)
			_exit(patdiff(lva, 256L, 0L, S_PROC) < 0 ? 0 :
			    patdiff(lva, 256L, o, S_MAP) < 0 ? 1 : 2);
		_exit(0);
	}
	if (pid == -1 || read(up[0], &c, 1) != 1) {
		t_fail(name, "child did not start");
		if (pid > 0)
			t_waitchild(pid, &st, 10);
		return;
	}
	sprintf(path, "/proc/%05ld", (long)pid);
	fd = open(path, how ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		t_fail(name, "open %s: %s", path, T_ERR);
		close(down[1]);
		t_waitchild(pid, &st, 10);
		return;
	}
	if (how == 0) {
		memset(hbuf2, FILL, 1024);
		n = lseek(fd, (off_t)(long)lva, 0) == -1 ? -1 : read(fd, hbuf2, 1024);
		if (n != 1024)
			t_fail(name, "read %d: %s", n, T_ERR);
		else if ((d = patdiff(hbuf2, 1024L, o, S_MAP)) < 0)
			t_pass(name);
		else
			t_fail(name, "data differs at %ld%s", d, patdiff(hbuf2, 1024L, 0L, S_DECOY) < 0 ?
			    ": got physical memory at the user address (kernel memory disclosure)" : "");
		write(down[1], "g", 1);
		t_waitchild(pid, &st, 10);
	} else {
		for (d = 0; d < 256; d++)
			hbuf2[d] = t_pattern(d, S_PROC);
		n = lseek(fd, (off_t)(long)lva, 0) == -1 ? -1 : write(fd, hbuf2, 256);
		write(down[1], "g", 1);
		t_waitchild(pid, &st, 10);
		rawio(rawfd, 0, bn0 * 1024, hbuf, 1024L);
		if (n != 256)
			t_fail(name, "write %d: %s", n, T_ERR);
		else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
			t_fail(name, "child status 0x%x%s", st,
			    patdiff(hbuf, 256L, 0L, S_PROC) < 0 ?
			    ": the write went to physical memory at the user address" : "");
		else
			t_check(name, patdiff(hbuf, 1024L, 0L, S_DECOY) < 0,
			    "physical memory at the user address changed");
	}
	close(fd);
	close(up[0]); close(up[1]); close(down[0]); close(down[1]);
	if (how == 0 && (!WIFEXITED(st) || WEXITSTATUS(st) != 0))
		t_info(name, "child status 0x%x", st);
}

/* raw SCSI disk (target 0, whole disk), when there is one */
static void
test_dd()
{
	char *va = (char *)0x00700800, *m;
	int fd;

	unlink(DDNODE);
	if (mknod(DDNODE, S_IFCHR | 0600, makedev(40, 0)) == -1) {
		t_skip("dd_read_lowva", "mknod: %s", T_ERR);
		return;
	}
	fd = open(DDNODE, O_RDONLY);
	unlink(DDNODE);
	if (fd < 0 || !rawio(fd, 0, 0L, hbuf, 4096L)) {
		t_skip("dd_read_lowva", "no SCSI disk at target 0");
		if (fd >= 0)
			close(fd);
		return;
	}
	m = lowmap((char *)0x00700000, 2L * PG);
	if (!m)
		t_fail("dd_read_lowva", "mmap: %s", T_ERR);
	else
		t_check("dd_read_lowva", rawio(fd, 0, 0L, va, 4096L) && memcmp(va, hbuf, 4096) == 0,
		    "data differs from a normal-buffer read");
	if (m)
		munmap(m, 2L * PG);
	close(fd);
}

int
main()
{
	t_init("vtop", 55);
	if (setup()) {
		test_raw_read();
		test_raw_write();
		test_proc("proc_read_lowva", 0, 0);
		test_proc("proc_read_lowva_resident", 0, 1);
		test_proc("proc_write_lowva", 1, 0);
	}
	test_dd();
	if (rawfd >= 0)
		close(rawfd);
	unlink(DECOY);
	unlink(MAPF);
	return t_done();
}
