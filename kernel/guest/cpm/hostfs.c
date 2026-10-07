/*
 * hostfs.c -- CP/M drives as host directories.
 *
 * Drive X: is the directory X (or x) under the environment's root;
 * user N > 0 is its subdirectory N.  A host name shows as 8.3 in upper
 * case when it fits (a leading dot makes the file SYS); other names
 * and anything not a regular file are left out.  Attributes and
 * protection are the file's mode:
 *	R/O	no write permission for us
 *	archive	others' execute bit, cleared by every write
 *	READ	no read bits for group and others (only the owner gets in)
 *	WRITE	no write bits for group and others
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include "bdos3.h"

char b3_root[1024];
static char drvpath[NDRV][1100];

#define	NFD	4
static struct {
	char	path[1400];
	int	fd, wr;
} fc[NFD];
static int fcnext;
static void lcclear();

void
hf_flush()
{
	int i;

	for (i = 0; i < NFD; i++) {
		if (fc[i].path[0])
			close(fc[i].fd);
		fc[i].path[0] = 0;
	}
	lcclear();
}

static void
fcdrop(path)
	char *path;
{
	int i;

	for (i = 0; i < NFD; i++)
		if (fc[i].path[0] && strcmp(fc[i].path, path) == 0) {
			close(fc[i].fd);
			fc[i].path[0] = 0;
		}
}

/* host name -> 8.3; 0, or -1 when it has no CP/M form */
int
hf_name(s, n, sys)
	char *s, *n;
	int *sys;
{
	char *dot;
	int i, k, c;

	*sys = *s == '.';
	if (*sys)
		s++;
	dot = strrchr(s, '.');
	memset(n, ' ', 11);
	for (i = k = 0; s[i]; i++) {
		c = s[i];
		if (s + i == dot) {
			if (k == 0 || s[i + 1] == 0)
				return -1;
			k = 8;
			continue;
		}
		if (c <= ' ' || c > '~' || strchr("<>.,;:=?*[]|/\\\"", c) ||
		    k == (dot && s + i > dot ? 11 : 8))
			return -1;
		n[k++] = c >= 'a' && c <= 'z' ? c - 32 : c;
	}
	return k ? 0 : -1;
}

/* 8.3 -> a new host name, lower case */
static void
hostname(n, sys, s)
	char *n, *s;
	int sys;
{
	int i;

	if (sys)
		*s++ = '.';
	for (i = 0; i < 11; i++) {
		if (i == 8 && n[8] != ' ')
			*s++ = '.';
		if (n[i] != ' ')
			*s++ = n[i] >= 'A' && n[i] <= 'Z' ? n[i] + 32 : n[i];
	}
	*s = 0;
}

/* drives present now; A: is made when missing */
int
hf_drives()
{
	struct stat sb;
	int d, v = 0, up;

	for (d = 0; d < NDRV; d++) {
		drvpath[d][0] = 0;
		for (up = 1; up >= 0; up--) {
			sprintf(drvpath[d], "%s/%c", b3_root, (up ? 'A' : 'a') + d);
			if (stat(drvpath[d], &sb) == 0 && (sb.st_mode & S_IFMT) == S_IFDIR)
				break;
			drvpath[d][0] = 0;
		}
		if (drvpath[d][0])
			v |= 1 << d;
	}
	return v;
}

/* user's directory on a drive; mk makes it */
int
hf_dir(drv, user, path, mk)
	int drv, user, mk;
	char *path;
{
	struct stat sb;

	if (drv < 0 || drv >= NDRV || !drvpath[drv][0])
		return -1;
	if (user == 0)
		strcpy(path, drvpath[drv]);
	else
		sprintf(path, "%s/%d", drvpath[drv], user);
	if (stat(path, &sb) == 0)
		return (sb.st_mode & S_IFMT) == S_IFDIR ? 0 : -1;
	if (!mk || mkdir(path, 0777) < 0)
		return -1;
	return 0;
}

static int
fill(path, sb, h)
	char *path;
	struct stat *sb;
	struct hfile *h;
{
	int m = sb->st_mode;

	h->ro = sb->st_uid == geteuid() ? !(m & 0200) : access(path, W_OK) < 0;
	h->arc = (m & S_IXOTH) != 0;
	h->pwmode = !(m & 044) ? XP_READ : !(m & 022) ? XP_WRITE : 0;
	h->size = sb->st_size;
	h->atime = sb->st_atime;
	h->mtime = sb->st_mtime;
	return 0;
}

