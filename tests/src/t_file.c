/*
 * t_file.c -- regular files and directories on the s5 RAM-disk root.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <utime.h>
#include "t.h"

#define D	"/tmp/tf"

static int drop();

/* compare file contents with the pattern; returns first bad offset + 1, or 0 */
static long
verify(path, n, seed)
char *path;
long n;
int seed;
{
	char buf[1500];
	long off = 0;
	int fd, k, i;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	while ((k = read(fd, buf, sizeof buf)) > 0) {
		for (i = 0; i < k; i++)
			if (buf[i] != (char)t_pattern(off + i, seed)) {
				close(fd);
				return off + i + 1;
			}
		off += k;
	}
	close(fd);
	return off == n ? 0 : -2 - off;
}

static long
fsize(path)
char *path;
{
	struct stat sb;

	if (stat(path, &sb) == -1)
		return -1;
	return sb.st_size;
}

static void
test_sizes()
{
	static long sizes[] = { 0, 1, 511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049,
		3000, 4095, 4096, 4097, 6143, 6144, 6145, 8191, 8192, 8193,
		10239, 10240, 10241, 12288, 16385, -1 };
	char name[64], path[64];
	long r;
	int i, fd, w;

	for (i = 0; sizes[i] >= 0; i++) {
		sprintf(path, D "/s%ld", sizes[i]);
		sprintf(name, "size_%ld", sizes[i]);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			t_fail(name, "create: %s", T_ERR);
			continue;
		}
		w = t_fill(fd, (int)sizes[i], i);
		close(fd);
		r = verify(path, sizes[i], i);
		if (w != sizes[i])
			t_fail(name, "wrote %d: %s", w, T_ERR);
		else if (fsize(path) != sizes[i])
			t_fail(name, "st_size %ld", fsize(path));
		else
			t_check(name, r == 0, r > 0 ? "content differs at offset %ld" :
			    "short read (%ld)", r > 0 ? r - 1 : -2 - r);
	}
	/* rewrite in place across page halves: 4 KB file, then 1 KB at 1536 and 3584 */
	sprintf(path, D "/rw");
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	t_fill(fd, 8192, 40);
	lseek(fd, 1536L, 0);
	write(fd, "abcdefgh", 8);
	lseek(fd, 6000L, 0);
	write(fd, "ABCDEFGH", 8);
	close(fd);
	{
		char buf[8192];
		int ok = 1, k;
		fd = open(path, O_RDONLY);
		k = read(fd, buf, sizeof buf);
		close(fd);
		for (i = 0; i < 8192; i++) {
			int want = t_pattern((long)i, 40);
			if (i >= 1536 && i < 1544) want = "abcdefgh"[i - 1536];
			if (i >= 6000 && i < 6008) want = "ABCDEFGH"[i - 6000];
			if (buf[i] != (char)want) {
				ok = 0;
				break;
			}
		}
		t_check("overwrite_in_place", k == 8192 && ok, "read %d, first difference at %d", k, i);
	}
	/* append in 700-byte steps across the 2 KB / 4 KB boundaries, reopening each time */
	sprintf(path, D "/app");
	unlink(path);
	{
		long off = 0;
		char buf[700];
		int j, ok = 1;
		for (j = 0; j < 20; j++) {
			for (i = 0; i < 700; i++)
				buf[i] = t_pattern(off + i, 41);
			fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
			write(fd, buf, 700);
			close(fd);
			off += 700;
			if (verify(path, off, 41) != 0) {
				ok = 0;
				break;
			}
		}
		t_check("append_700_reopen", ok, "content wrong after %ld bytes (first bad offset %ld)",
		    off, verify(path, off, 41) - 1);
	}
}

