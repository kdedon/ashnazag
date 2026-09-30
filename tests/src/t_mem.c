/*
 * t_mem.c -- brk/sbrk, malloc, copy-on-write, mmap, mprotect, stack growth.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

#define PG	4096


static int
fillchk(p, n, seed, check)
char *p;
long n;
int seed, check;
{
	long i;

	for (i = 0; i < n; i++) {
		if (check) {
			if (p[i] != (char)t_pattern(i, seed))
				return (int)(i + 1 > 0x7fffffff ? 0x7fffffff : i + 1);
		} else
			p[i] = t_pattern(i, seed);
	}
	return 0;
}

/* run fn in a child; return its wait status */
static int
inchild(fn, arg)
int (*fn)();
char *arg;
{
	pid_t pid;
	int st;

	pid = fork();
	if (pid == 0)
		_exit((*fn)(arg));
	if (pid == -1 || t_waitchild(pid, &st, 30) != pid)
		return -1;
	return st;
}

static void
test_sbrk()
{
	char *base, *p, *q, *reg[4];
	int i, bad;

	base = sbrk(0);
	bad = 0;
	for (i = 0; i < 4; i++) {
		p = sbrk(1024 * 1024);
		if (p == (char *)-1) {
			t_fail("sbrk_grow", "sbrk step %d: %s", i, T_ERR);
			return;
		}
		reg[i] = p;
		fillchk(p, 1024L * 1024, i, 0);
	}
	for (i = 0; i < 4; i++)
		if (fillchk(reg[i], 1024L * 1024, i, 1))
			bad++;
	t_check("sbrk_grow_4MB", bad == 0, "%d of 4 MB regions corrupted", bad);

	/* shrink by 2 MB, grow again: whole new pages must read as zero */
	q = sbrk(0);
	if (sbrk(-2 * 1024 * 1024) == (char *)-1) {
		t_fail("sbrk_shrink", "%s", T_ERR);
		return;
	}
	t_check("sbrk_shrink", sbrk(0) == q - 2 * 1024 * 1024, "break %lx, want %lx",
	    (long)sbrk(0), (long)(q - 2 * 1024 * 1024));
	p = sbrk(2 * 1024 * 1024);
	bad = 0;
	{
		char *z = (char *)(((long)p + PG - 1) & ~(PG - 1));
		long i2;
		for (i2 = 0; z + i2 < p + 2 * 1024 * 1024; i2++)
			if (z[i2] != 0) {
				bad = 1;
				break;
			}
	}
	t_check("sbrk_regrow_zero", bad == 0, "stale data after shrink/regrow");
	sbrk(-(int)((char *)sbrk(0) - base));
	t_check("brk_restore", sbrk(0) == base, "break %lx, want %lx", (long)sbrk(0), (long)base);
}

static void
test_malloc()
{
	char *p[6];
	int i, bad = 0, n = 0;

	p[0] = malloc(8 * 1024 * 1024);
	if (p[0] == 0) {
		t_fail("malloc_8MB", "malloc returned NULL");
	} else {
		fillchk(p[0], 8L * 1024 * 1024, 7, 0);
		t_check("malloc_8MB", fillchk(p[0], 8L * 1024 * 1024, 7, 1) == 0, "pattern mismatch");
		free(p[0]);
	}
	for (i = 0; i < 6; i++) {
		p[i] = malloc(1024 * 1024 + i * 1000);
		if (p[i] != 0) {
			fillchk(p[i], 1024L * 1024 + i * 1000, i + 20, 0);
			n++;
		}
	}
	for (i = 0; i < 6; i++)
		if (p[i] != 0 && fillchk(p[i], 1024L * 1024 + i * 1000, i + 20, 1))
			bad++;
	for (i = 0; i < 6; i++)
		free(p[i]);
	t_check("malloc_6x1MB", n == 6 && bad == 0, "%d allocated, %d corrupted", n, bad);
}

static char cow_bss[64 * 1024];
static char cow_data[8192] = { 1 };