/* fn(h, arg) on each file of a user area, or those named want; stops when fn returns nonzero */
static int
scan(drv, user, fn, arg, want)
	int drv, user;
	int (*fn)();
	char *arg, *want;
{
	char dir[1200], path[1500];
	struct hfile h;
	struct dirent *de;
	struct stat sb;
	DIR *dp;
	int r = 0;

	if (hf_dir(drv, user, dir, 0) < 0 || (dp = opendir(dir)) == 0)
		return 0;
	while (r == 0 && (de = readdir(dp)) != 0) {
		if (strlen(de->d_name) >= sizeof h.host ||
		    hf_name(de->d_name, h.name, &h.sys) < 0 || (want && memcmp(want, h.name, 11)))
			continue;
		sprintf(path, "%s/%s", dir, de->d_name);
		if (stat(path, &sb) < 0 || (sb.st_mode & S_IFMT) != S_IFREG)
			continue;
		strcpy(h.host, de->d_name);
		h.user = user;
		fill(path, &sb, &h);
		r = (*fn)(&h, arg);
	}
	closedir(dp);
	return r;
}

int
hf_scan(drv, user, fn, arg)
	int drv, user;
	int (*fn)();
	char *arg;
{
	return scan(drv, user, fn, arg, (char *)0);
}

/* recent lookups, checked by one stat */
#define	NLC	8
static struct {
	int	drv, user, sys;
	char	n[11];
	char	host[256];
} lc[NLC];
static int nlc, lcnext;

static void
lcclear()
{
	nlc = 0;
}

struct find {
	char		n[11];
	struct hfile	h;
};

static int
findone(h, arg)
	struct hfile *h;
	char *arg;
{
	struct find *p = (struct find *)arg;

	if (memcmp(h->name, p->n, 11))
		return 0;
	p->h = *h;
	return 1;
}

/* the file named n (8.3, attribute bits clear) */
int
hf_find(drv, user, n, h)
	int drv, user;
	char *n;
	struct hfile *h;
{
	char dir[1200], path[1500];
	struct find f;
	struct stat sb;
	int i;

	for (i = 0; i < nlc; i++)
		if (lc[i].drv == drv && lc[i].user == user && memcmp(lc[i].n, n, 11) == 0) {
			if (hf_dir(drv, user, dir, 0) < 0)
				break;
			sprintf(path, "%s/%s", dir, lc[i].host);
			if (stat(path, &sb) < 0 || (sb.st_mode & S_IFMT) != S_IFREG)
				break;
			memcpy(h->name, n, 11);
			strcpy(h->host, lc[i].host);
			h->user = user;
			h->sys = lc[i].sys;
			return fill(path, &sb, h);
		}
	memcpy(f.n, n, 11);
	if (scan(drv, user, findone, (char *)&f, n) == 0)
		return -1;
	*h = f.h;
	i = nlc < NLC ? nlc++ : (lcnext = (lcnext + 1) % NLC);
	lc[i].drv = drv;
	lc[i].user = user;
	lc[i].sys = h->sys;
	memcpy(lc[i].n, n, 11);
	strcpy(lc[i].host, h->host);
	return 0;
}

static void
hpath(drv, h, path)
	int drv;
	struct hfile *h;
	char *path;
{
	char dir[1200];

	if (hf_dir(drv, h->user, dir, 0) < 0)
		dir[0] = 0;
	sprintf(path, "%s/%s", dir, h->host);
}

int
hf_create(drv, user, n, h)
	int drv, user;
	char *n;
	struct hfile *h;
{
	char path[1500];
	struct stat sb;
	int fd;

	if (hf_dir(drv, user, path, 1) < 0)
		return -1;
	memcpy(h->name, n, 11);
	h->user = user;
	h->sys = 0;
	hostname(n, 0, h->host);
	sprintf(path + strlen(path), "/%s", h->host);
	if ((fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0666)) < 0)
		return -1;
	lcclear();
	fstat(fd, &sb);
	close(fd);
	return fill(path, &sb, h);
}

/* a descriptor for the file, kept open for later calls */
int
hf_open(drv, h, wr)
	int drv, wr;
	struct hfile *h;
{
	char path[1500];
	struct stat sb;
	int i, fd;

	hpath(drv, h, path);
	for (i = 0; i < NFD; i++)
		if (fc[i].path[0] && (fc[i].wr || !wr) && strcmp(fc[i].path, path) == 0)
			return fc[i].fd;
	if ((fd = open(path, wr ? O_RDWR : O_RDONLY)) < 0)
		return -1;
	fcntl(fd, F_SETFD, 1);
	if (wr && fstat(fd, &sb) == 0 && (sb.st_mode & S_IXOTH))
		fchmod(fd, sb.st_mode & 07776);
	fcdrop(path);
	i = fcnext;
	fcnext = (fcnext + 1) % NFD;
	if (fc[i].path[0])
		close(fc[i].fd);
	strcpy(fc[i].path, path);
	fc[i].fd = fd;
	fc[i].wr = wr;
	return fd;
}