static void
test_large()
{
	struct statvfs v0, v1;
	char path[64], buf[1024];
	long r, n = 600L * 1024;	/* single and double indirect blocks */
	int fd, k;

	statvfs("/", &v0);
	sprintf(path, D "/large");
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	k = t_fill(fd, (int)n, 3);
	if (k != n) {
		t_fail("large_600KB", "wrote %d of %ld: %s", k, n, T_ERR);
		close(fd);
		unlink(path);
		return;
	}
	r = verify(path, n, 3);
	t_check("large_600KB", r == 0, "content differs at %ld", r - 1);
	if (drop(path) == 0) {
		r = verify(path, n, 3);
		t_check("large_600KB_cold", r == 0, "content differs at %ld after dropping the pages", r - 1);
	}

	/* random-ish seeks */
	{
		static long offs[] = { 0, 1, 1023, 10239, 10240, 10241, 272383, 272384, 272385,
			400000, 599999, -1 };
		int i, ok = 1;
		for (i = 0; offs[i] >= 0; i++) {
			lseek(fd, offs[i], 0);
			if (read(fd, buf, 1) != 1 || buf[0] != (char)t_pattern(offs[i], 3)) {
				ok = 0;
				break;
			}
		}
		t_check("seek_read", ok, "offset %ld", offs[i]);
		t_check("seek_end", lseek(fd, 0L, 2) == n, "SEEK_END %ld", (long)lseek(fd, 0L, 2));
		lseek(fd, 100L, 0);
		t_check("seek_cur", lseek(fd, 50L, 1) == 150, "SEEK_CUR");
		t_check("read_at_eof", lseek(fd, 0L, 2) == n && read(fd, buf, 10) == 0, "read at EOF");
	}
	/* truncate (F_FREESP and ftruncate) */
	{
		struct flock fl;
		fl.l_whence = 0;
		fl.l_start = 5000;
		fl.l_len = 0;
		if (fcntl(fd, F_FREESP, &fl) == -1)
			t_fail("truncate_freesp", "%s", T_ERR);
		else
			t_check("truncate_freesp", fsize(path) == 5000 && verify(path, 5000L, 3) == 0,
			    "size %ld, verify %ld", fsize(path), verify(path, 5000L, 3));
		if (ftruncate(fd, 3000L) == -1)
			t_fail("ftruncate", "%s", T_ERR);
		else
			t_check("ftruncate", fsize(path) == 3000 && verify(path, 3000L, 3) == 0,
			    "size %ld", fsize(path));
		/* extend with a hole after truncate: bytes 3000..8191 must be zero */
		lseek(fd, 8191L, 0);
		write(fd, "z", 1);
		lseek(fd, 3000L, 0);
		k = read(fd, buf, 1024);
		{
			int i, ok = k == 1024;
			for (i = 0; i < k; i++)
				if (buf[i] != 0)
					ok = 0;
			t_check("truncate_then_extend_zero", ok, "stale data after truncate+extend (read %d)", k);
		}
	}
	close(fd);
	unlink(path);

	/* sparse file */
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	lseek(fd, 300000L, 0);
	write(fd, "E", 1);
	{
		struct stat sb;
		int i, ok = 1;
		fstat(fd, &sb);
		lseek(fd, 150000L, 0);
		read(fd, buf, sizeof buf);
		for (i = 0; i < (int)sizeof buf; i++)
			if (buf[i] != 0)
				ok = 0;
		t_check("sparse_hole_zero", ok && sb.st_size == 300001, "size %ld", (long)sb.st_size);
		t_info("sparse_blocks", "st_blocks %ld for a 300001-byte file with one byte", (long)sb.st_blocks);
	}
	close(fd);
	unlink(path);
	sync();
	statvfs("/", &v1);
	t_check("blocks_freed", v0.f_bfree == v1.f_bfree && v0.f_ffree == v1.f_ffree,
	    "free blocks %ld -> %ld, inodes %ld -> %ld", (long)v0.f_bfree, (long)v1.f_bfree,
	    (long)v0.f_ffree, (long)v1.f_ffree);
}

static void
test_misc_io()
{
	char path[64], buf[64];
	int fd, fd2, k;

	sprintf(path, D "/misc");
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
	t_check("create_excl", fd >= 0, "%s", T_ERR);
	t_check("create_excl_again", open(path, O_RDWR | O_CREAT | O_EXCL, 0600) == -1 && errno == EEXIST,
	    "errno %d", errno);
	write(fd, "0123456789", 10);
	fd2 = dup(fd);
	lseek(fd, 2L, 0);
	k = read(fd2, buf, 3);
	t_check("dup_shares_offset", k == 3 && memcmp(buf, "234", 3) == 0, "read %d", k);
	close(fd2);
	t_check("fsync", fsync(fd) == 0, "%s", T_ERR);
	close(fd);
	fd = open(path, O_WRONLY | O_APPEND);
	lseek(fd, 0L, 0);
	write(fd, "X", 1);
	close(fd);
	t_check("o_append", fsize(path) == 11, "size %ld", fsize(path));
	fd = open(path, O_WRONLY | O_TRUNC);
	close(fd);
	t_check("o_trunc", fsize(path) == 0, "size %ld", fsize(path));
	t_check("read_only_write", (fd = open(path, O_RDONLY)) >= 0 && write(fd, "a", 1) == -1 &&
	    errno == EBADF, "errno %d", errno);
	close(fd);
	unlink(path);
	t_check("enoent", open(D "/nope", O_RDONLY) == -1 && errno == ENOENT, "errno %d", errno);
	t_check("enotdir", open("/etc/passwd/x", O_RDONLY) == -1 && errno == ENOTDIR, "errno %d", errno);
}