static void
test_cow()
{
	char *heap;
	int p1[2], p2[2], st, stackbuf[1024];
	pid_t pid;
	char c;
	long i;

	heap = malloc(256 * 1024);
	if (heap == 0) {
		t_fail("cow", "malloc 256 KB returned NULL");
		return;
	}
	fillchk(heap, 256L * 1024, 1, 0);
	fillchk(cow_bss, (long)sizeof cow_bss, 2, 0);
	fillchk(cow_data, (long)sizeof cow_data, 3, 0);
	for (i = 0; i < 1024; i++)
		stackbuf[i] = i * 3;
	pipe(p1);
	pipe(p2);
	pid = fork();
	if (pid == 0) {
		int r = 0;
		read(p1[0], &c, 1);		/* parent has written its copies */
		if (fillchk(heap, 256L * 1024, 1, 1)) r |= 1;
		if (fillchk(cow_bss, (long)sizeof cow_bss, 2, 1)) r |= 2;
		if (fillchk(cow_data, (long)sizeof cow_data, 3, 1)) r |= 4;
		for (i = 0; i < 1024; i++)
			if (stackbuf[i] != i * 3) r |= 8;
		fillchk(heap, 256L * 1024, 11, 0);
		fillchk(cow_bss, (long)sizeof cow_bss, 12, 0);
		fillchk(cow_data, (long)sizeof cow_data, 13, 0);
		for (i = 0; i < 1024; i++)
			stackbuf[i] = -1;
		if (fillchk(heap, 256L * 1024, 11, 1)) r |= 16;
		write(p2[1], "x", 1);
		_exit(r);
	}
	if (pid == -1) {
		t_fail("cow", "fork: %s", T_ERR);
		free(heap);
		close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
		return;
	}
	/* parent writes after the fork, before the child reads */
	fillchk(heap, 256L * 1024, 21, 0);
	fillchk(cow_bss, (long)sizeof cow_bss, 22, 0);
	fillchk(cow_data, (long)sizeof cow_data, 23, 0);
	for (i = 0; i < 1024; i++)
		stackbuf[i] = 7;
	write(p1[1], "x", 1);
	read(p2[0], &c, 1);
	t_waitchild(pid, &st, 30);
	t_check("cow_child_sees_prefork", st == 0, "child status 0x%x (1 heap, 2 bss, 4 data, 8 stack, 16 own write)", st);
	{
		int r = 0;
		if (fillchk(heap, 256L * 1024, 21, 1)) r |= 1;
		if (fillchk(cow_bss, (long)sizeof cow_bss, 22, 1)) r |= 2;
		if (fillchk(cow_data, (long)sizeof cow_data, 23, 1)) r |= 4;
		for (i = 0; i < 1024; i++)
			if (stackbuf[i] != 7) r |= 8;
		t_check("cow_parent_keeps_own", r == 0, "parent corrupted, mask 0x%x", r);
	}
	free(heap);
	close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
}

#define MFSZ (3 * PG + 100)

static void
test_mmap_file()
{
	int fd, bad, i;
	char *m, buf[PG];

	unlink("/tmp/mmf");
	fd = open("/tmp/mmf", O_RDWR | O_CREAT | O_TRUNC, 0644);
	t_fill(fd, MFSZ, 5);
	m = mmap((caddr_t)0, MFSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0);
	if (m == (char *)-1) {
		t_fail("mmap_file_shared", "mmap: %s", T_ERR);
		close(fd);
		return;
	}
	t_check("mmap_file_content", fillchk(m, (long)MFSZ, 5, 1) == 0, "mapping differs from file");
	bad = 0;
	for (i = MFSZ; i < 4 * PG; i++)
		if (m[i] != 0)
			bad++;
	t_check("mmap_file_eof_zero", bad == 0, "%d nonzero bytes past EOF in last page", bad);

	m[10] = 'Q';
	m[3 * PG + 50] = 'R';
	msync(m, MFSZ, MS_SYNC);
	lseek(fd, 0L, 0);
	read(fd, buf, 20);
	lseek(fd, (off_t)(3 * PG + 50), 0);
	read(fd, buf + 20, 1);
	t_check("mmap_shared_store_to_read", buf[10] == 'Q' && buf[20] == 'R', "read() got %02x %02x",
	    buf[10] & 0xff, buf[20] & 0xff);
	lseek(fd, (off_t)(PG + 7), 0);
	write(fd, "W", 1);
	t_check("mmap_write_to_mapping", m[PG + 7] == 'W', "mapping sees %02x", m[PG + 7] & 0xff);
	munmap(m, MFSZ);

	/* private: stores stay out of the file */
	m = mmap((caddr_t)0, MFSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, (off_t)0);
	if (m == (char *)-1) {
		t_fail("mmap_file_private", "mmap: %s", T_ERR);
	} else {
		m[20] = 'P';
		lseek(fd, 20L, 0);
		read(fd, buf, 1);
		t_check("mmap_file_private", buf[0] != 'P' && m[20] == 'P', "private store reached the file");
		munmap(m, MFSZ);
	}
	/* offset mapping */
	m = mmap((caddr_t)0, PG, PROT_READ, MAP_SHARED, fd, (off_t)(2 * PG));
	if (m == (char *)-1)
		t_fail("mmap_file_offset", "mmap: %s", T_ERR);
	else {
		t_check("mmap_file_offset", m[1] == (char)t_pattern(2 * PG + 1, 5),
		    "byte %02x", m[1] & 0xff);
		munmap(m, PG);
	}
	close(fd);
	unlink("/tmp/mmf");
}

static char *zmap;

static int
touch_ro(arg)
char *arg;
{
	*(volatile char *)arg = 1;
	return 0;
}

static int
read_at(arg)
char *arg;
{
	return *(volatile char *)arg == 0 ? 0 : 0;
}

