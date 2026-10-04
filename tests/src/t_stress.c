/*
 * t_stress.c -- fork + pipe + file I/O + exec for about 20 s, then compare
 * free memory, swap and filesystem counts with the values before.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

#define XFER	16384

static int
child(i, wfd)
int i, wfd;
{
	char path[32], buf[12288];
	int fd, k, j;

	if (t_fill(wfd, XFER, i) != XFER)
		return 1;
	close(wfd);
	sprintf(path, "/tmp/st%d", (int)getpid());
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return 2;
	if (t_fill(fd, 5000 + (i % 7) * 1000, i) != 5000 + (i % 7) * 1000)
		return 3;
	lseek(fd, 0L, 0);
	k = read(fd, buf, sizeof buf);
	close(fd);
	unlink(path);
	if (k != 5000 + (i % 7) * 1000)
		return 4;
	for (j = 0; j < k; j++)
		if (buf[j] != (char)t_pattern((long)j, i))
			return 5;
	if (i % 5 == 0) {
		execl("/tests/xh_d2049", "xh", "exit", "0", (char *)0);
		return 6;
	}
	{
		char *m = malloc(200 * 1024);
		if (m == 0)
			return 7;
		memset(m, i, 200 * 1024);
		free(m);
	}
	return 0;
}

static int
round1(i)
int i;
{
	char buf[4096];
	long off = 0;
	pid_t pid;
	int p[2], k, j, st, bad = 0;

	if (pipe(p) == -1)
		return 100;
	pid = fork();
	if (pid == -1) {
		close(p[0]);
		close(p[1]);
		return 101;
	}
	if (pid == 0) {
		close(p[0]);
		_exit(child(i, p[1]));
	}
	close(p[1]);
	while ((k = read(p[0], buf, sizeof buf)) > 0) {
		for (j = 0; j < k; j++)
			if (buf[j] != (char)t_pattern(off + j, i))
				bad = 1;
		off += k;
	}
	close(p[0]);
	if (t_waitchild(pid, &st, 30) != pid)
		return 102;
	if (bad || off != XFER)
		return 103;
	if (st != 0)
		return WIFSIGNALED(st) ? 200 + WTERMSIG(st) : WEXITSTATUS(st);
	return 0;
}

static void
snapshot(fm, as, v)
long *fm, *as;
struct statvfs *v;
{
	sync();
	sleep(1);
	*fm = t_kmem("freemem");
	*as = t_kmem("availsmem");
	statvfs("/", v);
}

int
main()
{
	struct statvfs v0, v1;
	long fm0, fm1, as0, as1, t0, n = 0, errs = 0;
	int r, first = 0, i;

	t_init("stress", 80);
	for (i = 0; i < 10; i++)	/* warm caches */
		round1(i);
	snapshot(&fm0, &as0, &v0);
	t0 = t_now_ms();
	while (t_now_ms() - t0 < 20000) {
		r = round1((int)n);
		if (r != 0) {
			if (errs++ == 0)
				first = r;
		}
		n++;
	}
	snapshot(&fm1, &as1, &v1);
	t_info("rounds", "%ld rounds in %ld ms", n, t_now_ms() - t0 - 1000);
	t_check("rounds_ok", errs == 0 && n > 20, "%ld of %ld rounds failed, first code %d", errs, n, first);
	if (fm0 == -1)
		t_skip("freemem_stable", "freemem unreadable");
	else {
		t_info("freemem", "%ld -> %ld pages; availsmem %ld -> %ld", fm0, fm1, as0, as1);
		t_check("freemem_stable", fm0 - fm1 < 64, "lost %ld pages over %ld rounds", fm0 - fm1, n);
		t_check("availsmem_stable", as0 - as1 < 64, "lost %ld pages over %ld rounds", as0 - as1, n);
	}
	t_check("fs_counts_stable", v0.f_bfree == v1.f_bfree && v0.f_ffree == v1.f_ffree,
	    "free blocks %ld -> %ld, inodes %ld -> %ld", (long)v0.f_bfree, (long)v1.f_bfree,
	    (long)v0.f_ffree, (long)v1.f_ffree);
	return t_done();
}