static void
test_stat()
{
	struct stat sb;
	struct utimbuf ut;
	char path[64];
	time_t now = time((time_t *)0);
	int fd;

	sprintf(path, D "/st");
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0640);
	write(fd, "hello", 5);
	fstat(fd, &sb);
	close(fd);
	t_check("stat_mode", (sb.st_mode & 07777) == 0640 && S_ISREG(sb.st_mode), "mode 0%o", (int)sb.st_mode);
	t_check("stat_size_nlink", sb.st_size == 5 && sb.st_nlink == 1, "size %ld nlink %d",
	    (long)sb.st_size, (int)sb.st_nlink);
	t_check("stat_uid", sb.st_uid == getuid() && sb.st_gid == getgid(), "uid %d gid %d",
	    (int)sb.st_uid, (int)sb.st_gid);
	t_check("stat_mtime", sb.st_mtime >= now - 2 && sb.st_mtime <= now + 2, "mtime %ld now %ld",
	    (long)sb.st_mtime, (long)now);
	chmod(path, 0751);
	stat(path, &sb);
	t_check("chmod", (sb.st_mode & 07777) == 0751, "mode 0%o", (int)sb.st_mode);
	ut.actime = 700000000;
	ut.modtime = 700000001;
	utime(path, &ut);
	stat(path, &sb);
	t_check("utime", sb.st_atime == 700000000 && sb.st_mtime == 700000001, "atime %ld mtime %ld",
	    (long)sb.st_atime, (long)sb.st_mtime);
	stat("/", &sb);
	t_check("stat_root_dir", S_ISDIR(sb.st_mode) && sb.st_ino == 2, "mode 0%o ino %ld",
	    (int)sb.st_mode, (long)sb.st_ino);
	stat("/dev/null", &sb);
	t_check("stat_chr_dev", S_ISCHR(sb.st_mode) && ((sb.st_rdev >> 18) & 0x3fff) == 3 && (sb.st_rdev & 0x3ffff) == 2,
	    "mode 0%o rdev 0x%lx", (int)sb.st_mode, (long)sb.st_rdev);
	unlink(path);
}

static int
countdir(path)
char *path;
{
	DIR *d;
	struct dirent *de;
	int n = 0;

	d = opendir(path);
	if (d == 0)
		return -1;
	while ((de = readdir(d)) != 0)
		if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, ".."))
			n++;
	closedir(d);
	return n;
}