int
hf_unlink(drv, h)
	int drv;
	struct hfile *h;
{
	char path[1500];

	hpath(drv, h, path);
	fcdrop(path);
	lcclear();
	return unlink(path);
}

int
hf_rename(drv, h, n)
	int drv;
	struct hfile *h;
	char *n;
{
	char from[1500], to[1500], host[16];

	hpath(drv, h, from);
	hostname(n, h->sys, host);
	strcpy(to, from);
	strcpy(strrchr(to, '/') + 1, host);
	fcdrop(from);
	lcclear();
	return rename(from, to);
}

/* R/O, SYS and archive */
int
hf_setattr(drv, h, ro, sys, arc)
	int drv, ro, sys, arc;
	struct hfile *h;
{
	char path[1500], to[1500];
	struct stat sb;
	int m;

	hpath(drv, h, path);
	fcdrop(path);
	if (stat(path, &sb) < 0)
		return -1;
	m = sb.st_mode & 07777;
	m = ro ? m & ~0222 : m | 0200;
	m = arc ? m | S_IXOTH : m & ~S_IXOTH;
	if (m != (sb.st_mode & 07777) && chmod(path, m) < 0)
		return -1;
	if (sys != h->sys) {
		lcclear();
		strcpy(to, path);
		hostname(h->name, sys, strrchr(to, '/') + 1);
		if (access(to, 0) == 0 || rename(path, to) < 0)
			return -1;
	}
	return 0;
}

/* function 103: password modes become group/other permissions */
int
hf_protect(drv, h, mode)
	int drv, mode;
	struct hfile *h;
{
	char path[1500];
	struct stat sb;
	int m, um;

	hpath(drv, h, path);
	if (stat(path, &sb) < 0 || sb.st_uid != geteuid())
		return -1;
	m = sb.st_mode & 07777;
	if (mode & XP_READ)
		m &= ~077;
	else if (mode & (XP_WRITE | XP_DELETE))
		m = (m & ~022) | 044;
	else {
		um = umask(0);
		umask(um);
		m |= 066 & ~um;
	}
	return chmod(path, m);
}

int
hf_truncate(drv, h, len)
	int drv;
	struct hfile *h;
	long len;
{
	char path[1500];

	hpath(drv, h, path);
	fcdrop(path);
	return truncate(path, (off_t)len);
}

/* free 128-byte records on the drive's file system */
long
hf_free(drv)
	int drv;
{
	struct statvfs sv;
	double r;

	if (drv < 0 || drv >= NDRV || !drvpath[drv][0] || statvfs(drvpath[drv], &sv) < 0)
		return 0;
	r = (double)sv.f_bavail * (sv.f_frsize ? sv.f_frsize : sv.f_bsize) / RECLEN;
	return r > 0x7fffffffL ? 0x7fffffffL : (long)r;
}

/* CP/M 3 stamp: days from 1978-01-01 as day 1 (little-endian), BCD hour, minute */
void
hf_stamp(t, b)
	long t;
	unsigned char *b;
{
	time_t tt = t;
	struct tm *tm = localtime(&tt);
	long y, days = 0;

	for (y = 78; y < tm->tm_year; y++)
		days += (y % 4) == 0 ? 366 : 365;
	days += tm->tm_yday + 1;
	b[0] = days;
	b[1] = days >> 8;
	b[2] = (tm->tm_hour / 10) << 4 | tm->tm_hour % 10;
	b[3] = (tm->tm_min / 10) << 4 | tm->tm_min % 10;
}

/*
 * A new A: holding the distribution: links to its files (read-only to
 * users), X.68K for each X.REL, and the program given.
 */
int
hf_mkdist(dir, dist, prg, prgname, prglen)
	char *dir, *dist, *prg, *prgname;
	int prglen;
{
	char path[1500], to[1500], n[11], host[16];
	struct dirent *de;
	DIR *dp;
	int sys, fd;

	if (mkdir(dir, 0777) < 0)
		return -1;
	if ((dp = opendir(dist)) != 0) {
		while ((de = readdir(dp)) != 0) {
			if (strlen(de->d_name) > 100 || hf_name(de->d_name, n, &sys) < 0 || sys)
				continue;
			sprintf(path, "%s/%s", dist, de->d_name);
			hostname(n, 0, host);
			sprintf(to, "%s/%s", dir, host);
			symlink(path, to);
			if (memcmp(n + 8, "REL", 3) == 0) {
				memcpy(n + 8, "68K", 3);
				hostname(n, 0, host);
				sprintf(to, "%s/%s", dir, host);
				symlink(path, to);
			}
		}
		closedir(dp);
	}
	sprintf(path, "%s/%s", dir, prgname);
	if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0)
		return -1;
	if (write(fd, prg, prglen) != prglen) {
		close(fd);
		return -1;
	}
	return close(fd);
}
