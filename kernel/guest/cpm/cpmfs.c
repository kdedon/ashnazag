/*
 * cpmfs.c -- make CP/M 2.2 file systems and put files in them.
 *
 * Block pointers are little-endian, as CP/M-68K's BDOS keeps them on
 * disk.  K&R C; also built on the build host for its checks.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cpmfs.h"

static unsigned char skew6[26] = {
	0, 6, 12, 18, 24, 4, 10, 16, 22, 2, 8, 14, 20,
	1, 7, 13, 19, 25, 5, 11, 17, 23, 3, 9, 15, 21
};

int
cpm_format(size, f)
	long size;
	struct cpmfmt *f;
{
	memset((char *)f, 0, sizeof *f);
	f->size = size;
	if (size == 256256L) {
		f->spt = 26;
		f->bsh = 3;
		f->dsm = 242;
		f->drm = 63;
		f->al = 0xc000;
		f->off = 2;
		f->skew = skew6;
		return 0;
	}
	if (size % 16384 || size < CPM_HDMIN || size > CPM_HDMAX)
		return -1;
	f->spt = 128;
	f->bsh = 5;
	f->dsm = size / 4096 - 1;
	f->drm = 1023;
	f->al = 0xff00;
	f->exm = f->dsm > 255 ? 1 : 3;
	return 0;
}

/* the byte offset of a physical sector */
long
cpm_secoff(f, trk, sec)
	struct cpmfmt *f;
	long trk, sec;
{
	return (trk * f->spt + sec) * 128;
}

/* n bytes at off; 0, or -1 outside the image or on an I/O error */
int
cpm_io(d, off, buf, n, wr)
	struct cpmimg *d;
	long off;
	char *buf;
	int n, wr;
{
	int k;

	if (off < 0 || off + n > d->f.size || (wr && d->ro))
		return -1;
	if (d->mem) {
		if (wr)
			memcpy(d->mem + off, buf, n);
		else
			memcpy(buf, d->mem + off, n);
		return 0;
	}
	if (lseek(d->fd, off, 0) != off)
		return -1;
	if (wr)
		return write(d->fd, buf, n) == n ? 0 : -1;
	if ((k = read(d->fd, buf, n)) < 0)
		return -1;
	if (k < n)
		memset(buf + k, 0xe5, n - k);	/* past the end of a short file */
	return 0;
}

/* record r of the data area */
static int
recio(d, r, buf, wr)
	struct cpmimg *d;
	long r;
	char *buf;
	int wr;
{
	struct cpmfmt *f = &d->f;
	long s = r % f->spt;

	return cpm_io(d, cpm_secoff(f, f->off + r / f->spt, f->skew ? (long)f->skew[s] : s),
	    buf, 128, wr);
}

static int
dirio(d, i, e, wr)
	struct cpmimg *d;
	int i, wr;
	unsigned char *e;
{
	char rec[128];

	if (recio(d, (long)i / 4, rec, 0) < 0)
		return -1;
	if (!wr) {
		memcpy((char *)e, rec + i % 4 * 32, 32);
		return 0;
	}
	memcpy(rec + i % 4 * 32, (char *)e, 32);
	return recio(d, (long)i / 4, rec, 1);
}

/* an empty directory; a file image is first set to its full size */
int
cpm_mkfs(d)
	struct cpmimg *d;
{
	char rec[128];
	long r;

	if (!d->mem && ftruncate(d->fd, d->f.size) < 0)
		return -1;
	memset(rec, 0xe5, sizeof rec);
	for (r = 0; r < (d->f.drm + 1) / 4; r++)
		if (recio(d, r, rec, 1) < 0)
			return -1;
	return 0;
}

/* a Unix name as CP/M's 11 upper-case characters; -1 if it has no 8.3 form */
int
cpm_name(s, n)
	char *s, *n;
{
	char *dot = strrchr(s, '.');
	int i, k, c;

	memset(n, ' ', 11);
	for (i = k = 0; s[i]; i++) {
		c = s[i];
		if (s + i == dot) {
			if (k == 0)
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

/*
 * A file of len bytes; its last record padded with ^Z.  0, or -1 when
 * the name is taken or the directory or the disk is full.
 */
int
cpm_put(d, user, name, data, len)
	struct cpmimg *d;
	int user;
	char *name, *data;
	long len;
{
	struct cpmfmt *f = &d->f;
	int big = f->dsm > 255, nptr = big ? 8 : 16, bpr = 1 << f->bsh;
	long nrec = (len + 127) / 128, rpe = (long)nptr << f->bsh, start, r, n, ext;
	unsigned char e[32], *used;
	char rec[128];
	int i, j, k, b, nb, slot, nslot = 0, nblk = 0, ok = -1;

	if ((used = (unsigned char *)calloc(f->dsm + 1, 1)) == 0)
		return -1;
	for (b = 0; b < 16; b++)
		if (f->al & 0x8000 >> b)
			used[b] = 1;
	for (i = 0; i <= f->drm; i++) {
		if (dirio(d, i, e, 0) < 0)
			goto out;
		if (e[0] == 0xe5) {
			nslot++;
			continue;
		}
		if (e[0] == user && memcmp((char *)e + 1, name, 11) == 0)
			goto out;
		for (k = 0; k < nptr; k++) {
			b = big ? e[16 + 2 * k] | e[17 + 2 * k] << 8 : e[16 + k];
			if (b > 0 && b <= f->dsm)
				used[b] = 1;
		}
	}
	for (b = 0; b <= f->dsm; b++)
		nblk += !used[b];
	if (nblk < (nrec + bpr - 1) / bpr || nslot < (nrec ? (nrec + rpe - 1) / rpe : 1))
		goto out;
	slot = 0;
	b = 0;
	for (start = 0; start == 0 || start < nrec; start += rpe) {
		n = nrec - start < rpe ? nrec - start : rpe;
		ext = n ? (start + n - 1) / 128 : 0;
		memset((char *)e, 0, sizeof e);
		e[0] = user;
		memcpy((char *)e + 1, name, 11);
		e[12] = ext & 31;
		e[14] = ext >> 5;
		e[15] = n ? start + n - ext * 128 : 0;
		nb = (n + bpr - 1) / bpr;
		for (k = 0; k < nb; k++) {
			while (b <= f->dsm && used[b])
				b++;
			if (b > f->dsm)
				goto out;
			used[b] = 1;
			if (big) {
				e[16 + 2 * k] = b;
				e[17 + 2 * k] = b >> 8;
			} else
				e[16 + k] = b;
			for (j = 0; j < bpr && (r = start + (long)k * bpr + j) < nrec; j++) {
				memset(rec, 0x1a, sizeof rec);
				memcpy(rec, data + r * 128, len - r * 128 < 128 ? len - r * 128 : 128);
				if (recio(d, ((long)b << f->bsh) + j, rec, 1) < 0)
					goto out;
			}
		}
		for (;;) {
			if (slot > f->drm || dirio(d, slot, (unsigned char *)rec, 0) < 0)
				goto out;
			if ((unsigned char)rec[0] == 0xe5)
				break;
			slot++;
		}
		if (dirio(d, slot++, e, 1) < 0)
			goto out;
	}
	ok = 0;
out:
	free((char *)used);
	return ok;
}