static void
test_dirs()
{
	struct stat sb;
	struct statvfs v0, v1;
	char path[64], p2[64], buf[64];
	int i, fd, ok, k;

	sync();
	statvfs("/", &v0);
	t_check("mkdir", mkdir(D "/d1", 0755) == 0, "%s", T_ERR);
	t_check("mkdir_eexist", mkdir(D "/d1", 0755) == -1 && errno == EEXIST, "errno %d", errno);
	stat(D "/d1", &sb);
	t_check("dir_nlink", S_ISDIR(sb.st_mode) && sb.st_nlink == 2, "nlink %d", (int)sb.st_nlink);
	mkdir(D "/d1/sub", 0755);
	stat(D "/d1", &sb);
	t_check("dir_nlink_sub", sb.st_nlink == 3, "nlink %d", (int)sb.st_nlink);

	/* 100 entries: the directory grows past one 1 KB block (64 entries) */
	ok = 1;
	for (i = 0; i < 100; i++) {
		sprintf(path, D "/d1/f%03d", i);
		fd = creat(path, 0644);
		if (fd < 0) {
			ok = 0;
			break;
		}
		close(fd);
	}
	t_check("create_100", ok, "creat %s: %s", path, T_ERR);
	k = countdir(D "/d1");
	t_check("readdir_101", k == 101, "readdir saw %d entries", k);
	t_check("rmdir_notempty", rmdir(D "/d1") == -1 && (errno == EEXIST || errno == ENOTEMPTY),
	    "errno %d", errno);

	/* rename in dir, across dirs, over an existing file */
	t_check("rename_same_dir", rename(D "/d1/f000", D "/d1/renamed") == 0 &&
	    access(D "/d1/f000", 0) == -1 && access(D "/d1/renamed", 0) == 0, "%s", T_ERR);
	t_check("rename_cross_dir", rename(D "/d1/renamed", D "/d1/sub/moved") == 0 &&
	    access(D "/d1/sub/moved", 0) == 0, "%s", T_ERR);
	fd = open(D "/d1/f001", O_WRONLY);
	write(fd, "new", 3);
	close(fd);
	t_check("rename_replace", rename(D "/d1/f001", D "/d1/f002") == 0 && fsize(D "/d1/f002") == 3 &&
	    access(D "/d1/f001", 0) == -1, "%s", T_ERR);
	t_check("rename_dir", rename(D "/d1/sub", D "/d1/sub2") == 0 && access(D "/d1/sub2/moved", 0) == 0,
	    "%s", T_ERR);
	stat(D "/d1/sub2/..", &sb);
	{
		struct stat s2;
		stat(D "/d1", &s2);
		t_check("rename_dir_dotdot", sb.st_ino == s2.st_ino, "'..' of renamed dir is ino %ld, want %ld",
		    (long)sb.st_ino, (long)s2.st_ino);
	}

	/* hard links */
	t_check("link", link(D "/d1/f002", D "/d1/hl") == 0, "%s", T_ERR);
	stat(D "/d1/f002", &sb);
	t_check("link_nlink2", sb.st_nlink == 2, "nlink %d", (int)sb.st_nlink);
	unlink(D "/d1/f002");
	stat(D "/d1/hl", &sb);
	t_check("unlink_nlink1", sb.st_nlink == 1 && sb.st_size == 3, "nlink %d size %ld",
	    (int)sb.st_nlink, (long)sb.st_size);

	/* symlinks */
	if (symlink("hl", D "/d1/sl") == -1)
		t_fail("symlink", "%s", T_ERR);
	else {
		k = readlink(D "/d1/sl", buf, sizeof buf);
		t_check("readlink", k == 2 && memcmp(buf, "hl", 2) == 0, "readlink %d", k);
		t_check("symlink_follow", fsize(D "/d1/sl") == 3, "size via link %ld", fsize(D "/d1/sl"));
		lstat(D "/d1/sl", &sb);
		t_check("lstat_symlink", (sb.st_mode & S_IFMT) == S_IFLNK, "mode 0%o", (int)sb.st_mode);
	}

	/* open file survives unlink */
	fd = open(D "/d1/hl", O_RDONLY);
	unlink(D "/d1/hl");
	k = read(fd, buf, 10);
	close(fd);
	t_check("unlinked_open_file", k == 3 && memcmp(buf, "new", 3) == 0, "read %d", k);

	/* cwd, relative paths */
	getcwd(p2, sizeof p2);
	chdir(D "/d1/sub2");
	getcwd(buf, sizeof buf);
	t_check("chdir_getcwd", strcmp(buf, D "/d1/sub2") == 0, "getcwd '%s'", buf);
	t_check("relative_dotdot", access("../f050", 0) == 0, "%s", T_ERR);
	chdir(p2);

	/* long name: s5 keeps 14 characters */
	fd = creat(D "/d1/abcdefghijklmnopq", 0644);
	close(fd);
	if (access(D "/d1/abcdefghijklmn", 0) == 0)
		t_info("name_14", "long name truncated to 14 characters");
	else
		t_info("name_14", "long name: creat %s", fd >= 0 ? "kept it" : strerror(errno));

	/* clean up */
	for (i = 0; i < 100; i++) {
		sprintf(path, D "/d1/f%03d", i);
		unlink(path);
	}
	unlink(D "/d1/sl");
	unlink(D "/d1/abcdefghijklmn");
	unlink(D "/d1/abcdefghijklmnopq");
	unlink(D "/d1/sub2/moved");
	t_check("rmdir_sub", rmdir(D "/d1/sub2") == 0, "%s", T_ERR);
	t_check("rmdir", rmdir(D "/d1") == 0, "%s", T_ERR);
	sync();
	statvfs("/", &v1);
	t_check("dir_blocks_freed", v0.f_bfree == v1.f_bfree && v0.f_ffree == v1.f_ffree,
	    "free blocks %ld -> %ld, inodes %ld -> %ld", (long)v0.f_bfree, (long)v1.f_bfree,
	    (long)v0.f_ffree, (long)v1.f_ffree);
}