static void
test_mmap_zero()
{
	int fd, st, i, bad;
	char *m, *s;
	pid_t pid;

	fd = open("/dev/zero", O_RDWR);
	if (fd < 0) {
		t_fail("dev_zero_open", "%s", T_ERR);
		return;
	}
	m = mmap((caddr_t)0, 16 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, (off_t)0);
	if (m == (char *)-1) {
		t_fail("mmap_zero_private", "mmap: %s", T_ERR);
		close(fd);
		return;
	}
	bad = 0;
	for (i = 0; i < 16 * PG; i++)
		if (m[i] != 0)
			bad++;
	t_check("mmap_zero_private_zero", bad == 0, "%d nonzero", bad);
	fillchk(m, 16L * PG, 9, 0);
	pid = fork();
	if (pid == 0) {
		fillchk(m, 16L * PG, 10, 0);
		_exit(0);
	}
	t_waitchild(pid, &st, 30);
	t_check("mmap_zero_private_cow", fillchk(m, 16L * PG, 9, 1) == 0, "child write visible in parent");

	s = mmap((caddr_t)0, 4 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0);
	if (s == (char *)-1)
		t_fail("mmap_zero_shared", "mmap: %s", T_ERR);
	else {
		s[0] = 1;
		pid = fork();
		if (pid == 0) {
			s[0] = 2;
			s[3 * PG] = 3;
			_exit(0);
		}
		t_waitchild(pid, &st, 30);
		t_check("mmap_zero_shared", s[0] == 2 && s[3 * PG] == 3, "parent sees %d %d", s[0], s[3 * PG]);
		munmap(s, 4 * PG);
	}

	/* mprotect */
	zmap = m;
	if (mprotect(m, PG, PROT_READ) == -1)
		t_fail("mprotect_ro", "mprotect: %s", T_ERR);
	else {
		st = inchild(touch_ro, m + 5);
		t_check("mprotect_ro_faults", st != -1 && WIFSIGNALED(st) &&
		    (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS),
		    "store to read-only page: status 0x%x", st);
		st = inchild(touch_ro, m + PG + 5);
		t_check("mprotect_neighbour_rw", st == 0, "store to next page: status 0x%x", st);
		t_check("mprotect_ro_readable", m[5] == (char)t_pattern(5, 9), "content changed");
		mprotect(m, PG, PROT_READ | PROT_WRITE);
		m[5] = 77;
		t_check("mprotect_rw_again", m[5] == 77, "store lost");
	}
	if (mprotect(m + 2 * PG, PG, PROT_NONE) == 0) {
		st = inchild(read_at, m + 2 * PG);
		t_check("mprotect_none_faults", st != -1 && WIFSIGNALED(st), "load from PROT_NONE page: status 0x%x", st);
	}
	munmap(m, 16 * PG);
	st = inchild(read_at, m + 4);
	t_check("munmap_faults", st != -1 && WIFSIGNALED(st), "load after munmap: status 0x%x", st);
	close(fd);
}

static int badn = -1, badv;

static void
recurse(n)
int n;
{
	volatile char buf[1024];

	buf[0] = n;
	buf[1023] = n;
	if (n > 0)
		recurse(n - 1);
	if ((buf[0] != (char)n || buf[1023] != (char)n) && badn < 0) {
		badn = n;
		badv = buf[0] != (char)n ? buf[0] : buf[1023];
	}
}

/* ~400 KB of stack; status 1 + (depth of the first bad frame) / 2, 255 if it read zero */
static int
deepstack(arg)
char *arg;
{
	recurse(400);
	if (badn < 0)
		return 0;
	return badv == 0 ? 255 : 1 + (400 - badn) / 2;
}

static int
nullref(arg)
char *arg;
{
	return *(volatile int *)0 == 0x12345678 ? 3 : 2;
}

static void
test_stack()
{
	int st;

	st = inchild(deepstack, (char *)0);
	t_check("stack_grow_400KB", st == 0, "status 0x%x%s", st,
	    st == 0xff00 ? " (a frame read zero)" : "");
	st = inchild(nullref, (char *)0);
	if (st != -1 && WIFSIGNALED(st))
		t_pass("null_deref_faults");
	else
		t_info("null_deref_faults", "load from address 0 did not fault (status 0x%x)", st);
}

int
main()
{
	pid_t pid;
	int st;

	t_init("mem", 80);
	/*
	 * Heap tests in a child: malloc keeps the heap, and every later fork
	 * would reserve its copy against swap.
	 */
	pid = fork();
	if (pid == 0) {
		test_sbrk();
		test_malloc();
		_exit(t_nfail != 0);
	}
	if (pid == -1 || t_waitchild(pid, &st, 60) != pid || !WIFEXITED(st))
		t_fail("heap", "child: %s", pid == -1 ? T_ERR : "died or hung");
	else if (WEXITSTATUS(st) != 0)
		t_nfail++;
	test_cow();
	test_mmap_file();
	test_mmap_zero();
	test_stack();
	return t_done();
}
