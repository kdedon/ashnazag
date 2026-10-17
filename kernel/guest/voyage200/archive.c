/*
 * archive.c -- archived variables as host files, one per variable in
 * TI's variable file format.  AMS keeps them as entries in the archive
 * blocks of the running copy: the files are laid out there at start
 * and written back as the entries change.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "profile.h"

extern int mkdir();

#define R(a) (*(__volatile__ unsigned short *)(a))
#define WR(a, w) (*(__volatile__ unsigned short *)(TI_ALIAS + ((a) - TI_FLASH)) = (w))
#define BLOCK 0x10000UL
#define TOP (TI_FLASH + TI_FLASH_SIZE)
#define HDR 22			/* entry: status, folder, name, symbol word, length */
#define E_OK 0xffe0
#define E_NEW 0xfff0
#define E_GONE 0xffc0
#define B_USED 0xfffe
#define B_FULL 0xfffc
#define B_OLD 0x0000
#define MAXV 512
#define SIG "**TI92P*"

unsigned long arclo = TOP;	/* the archive blocks the host uses: [arclo, TOP) */
static char dir[1100];
static char *known[MAXV];	/* files loaded or written */
static int nknown;

/* the bytes of the entry at a, 0 if it runs past its block */
static unsigned long
elen(a, b)
	unsigned long a, b;
{
	unsigned long n = ((R(a + 20) & 0x7fffUL) + 10) * 2;

	return a + n > b + BLOCK ? 0 : n;
}

/* a block AMS's archive format accounts for: blank, or entries to its end */
static int
shaped(b)
	unsigned long b;
{
	unsigned long a, n;
	int s = R(b);

	if (s == 0xffff) {
		for (a = b; a < b + BLOCK; a += 2)
			if (R(a) != 0xffff)
				return 0;
		return 1;
	}
	if (s != B_USED && s != B_FULL && s != B_OLD)
		return 0;
	for (a = b + 2; a < b + BLOCK && (s = R(a)) != 0xffff; a += n)
		if ((s != E_OK && s != E_NEW && s != E_GONE) || (n = elen(a, b)) == 0)
			return s == E_NEW;
	return 1;
}

static int
isknown(name)
	char *name;
{
	int i;

	for (i = 0; i < nknown; i++)
		if (strcmp(known[i], name) == 0)
			return 1;
	return 0;
}

static void
remember(name)
	char *name;
{
	char *s;

	if (nknown < MAXV && !isknown(name) && (s = malloc(strlen(name) + 1)) != 0)
		known[nknown++] = strcpy(s, name);
}

/* TI's type ID and file suffix from the variable's tag, its last byte */
static int
vtype(tag, suffix)
	int tag;
	char *suffix;
{
	static unsigned char tags[] = { 0x2d, 0xd9, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xf3, 0xf8, 0 };
	static unsigned char ids[] = { 0x0c, 0x04, 0x06, 0x12, 0x0a, 0x0d, 0x10, 0x0b, 0x0e, 0x14, 0x21, 0x1c, 0x00 };
	static char suffixes[] = "slmpcditaxzye";
	int i;

	for (i = 0; tags[i] && tags[i] != tag; i++)
		;
	*suffix = suffixes[i];
	return ids[i];
}

/* a name of up to 8 bytes as a file name part */
static void
part(out, in)
	char *out;
	unsigned char *in;
{
	int i;

	for (i = 0; i < 8 && in[i]; i++)
		out[i] = in[i] == '/' || in[i] <= ' ' || in[i] == '.' ? '_' : in[i];
	out[i] = 0;
}

static void
le(p, v, n)
	unsigned char *p;
	unsigned long v;
	int n;
{
	while (n-- > 0) {
		*p++ = v;
		v >>= 8;
	}
}