/*
 * What write() leaves in the page cache must reach the disk: after fsync,
 * read the file's blocks back through the raw device, past the cache.
 * The upper 2 KB of each 4 KB page is rewritten separately.
 */
#define RAWDEV	"/dev/rdsk/rd0"
#define RAWSZ	8192

static int
raw_read(fd, off, buf, n)
int fd;
long off, n;
char *buf;
{
	return lseek(fd, (off_t)off, 0) != -1 && read(fd, buf, (unsigned)n) == n;
}

static void
test_raw_readback()
{
	struct stat sb, rb;
	struct statvfs v;
	char path[64], buf[2048], ino[512], blk[1024];
	unsigned char *a;
	long bs, bn, i, j, bad = -1;
	int fd, rfd, want;

	if (stat("/", &sb) == -1 || stat("/dev/dsk/rd0", &rb) == -1 || sb.st_dev != rb.st_rdev ||
	    statvfs("/", &v) == -1 || v.f_bsize > sizeof blk) {
		t_skip("raw_readback", "root is not rd0 with blocks of at most 1 KB");
		return;
	}
	bs = v.f_bsize;
	sprintf(path, D "/raw");
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	t_fill(fd, RAWSZ, 50);
	for (i = 2048; i < RAWSZ; i += 4096) {
		for (j = 0; j < 2048; j++)
			buf[j] = t_pattern(i + j, 51);
		lseek(fd, (off_t)i, 0);
		write(fd, buf, 2048);
	}
	fsync(fd);
	fstat(fd, &sb);
	close(fd);
	sync();

	rfd = open(RAWDEV, O_RDONLY);
	if (rfd < 0) {
		t_fail("raw_readback", "open " RAWDEV ": %s", T_ERR);
		return;
	}
	/* s5: 64-byte inodes from block 2, 3-byte block numbers at offset 12 */
	j = 2 * bs + (sb.st_ino - 1) * 64;
	if (!raw_read(rfd, j & ~511L, ino, 512L)) {
		t_fail("raw_readback", "inode read: %s", T_ERR);
		close(rfd);
		return;
	}
	a = (unsigned char *)ino + (j & 511) + 12;
	for (i = 0; i < RAWSZ / bs && bad < 0; i++) {
		bn = (long)a[3 * i] << 16 | a[3 * i + 1] << 8 | a[3 * i + 2];
		if (bn == 0 || !raw_read(rfd, bn * bs, blk, bs)) {
			bad = -2 - i;
			break;
		}
		for (j = 0; j < bs; j++) {
			want = t_pattern(i * bs + j, (i * bs + j) % 4096 >= 2048 ? 51 : 50);
			if (blk[j] != (char)want) {
				bad = i * bs + j;
				break;
			}
		}
	}
	close(rfd);
	if (bad <= -2)
		t_fail("raw_readback", "block %ld of the file unreadable on disk", -2 - bad);
	else
		t_check("raw_readback", bad < 0, "disk differs from written data at offset %ld", bad);
	unlink(path);
}

/*
 * Drop a file's pages from the cache: write them back and invalidate them
 * through a shared mapping, so the next read comes from the disk.
 */
static int
drop(path)
char *path;
{
	struct stat sb;
	caddr_t m;
	int fd, r = -1;

	fd = open(path, O_RDWR);
	if (fd < 0 || fstat(fd, &sb) == -1)
		return -1;
	if (sb.st_size == 0) {
		close(fd);
		return 0;
	}
	m = mmap((caddr_t)0, (size_t)sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0);
	if (m != (caddr_t)-1) {
		r = msync(m, (size_t)sb.st_size, MS_SYNC | MS_INVALIDATE);
		munmap(m, (size_t)sb.st_size);
	}
	close(fd);
	return r;
}

/* write n pattern bytes at off in pieces of step */
static void
put(fd, off, n, step, seed)
int fd, step, seed;
long off, n;
{
	char buf[4096];
	long i, k;

	for (; n > 0; off += k, n -= k) {
		k = n < step ? n : step;
		for (i = 0; i < k; i++)
			buf[i] = t_pattern(off + i, seed);
		lseek(fd, (off_t)off, 0);
		write(fd, buf, (unsigned)k);
	}
}

