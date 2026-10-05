/*
 * t_ufs.c -- a ufs volume (SCSI disk 2, made by mkufs.py) mounted, used
 * and unmounted ten times, then remounted; after each unmount, file I/O
 * on the s5 root and exec must not reach the unmounted file system.
 *
 * The volume holds /d/f<k> (k = 0..3), files of 3000 + 9000 k bytes in
 * t_pattern(off, k).
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include "t.h"

#define DEV	"/dev/dsk/c2d0s0"
#define MNT	"/ufs"
#define NF	4
#define FSZ(k)	(3000 + 9000 * (k))
#define ROUNDS	10
#define S5F	"/tmp/tufs.big"
#define S5SZ	(768 * 1024)
#define SEF	"/tmp/tufs.se"
#define NSE	32

/* bytes of path matching t_pattern(off, seed), -1 if unreadable */
static long
same(path, n, seed)
char *path;
long n;
int seed;
{
	char buf[2000];
	long off = 0;
	int fd, k, i;

	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	while ((k = read(fd, buf, sizeof buf)) > 0) {
		for (i = 0; i < k; i++)
			if (buf[i] != (char)t_pattern(off + i, seed))
				break;
		off += i;
		if (i < k)
			break;
	}
	close(fd);
	return off == n ? n : off;
}

/* reads, writes, chown, mmap and directory work on the mounted volume */
static int
use(r)
int r;
{
	char p[64], q[64];
	struct stat sb;
	DIR *d;
	struct dirent *e;
	char *m;
	int k, fd, n, ok = 1;

	for (k = 0; k < NF; k++) {
		sprintf(p, MNT "/d/f%d", k);
		if (same(p, (long)FSZ(k), k) != FSZ(k)) {
			t_fail("read", "round %d: %s wrong", r, p);
			ok = 0;
		}
		if (chown(p, 101 + r, 3) < 0) {
			t_fail("chown", "round %d: %s: %s", r, p, T_ERR);
			ok = 0;
		}
	}
	/* a file written in the previous round survives the unmount */
	if (r > 0) {
		sprintf(p, MNT "/w%d", r - 1);
		if (same(p, 20000L, r + 8) != 20000) {
			t_fail("persist", "round %d: %s wrong", r, p);
			ok = 0;
		}
	}
	sprintf(p, MNT "/w%d", r);
	if ((fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0 ||
	    t_fill(fd, 20000, r + 10) != 20000) {
		t_fail("write", "round %d: %s: %s", r, p, T_ERR);
		ok = 0;
	}
	/* shared mapping: dirty pages left for the unmount to write */
	if (fd >= 0) {
		m = mmap((caddr_t)0, 20000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (m == (char *)-1) {
			t_fail("mmap", "round %d: %s", r, T_ERR);
			ok = 0;
		} else {
			for (n = 0; n < 20000; n++)
				m[n] = t_pattern((long)n, r + 9);
			munmap(m, 20000);
		}
		close(fd);
	}
	sprintf(q, MNT "/dir%d", r);
	if (mkdir(q, 0755) < 0 && errno != EEXIST) {
		t_fail("mkdir", "round %d: %s", r, T_ERR);
		ok = 0;
	}
	for (k = 0; k < 30; k++) {
		sprintf(p, "%s/a_long_file_name_%02d", q, k);
		close(creat(p, 0644));
		if (k & 1)
			unlink(p);
	}
	n = 0;
	if ((d = opendir(q)) != 0) {
		while ((e = readdir(d)) != 0)
			n++;
		closedir(d);
	}
	if (n != 17) {
		t_fail("readdir", "round %d: %d entries in %s, want 17", r, n, q);
		ok = 0;
	}
	if (stat(MNT "/d", &sb) < 0 || !S_ISDIR(sb.st_mode)) {
		t_fail("stat", "round %d: " MNT "/d", r);
		ok = 0;
	}
	return ok;
}

/* segmap faults for 4-byte writes at off in NSE fresh file-window slots */
static long
slotfaults(off)
long off;
{
	long c0, c1;
	int fd, k;

	if ((c0 = t_kmem("segmapcnt")) < 0 ||
	    (fd = open(SEF, O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0)
		return -1;
	for (k = 0; k < NSE; k++) {
		lseek(fd, (long)k * 8192 + off, 0);
		write(fd, "abcd", 4);
	}
	c1 = t_kmem("segmapcnt");
	close(fd);
	unlink(SEF);
	return c1 - c0;
}

/*
 * Extra faults of writes in the last 8 bytes of a slot over writes just
 * before them; the former must not fault on the next slot.
 * Neither reaches the slot's end, whose release starts a write-back.
 */
static long
slotend()
{
	long mid, end;

	if ((mid = slotfaults(8176L)) < 0 || (end = slotfaults(8184L)) < 0)
		return -1;
	return end - mid;
}

/* the s5 root's file pages and a fork and exec, with the volume gone */
static int
other(r)
int r;
{
	int fd, st;
	long n;
	pid_t p;

	if ((fd = open(S5F, O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0)
		return 0;
	n = t_fill(fd, S5SZ, r);
	close(fd);
	if (n != S5SZ || same(S5F, (long)S5SZ, r) != S5SZ) {
		unlink(S5F);
		return 0;
	}
	unlink(S5F);
	if ((p = fork()) == 0) {
		execl("/sbin/sh", "sh", "-c", "/sbin/sh -c :", (char *)0);
		_exit(127);
	}
	st = -1;
	return p > 0 && t_waitchild(p, &st, 30) == p && st == 0;
}

main()
{
	struct stat sb;
	int fd, r, ok;
	long n, se = -1;

	t_init("ufs", 120);
	if ((fd = open(DEV, O_RDONLY)) < 0) {
		t_skip("volume", "no ufs volume attached");
		return t_done();
	}
	close(fd);
	mkdir(MNT, 0755);
	for (r = 0; r < ROUNDS; r++) {
		t_rearm(120);
		if (system("/sbin/mount -F ufs " DEV " " MNT) != 0) {
			t_fail("mount", "round %d failed", r);
			break;
		}
		ok = use(r);
		if (system("/sbin/umount " MNT) != 0) {
			t_fail("umount", "round %d failed", r);
			break;
		}
		ok &= stat(MNT "/d", &sb) < 0;
		if ((n = slotend()) > se)
			se = n;
		if (!other(r)) {
			t_fail("after_umount", "round %d: s5 I/O or exec failed", r);
			ok = 0;
		}
		if (!ok)
			break;
	}
	t_check("rounds", r == ROUNDS, "stopped in round %d", r);
	if (se < 0)
		t_skip("slot_end", "no segmapcnt");
	else
		t_check("slot_end", se < NSE / 2, "%ld extra faults for %d writes at a slot's end", se, NSE);
	/* the flusher's scan of every page, with the volume unmounted */
	sleep(35);
	t_rearm(120);
	t_check("remount", system("/sbin/mount -F ufs -r " DEV " " MNT) == 0, "mount failed");
	t_check("remount_read", same(MNT "/d/f3", (long)FSZ(3), 3) == FSZ(3) &&
	    same(MNT "/w9", 20000L, 18) == 20000, "contents wrong");
	t_check("remount_umount", system("/sbin/umount " MNT) == 0, "umount failed");
	t_check("after_remount", other(ROUNDS), "s5 I/O or exec failed");
	return t_done();
}