/* the entry at a as a variable file; its name into name */
static int
export(a, name, buf)
	unsigned long a;
	char *name;
	unsigned char *buf;
{
	unsigned char folder[8], var[8], *d = (unsigned char *)(a + HDR);
	unsigned long n = R(a + HDR) + 2UL, sum, i;
	char f[9], v[9], suffix;
	int type;

	if (n + HDR > elen(a, a & ~(BLOCK - 1)) || n < 3)
		return -1;
	memcpy(folder, (char *)(a + 2), 8);
	memcpy(var, (char *)(a + 10), 8);
	type = vtype(d[n - 1], &suffix);
	part(f, folder);
	part(v, var);
	if (!v[0])
		return -1;
	sprintf(name, "%s%s%s.v2%c", strcmp(f, "main") && f[0] ? f : "",
	    strcmp(f, "main") && f[0] ? "." : "", v, suffix);
	memset(buf, 0, 0x56);
	memcpy(buf, SIG, 8);
	buf[8] = 1;
	memcpy(buf + 0x0a, folder, 8);
	buf[0x3a] = 1;
	le(buf + 0x3c, 0x52UL, 4);
	memcpy(buf + 0x40, var, 8);
	buf[0x48] = type;
	buf[0x49] = 2;		/* archived */
	le(buf + 0x4c, 0x58 + n, 4);
	buf[0x50] = 0xa5;
	buf[0x51] = 0x5a;
	memcpy(buf + 0x56, (char *)d, n);
	for (sum = i = 0; i < n; i++)
		sum += d[i];
	le(buf + 0x56 + n, sum, 2);
	return (int)(0x58 + n);
}

static int
same(path, buf, n)
	char *path;
	unsigned char *buf;
	int n;
{
	static unsigned char old[0x10000 + 0x60];
	int fd, m;

	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	m = read(fd, (char *)old, sizeof old);
	close(fd);
	return m == n && memcmp(old, buf, n) == 0;
}

static void
put(name, buf, n)
	char *name;
	unsigned char *buf;
	int n;
{
	char path[1200], tmp[1200];
	int fd;

	sprintf(path, "%s/%s", dir, name);
	if (!isknown(name) && access(path, F_OK) == 0)	/* a file not loaded whole stays as it is */
		return;
	remember(name);
	if (same(path, buf, n))
		return;
	sprintf(tmp, "%s/.%s.new", dir, name);
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
		return;
	if (write(fd, (char *)buf, n) != n || close(fd) < 0 || rename(tmp, path) < 0)
		unlink(tmp);
}

/* the files match the valid entries: new and changed ones written, gone ones removed */
void
arc_save()
{
	static unsigned char buf[0x10000 + 0x60];
	static char *now[MAXV];
	char name[40], path[1200];
	unsigned long b, a, n;
	int i, j, m, nnow = 0, over = 0;

	if (!dir[0])
		return;
	for (b = arclo; b < TOP; b += BLOCK) {
		if (R(b) != B_USED && R(b) != B_FULL)
			continue;
		for (a = b + 2; a < b + BLOCK && R(a) != 0xffff && (n = elen(a, b)) != 0; a += n)
			if (R(a) == E_OK && (m = export(a, name, buf)) > 0) {
				put(name, buf, m);
				if (nnow < MAXV && (now[nnow] = malloc(strlen(name) + 1)) != 0)
					strcpy(now[nnow++], name);
				else
					over = 1;	/* untracked: nothing is judged gone */
			}
	}
	for (i = 0; !over && i < nknown; i++) {
		for (j = 0; j < nnow && strcmp(known[i], now[j]); j++)
			;
		if (j < nnow)
			continue;
		sprintf(path, "%s/%s", dir, known[i]);
		unlink(path);
		free(known[i]);
		known[i--] = known[--nknown];
	}
	while (nnow > 0)
		free(now[--nnow]);
}

static void
wbytes(a, p, n)
	unsigned long a;
	unsigned char *p;
	unsigned long n;
{
	for (; n >= 2; n -= 2, a += 2, p += 2)
		WR(a, p[0] << 8 | p[1]);
	if (n)
		WR(a, p[0] << 8 | 0xff);
}

static unsigned long cur, pos;	/* the block being filled, the next entry */

/* one variable as an entry */
static int
place(folder, var, d, n)
	unsigned char *folder, *var, *d;
	unsigned long n;
{
	unsigned long len = HDR + ((n + 1) & ~1UL);
	unsigned char h[HDR];

	if (len > BLOCK - 2)
		return -1;
	if (!cur || pos + len > cur + BLOCK) {
		if ((cur ? cur : TOP) - BLOCK < arclo + BLOCK)	/* a blank block stays for AMS's collection */
			return -1;
		cur = (cur ? cur : TOP) - BLOCK;
		WR(cur, B_USED);
		pos = cur + 2;
	}
	memset(h, 0, sizeof h);
	h[0] = E_OK >> 8;
	h[1] = E_OK & 0xff;
	memcpy(h + 2, folder, 8);
	memcpy(h + 10, var, 8);
	h[20] = 0x80 | ((n + 3) >> 1) >> 8;
	h[21] = (n + 3) >> 1;
	wbytes(pos, h, (unsigned long)HDR);
	wbytes(pos + HDR, d, n);
	pos += len;
	return 0;
}