/* compare with seed a below split in each 4 KB page, seed b above; first bad offset + 1 */
static long
verify2(path, n, split, a, b)
char *path;
long n, split;
int a, b;
{
	char buf[1024];
	long off = 0;
	int fd, k, i;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	while ((k = read(fd, buf, sizeof buf)) > 0) {
		for (i = 0; i < k; i++)
			if (buf[i] != (char)t_pattern(off + i, (off + i) % 4096 < split ? a : b)) {
				close(fd);
				return off + i + 1;
			}
		off += k;
	}
	close(fd);
	return off == n ? 0 : -2 - off;
}

static void
test_cold()
{
	char path[64], buf[1024];
	long r;
	int fd, i, ok;

	sprintf(path, D "/cold");
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	put(fd, 0L, 100L, 100, 60);
	close(fd);
	if (drop(path) == -1) {
		t_skip("cold", "msync MS_INVALIDATE: %s", T_ERR);
		unlink(path);
		return;
	}

	/* pieces of 700 bytes past 3 KB, read back from disk */
	fd = open(path, O_RDWR | O_TRUNC);
	put(fd, 0L, 9000L, 700, 61);
	close(fd);
	drop(path);
	r = verify(path, 9000L, 61);
	t_check("cold_pieces_9000", r == 0, "differs at %ld", r - 1);

	/* extend from page offset 2048 with the page not cached */
	fd = open(path, O_RDWR | O_TRUNC);
	put(fd, 0L, 2048L, 1024, 62);
	close(fd);
	drop(path);
	fd = open(path, O_RDWR);
	put(fd, 2048L, 3000L, 3000, 62);
	close(fd);
	r = verify(path, 5048L, 62);
	t_check("cold_extend_2048_cached", r == 0, "differs at %ld", r - 1);
	drop(path);
	r = verify(path, 5048L, 62);
	t_check("cold_extend_2048", r == 0, "differs at %ld", r - 1);

	/* overwrite the upper half of each page, then read back from disk */
	fd = open(path, O_RDWR | O_TRUNC);
	put(fd, 0L, 8192L, 1024, 63);
	close(fd);
	drop(path);
	fd = open(path, O_RDWR);
	put(fd, 2048L, 2048L, 2048, 64);
	put(fd, 6144L, 2048L, 2048, 64);
	close(fd);
	drop(path);
	r = verify2(path, 8192L, 2048L, 63, 64);
	t_check("cold_upper_half", r == 0, "differs at %ld", r - 1);

	/* truncate, extend past a hole, read the hole from disk */
	fd = open(path, O_RDWR | O_TRUNC);
	put(fd, 0L, 5000L, 1024, 65);
	ftruncate(fd, 3000L);
	lseek(fd, 8191L, 0);
	write(fd, "z", 1);
	close(fd);
	drop(path);
	fd = open(path, O_RDONLY);
	r = -1;
	for (i = 0; i < 3000; i++)
		if ((i % sizeof buf == 0 && read(fd, buf, sizeof buf) != sizeof buf) ||
		    buf[i % sizeof buf] != (char)t_pattern((long)i, 65)) {
			r = i;
			break;
		}
	lseek(fd, 3000L, 0);
	ok = read(fd, buf, sizeof buf) == sizeof buf;
	for (i = 0; i < (int)sizeof buf; i++)
		if (buf[i] != 0)
			ok = 0;
	close(fd);
	t_check("cold_truncate_extend_zero", ok && r < 0, "hole %s, head differs at %ld",
	    ok ? "zero" : "not zero", r);
	unlink(path);
}

static void
test_statvfs()
{
	struct statvfs v;

	if (statvfs("/", &v) == -1) {
		t_fail("statvfs", "%s", T_ERR);
		return;
	}
	t_info("statvfs", "fstype %s bsize %ld frsize %ld blocks %ld bfree %ld files %ld ffree %ld",
	    v.f_basetype, (long)v.f_bsize, (long)v.f_frsize, (long)v.f_blocks, (long)v.f_bfree,
	    (long)v.f_files, (long)v.f_ffree);
	t_check("statvfs", v.f_blocks > 0 && v.f_bfree > 0 && v.f_bfree <= v.f_blocks &&
	    strcmp(v.f_basetype, "s5") == 0, "implausible values");
}

int
main()
{
	t_init("file", 80);
	mkdir(D, 0755);
	test_statvfs();
	test_sizes();
	test_raw_readback();
	test_cold();
	test_large();
	test_misc_io();
	test_stat();
	test_dirs();
	system("rm -rf " D);
	rmdir(D);
	return t_done();
}