struct var { unsigned char folder[8], name[8]; };
static struct var placed[MAXV];
static int nplaced;

/* the variables of one file, in TI's format (one or a group) */
static void
load(name)
	char *name;
{
	static unsigned char buf[0x40000];
	char path[1200];
	unsigned char folder[8], *e;
	unsigned long size, off, next, n, sum;
	int fd, m, count, i, k, whole = 1;

	sprintf(path, "%s/%s", dir, name);
	if ((fd = open(path, O_RDONLY)) < 0)
		return;
	m = read(fd, (char *)buf, sizeof buf);
	close(fd);
	if (m < 0x58 || (memcmp(buf, SIG, 8) && memcmp(buf, "**TI89**", 8)))
		return;
	count = buf[0x3a] | buf[0x3b] << 8;
	if (0x3c + 16 * count + 6 > m)
		return;
	memcpy(folder, buf + 0x0a, 8);
	e = buf + 0x3c + 16 * count;
	size = e[0] | e[1] << 8 | (unsigned long)e[2] << 16 | (unsigned long)e[3] << 24;
	off = buf[0x3c] | buf[0x3d] << 8 | (unsigned long)buf[0x3e] << 16 | (unsigned long)buf[0x3f] << 24;
	if (size > m || size < off + 2 || off < 0x3c) {
		fprintf(stderr, "voyage200: %s: bad size\n", name);
		return;
	}
	for (sum = 0, k = off; k < size - 2; k++)
		sum += buf[k];
	if ((sum & 0xffff) != (buf[size - 2] | buf[size - 1] << 8)) {
		fprintf(stderr, "voyage200: %s: bad checksum\n", name);
		return;
	}
	for (i = 0; i < count; i++) {
		e = buf + 0x3c + 16 * i;
		off = e[0] | e[1] << 8 | (unsigned long)e[2] << 16 | (unsigned long)e[3] << 24;
		if (e[12] == 0x1f) {
			memcpy(folder, e + 4, 8);
			continue;
		}
		for (next = size, k = i + 1; k < count; k++) {
			e = buf + 0x3c + 16 * k;
			if (e[12] != 0x1f) {
				next = e[0] | e[1] << 8 | (unsigned long)e[2] << 16 | (unsigned long)e[3] << 24;
				break;
			}
		}
		e = buf + 0x3c + 16 * i;
		if (off + 6 > next || next > size) {
			whole = 0;
			continue;
		}
		n = buf[off + 4] << 8 | buf[off + 5];
		if (n + 2 > next - off - 6) {
			whole = 0;
			continue;
		}
		for (k = 0; k < nplaced; k++)
			if (!memcmp(placed[k].folder, folder, 8) && !memcmp(placed[k].name, e + 4, 8))
				break;
		if (k < nplaced || nplaced >= MAXV) {
			whole = 0;
			continue;
		}
		if (place(folder, e + 4, buf + off + 4, n + 2) < 0) {
			fprintf(stderr, "voyage200: archive full at %s\n", name);
			whole = 0;
			break;
		}
		memcpy(placed[nplaced].folder, folder, 8);
		memcpy(placed[nplaced++].name, e + 4, 8);
	}
	if (whole)
		remember(name);
}

/*
 * The archive at start: the dump's variables become files the first
 * time, then the variable blocks are freed and every file laid out.
 */
void
arc_load(root)
	char *root;
{
	struct dirent *d;
	unsigned long b, a;
	DIR *dp;

	for (b = TOP - BLOCK; b > TI_FLASH && shaped(b); b -= BLOCK)
		arclo = b;
	if (arclo >= TOP - BLOCK)
		return;
	sprintf(dir, "%s/archive", root);
	if (mkdir(dir, 0755) == 0)
		arc_save();
	else if (errno != EEXIST) {
		fprintf(stderr, "voyage200: %s: cannot create archive folder\n", dir);
		dir[0] = 0;
		return;
	}
	for (b = arclo; b < TOP; b += BLOCK)
		if (R(b) != 0xffff)
			for (a = b; a < b + BLOCK; a += 2)
				WR(a, 0xffff);
	if ((dp = opendir(dir)) == 0)
		return;
	while ((d = readdir(dp)) != 0)
		if (d->d_name[0] != '.')
			load(d->d_name);
	closedir(dp);
}
