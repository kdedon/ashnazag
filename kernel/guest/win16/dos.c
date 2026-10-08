/*
 * dos.c -- the DOS a Win16 program sees: INT 21h (and KERNEL's
 * DOS3Call and file calls) over host files.
 *
 * Drives are host directories (drive_root); a DOS path is upper case
 * 8.3 and each of its components is matched against the host directory
 * without regard to case, so a host file's name keeps its case.  Host
 * names that are not 8.3 are not seen.  File handles are DOS handles
 * over host descriptors; 0-2 are the console (the log).
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include "w16.h"

#define	NFILE	128

char *drive_root[26];
int dos_drive = 2;			/* C: */
static char curdir[26][128];		/* without the drive, from the root: "" or "\\DIR" */
static int fds[NFILE];
static char fnames[NFILE][128];		/* DOS names, for messages */
static u32 dta0;			/* the DTA outside any task */
/* each task's own disk transfer address (far pointer) */
#define	dta	(*(curtask ? &curtask->t_dta : &dta0))
static int lasterr;

void
dos_init()
{
	int i;

	for (i = 0; i < NFILE; i++)
		fds[i] = -1;
	fds[0] = fds[1] = fds[2] = -2;	/* the console */
}

/* ---- names ---- */

int
w16_stricmp(a, b)
	char *a, *b;
{
	int ca, cb;

	for (;; a++, b++) {
		ca = *a >= 'a' && *a <= 'z' ? *a - 32 : (u8)*a;
		cb = *b >= 'a' && *b <= 'z' ? *b - 32 : (u8)*b;
		if (ca != cb || !ca)
			return ca - cb;
	}
}

int
w16_strnicmp(a, b, n)
	char *a, *b;
	int n;
{
	int ca, cb;

	for (; n > 0; a++, b++, n--) {
		ca = *a >= 'a' && *a <= 'z' ? *a - 32 : (u8)*a;
		cb = *b >= 'a' && *b <= 'z' ? *b - 32 : (u8)*b;
		if (ca != cb || !ca)
			return ca - cb;
	}
	return 0;
}

void
w16_upper(s)
	char *s;
{
	for (; *s; s++)
		if (*s >= 'a' && *s <= 'z')
			*s -= 32;
}

/* a host name as DOS sees it (upper case 8.3), 0 if it has none */
static int
dosname(h, out)
	char *h, *out;
{
	char *dot = strrchr(h, '.');
	int n, e;

	if (h[0] == '.')
		return 0;
	n = dot ? dot - h : strlen(h);
	e = dot ? strlen(dot + 1) : 0;
	if (n < 1 || n > 8 || e > 3 || (dot && strchr(h, '.') != dot))
		return 0;
	strcpy(out, h);
	w16_upper(out);
	if (strchr(out, ' '))
		return 0;
	return 1;
}

/* the full DOS path: drive, root, no . or .. */
int
dos_fullpath(p, out)
	char *p, *out;
{
	char buf[300], *s, *d, *comp[64];
	int drive = dos_drive, n = 0, i;

	if (p[0] && p[1] == ':') {
		drive = (p[0] | 0x20) - 'a';
		p += 2;
	}
	if (drive < 0 || drive >= 26)
		return 15;
	if (p[0] == '\\' || p[0] == '/')
		strncpy(buf, p, sizeof buf - 1);
	else
		sprintf(buf, "%s\\%.150s", curdir[drive], p);
	buf[sizeof buf - 1] = 0;
	for (s = buf; *s; s++)
		if (*s == '/')
			*s = '\\';
	w16_upper(buf);
	for (s = strtok(buf, "\\"); s; s = strtok((char *)0, "\\")) {
		if (strcmp(s, ".") == 0 || *s == 0)
			continue;
		if (strcmp(s, "..") == 0) {
			if (n > 0)
				n--;
			continue;
		}
		if (n < 64)
			comp[n++] = s;
	}
	d = out;
	*d++ = 'A' + drive;
	*d++ = ':';
	if (n == 0)
		*d++ = '\\';
	for (i = 0; i < n; i++) {
		if (d - out + strlen(comp[i]) + 2 > 127)
			return 3;
		*d++ = '\\';
		strcpy(d, comp[i]);
		d += strlen(d);
	}
	*d = 0;
	return 0;
}

/* the host entry in dir whose DOS name is name */
static int
lookup(dir, name, out)
	char *dir, *name, *out;
{
	DIR *dp;
	struct dirent *de;
	char dn[16];
	int found = 0;

	if ((dp = opendir(dir)) == 0)
		return 0;
	while ((de = readdir(dp)) != 0)
		if (dosname(de->d_name, dn) && strcmp(dn, name) == 0) {
			strcpy(out, de->d_name);
			found = 1;
			break;
		}
	closedir(dp);
	return found;
}

/* the host path of a DOS path: 0, or a DOS error */
int
dos_hostpath(p, host, size, create)
	char *p, *host;
	int size, create;
{
	char full[300], *s, *next, ent[300];
	int drive;

	if (dos_fullpath(p, full) != 0)
		return 3;
	drive = full[0] - 'A';
	if (!drive_root[drive])
		return 15;
	if (strlen(drive_root[drive]) + strlen(full) + 2 >= (unsigned)size)
		return 3;
	strcpy(host, drive_root[drive]);
	for (s = full + 3; *s; s = next) {
		next = strchr(s, '\\');
		if (next)
			*next++ = 0;
		else
			next = s + strlen(s);
		if (strlen(host) + strlen(s) + 2 >= (unsigned)size)
			return 3;
		if (lookup(host, s, ent)) {
			strcat(host, "/");
			strcat(host, ent);
		} else if (*next == 0 && create) {
			/* a new file: lower case, as the host likes it */
			char *e = host + strlen(host);

			*e++ = '/';
			strcpy(e, s);
			for (; *e; e++)
				if (*e >= 'A' && *e <= 'Z')
					*e += 32;
		} else
			return *next ? 3 : 2;
	}
	return 0;
}

static int
doserr(e)
	int e;
{
	switch (e) {
	case ENOENT: return 2;
	case ENOTDIR: return 3;
	case EMFILE: case ENFILE: return 4;
	case EACCES: case EPERM: case EROFS: case EISDIR: return 5;
	case EEXIST: return 80;
	case ENOSPC: return 39;
	}
	return 31;
}

static int
newhandle()
{
	int i;

	for (i = 5; i < NFILE; i++)
		if (fds[i] == -1)
			return i;
	return -1;
}

int
dos_hostfd(h)
	int h;
{
	return h >= 0 && h < NFILE && fds[h] >= 0 ? fds[h] : -1;
}

/* mode: DOS access byte (0 read, 1 write, 2 both; sharing bits ignored) */
int
dos_open(p, mode)
	char *p;
	int mode;
{
	char host[1024];
	int h, fd, e, fl;
	struct stat st;

	if ((e = dos_hostpath(p, host, sizeof host, 0)) != 0)
		return -e;
	if (stat(host, &st) == 0 && S_ISDIR(st.st_mode))
		return -5;
	fl = (mode & 3) == 0 ? O_RDONLY : (mode & 3) == 1 ? O_WRONLY : O_RDWR;
	if ((h = newhandle()) < 0)
		return -4;
	if ((fd = open(host, fl)) < 0) {
		/* a read-only file opened for both: read only, as a CD would refuse */
		return -doserr(errno);
	}
	fds[h] = fd;
	dos_fullpath(p, fnames[h]);
	return h;
}

int
dos_creat(p, attr)
	char *p;
	int attr;
{
	char host[1024];
	int h, fd, e;

	if ((e = dos_hostpath(p, host, sizeof host, 1)) != 0)
		return -e;
	if ((h = newhandle()) < 0)
		return -4;
	if ((fd = open(host, O_RDWR | O_CREAT | O_TRUNC, (attr & 1) ? 0444 : 0666)) < 0)
		return -doserr(errno);
	fds[h] = fd;
	dos_fullpath(p, fnames[h]);
	return h;
}

int
dos_close(h)
	int h;
{
	if (h < 0 || h >= NFILE || fds[h] == -1)
		return -6;
	if (fds[h] >= 0)
		close(fds[h]);
	fds[h] = h < 3 ? -2 : -1;
	return 0;
}

s32
dos_read(h, a, n)
	int h;
	u32 a, n;
{
	int r;

	if (h < 0 || h >= NFILE || fds[h] == -1)
		return -6;
	if (a + n > MSIZE)
		return -5;
	if (fds[h] == -2)
		return 0;
	r = read(fds[h], (char *)M + a, n);
	return r < 0 ? -doserr(errno) : r;
}

s32
dos_write(h, a, n)
	int h;
	u32 a, n;
{
	int r;

	if (h < 0 || h >= NFILE || fds[h] == -1)
		return -6;
	if (a + n > MSIZE)
		return -5;
	if (fds[h] == -2) {
		if (w16_debug)
			fwrite(M + a, 1, n, stderr);
		return n;
	}
	if (n == 0) {
		/* a zero-byte write truncates there */
		off_t o = lseek(fds[h], 0, SEEK_CUR);

		return ftruncate(fds[h], o) < 0 ? -doserr(errno) : 0;
	}
	r = write(fds[h], (char *)M + a, n);
	return r < 0 ? -doserr(errno) : r;
}

s32
dos_seek(h, off, whence)
	int h, whence;
	s32 off;
{
	off_t r;

	if (h < 0 || h >= NFILE || fds[h] == -1)
		return -6;
	if (fds[h] == -2)
		return 0;
	r = lseek(fds[h], (off_t)off, whence);
	return r < 0 ? -doserr(errno) : (s32)r;
}

int
dos_delete(p)
	char *p;
{
	char host[1024];
	int e;

	if ((e = dos_hostpath(p, host, sizeof host, 0)) != 0)
		return e;
	return unlink(host) < 0 ? doserr(errno) : 0;
}

/* ---- directory searches ---- */

#define	NSEARCH	16

struct search {
	char	dir[1024];
	char	pat[16];
	int	attr;
	long	pos;
	int	used;
};
static struct search srch[NSEARCH];
static int nextsrch;

/* DOS wildcard match of an 8.3 name */
static int
wild(pat, name)
	char *pat, *name;
{
	char pn[9], pe[4], nn[9], ne[4], *d;
	int i;

	memset(pn, ' ', 8); memset(pe, ' ', 3); memset(nn, ' ', 8); memset(ne, ' ', 3);
	for (i = 0; *pat && *pat != '.' && i < 8; pat++)
		if (*pat == '*') { while (i < 8) pn[i++] = '?'; } else pn[i++] = *pat;
	while (*pat && *pat != '.') pat++;
	if (*pat == '.') pat++;
	for (i = 0; *pat && i < 3; pat++)
		if (*pat == '*') { while (i < 3) pe[i++] = '?'; } else pe[i++] = *pat;
	d = strchr(name, '.');
	for (i = 0; *name && name != d && i < 8; name++)
		nn[i++] = *name;
	if (d)
		for (i = 0, d++; *d && i < 3; d++)
			ne[i++] = *d;
	for (i = 0; i < 8; i++)
		if (pn[i] != '?' && pn[i] != nn[i])
			return 0;
	for (i = 0; i < 3; i++)
		if (pe[i] != '?' && pe[i] != ne[i])
			return 0;
	return 1;
}

static void
dosdate(t, date, time)
	time_t t;
	u32 *date, *time;
{
	struct tm *tm = localtime(&t);

	*date = (tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday;
	*time = tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2;
}

/* the next match into the DTA: 0, or 18 none */
static int
findnext(id)
	int id;
{
	struct search *s;
	DIR *dp;
	struct dirent *de;
	char dn[16], path[1300];
	struct stat st;
	long n = 0;
	u32 a = lin(FPSEL(dta), FPOFF(dta)), date, time;
	int at;

	if (id < 0 || id >= NSEARCH || !srch[id].used || !a)
		return 18;
	s = &srch[id];
	if ((dp = opendir(s->dir)) == 0)
		return 18;
	while ((de = readdir(dp)) != 0) {
		if (n++ < s->pos)
			continue;
		s->pos++;
		if (!dosname(de->d_name, dn) || !wild(s->pat, dn))
			continue;
		sprintf(path, "%s/%s", s->dir, de->d_name);
		if (stat(path, &st) < 0)
			continue;
		at = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
		if (!(st.st_mode & 0222))
			at |= 1;
		if ((at & 0x10) && !(s->attr & 0x10))
			continue;
		closedir(dp);
		PB(a + 0x15, at);
		dosdate(st.st_mtime, &date, &time);
		PW(a + 0x16, time);
		PW(a + 0x18, date);
		PL(a + 0x1a, (u32)st.st_size);
		memset(M + a + 0x1e, 0, 13);
		strcpy((char *)M + a + 0x1e, dn);
		return 0;
	}
	closedir(dp);
	s->used = 0;
	return 18;
}

static int
findfirst(p, attr)
	char *p;
	int attr;
{
	char full[300], *slash;
	struct search *s;
	int id = nextsrch++ % NSEARCH, e;
	u32 a = lin(FPSEL(dta), FPOFF(dta));

	if (!a)
		return 2;
	if ((e = dos_fullpath(p, full)) != 0)
		return e;
	slash = strrchr(full, '\\');
	s = &srch[id];
	strncpy(s->pat, slash + 1, 15);
	s->pat[15] = 0;
	slash[slash == full + 2 ? 1 : 0] = 0;	/* "C:\" keeps its backslash */
	if ((e = dos_hostpath(full, s->dir, sizeof s->dir, 0)) != 0)
		return e == 2 ? 3 : e;
	s->attr = attr;
	s->pos = 0;
	s->used = 1;
	PB(a, id);
	PB(a + 1, 0x5a);
	if ((e = findnext(id)) != 0)
		return e == 18 ? 2 : e;	/* 2 file not found on the first */
	return 0;
}

/* ---- INT 21h ---- */

#define	AX	(c->r[R_AX] & 0xffff)
#define	AH	(c->r[R_AX] >> 8 & 0xff)
#define	AL	(c->r[R_AX] & 0xff)
#define	BX	(c->r[R_BX] & 0xffff)
#define	CX	(c->r[R_CX] & 0xffff)
#define	DX	(c->r[R_DX] & 0xffff)
#define	SETW(n, v)	(c->r[n] = (c->r[n] & ~0xffff) | ((v) & 0xffff))
#define	SETAL(v)	(c->r[R_AX] = (c->r[R_AX] & ~0xff) | ((v) & 0xff))

static void
carry(c, on)
	struct x86 *c;
	int on;
{
	u32 f = x86_flags(c);

	x86_setflags(c, on ? f | F_CF : f & ~F_CF);
}

static void
fail(c, e)
	struct x86 *c;
	int e;
{
	lasterr = e;
	SETW(R_AX, e);
	carry(c, 1);
}

static char *
dsdx(c)
	struct x86 *c;
{
	char *s = gptr(FP(c->s[S_DS].sel, DX));

	return s ? s : "";
}

int
dos_int21(c)
	struct x86 *c;
{
	char buf[300], host[1024], *s;
	s32 r;
	u32 a, date, dtime;
	time_t now;
	struct tm *tm;
	struct stat st;
	int e;

	carry(c, 0);
	if (w16_debug > 1)
		w16_log("INT 21h AX=%04x BX=%04x CX=%04x DX=%04x\n", AX, c->r[R_BX] & 0xffff, c->r[R_CX] & 0xffff,
		    c->r[R_DX] & 0xffff);
	switch (AH) {
	case 0x02:
		if (w16_debug)
			fputc(c->r[R_DX] & 0xff, stderr);
		return 1;
	case 0x09:
		if (w16_debug)
			for (s = dsdx(c); *s && *s != '$'; s++)
				fputc(*s, stderr);
		return 1;
	case 0x0e:
		if ((c->r[R_DX] & 0xff) < 26 && drive_root[c->r[R_DX] & 0xff])
			dos_drive = c->r[R_DX] & 0xff;
		SETAL(26);
		return 1;
	case 0x11:		/* FCB find first, next (volume labels): nothing */
	case 0x12:
		SETAL(0xff);
		return 1;
	case 0x19:
		SETAL(dos_drive);
		return 1;
	case 0x1a:
		dta = FP(c->s[S_DS].sel, DX);
		return 1;
	case 0x2f:
		x86_loadseg(c, S_ES, FPSEL(dta));
		SETW(R_BX, FPOFF(dta));
		return 1;
	case 0x25:		/* set vector: kept for INT n to reach (startwin.c) */
		{
			extern u32 pmvec[];

			pmvec[AL] = FP(c->s[S_DS].sel, DX);
		}
		return 1;
	case 0x35:
		{
			extern u32 pmvec[];

			x86_loadseg(c, S_ES, FPSEL(pmvec[AL]));
			SETW(R_BX, FPOFF(pmvec[AL]));
		}
		return 1;
	case 0x2a:
		now = time((time_t *)0);
		tm = localtime(&now);
		SETW(R_CX, tm->tm_year + 1900);
		SETW(R_DX, (tm->tm_mon + 1) << 8 | tm->tm_mday);
		SETAL(tm->tm_wday);
		return 1;
	case 0x2c:
		{
			u32 ms = w16_ticks();

			now = time((time_t *)0);
			tm = localtime(&now);
			SETW(R_CX, tm->tm_hour << 8 | tm->tm_min);
			SETW(R_DX, tm->tm_sec << 8 | (ms / 10 % 100));
		}
		return 1;
	case 0x2b:
	case 0x2d:
		SETAL(0);
		return 1;
	case 0x30:
		SETW(R_AX, 0x0005);	/* DOS 5.0 */
		SETW(R_BX, 0);
		SETW(R_CX, 0);
		return 1;
	case 0x33:
		c->r[R_DX] &= ~0xff;
		return 1;
	case 0x36:
		SETW(R_AX, 64);		/* sectors per cluster */
		SETW(R_BX, 0x7fff);	/* free clusters */
		SETW(R_CX, 512);
		SETW(R_DX, 0xffff);
		return 1;
	case 0x39:
	case 0x3a:
		if ((e = dos_hostpath(dsdx(c), host, sizeof host, AH == 0x39)) != 0) {
			fail(c, e);
			return 1;
		}
		if ((AH == 0x39 ? mkdir(host, 0777) : rmdir(host)) < 0)
			fail(c, doserr(errno));
		return 1;
	case 0x3b:
		if ((e = dos_fullpath(dsdx(c), buf)) != 0 ||
		    (e = dos_hostpath(buf, host, sizeof host, 0)) != 0) {
			fail(c, e == 2 ? 3 : e);
			return 1;
		}
		if (stat(host, &st) < 0 || !S_ISDIR(st.st_mode)) {
			fail(c, 3);
			return 1;
		}
		strcpy(curdir[buf[0] - 'A'], buf[3] ? buf + 2 : "");
		return 1;
	case 0x3c:
	case 0x5b:
		if (AH == 0x5b && dos_hostpath(dsdx(c), host, sizeof host, 0) == 0) {
			fail(c, 80);
			return 1;
		}
		r = dos_creat(dsdx(c), CX);
		if (r < 0)
			fail(c, -r);
		else
			SETW(R_AX, r);
		return 1;
	case 0x3d:
		r = dos_open(dsdx(c), AL);
		if (r < 0)
			fail(c, -r);
		else
			SETW(R_AX, r);
		return 1;
	case 0x3e:
		if ((r = dos_close(BX)) < 0)
			fail(c, -r);
		return 1;
	case 0x3f:
	case 0x40:
		a = lin(c->s[S_DS].sel, DX);
		if (!a && CX) {
			fail(c, 5);
			return 1;
		}
		r = AH == 0x3f ? dos_read(BX, a, CX) : dos_write(BX, a, CX);
		if (r < 0)
			fail(c, -r);
		else
			SETW(R_AX, r);
		return 1;
	case 0x41:
		if ((e = dos_delete(dsdx(c))) != 0)
			fail(c, e);
		return 1;
	case 0x42:
		r = dos_seek(BX, (s32)(CX << 16 | DX), AL);
		if (r < 0)
			fail(c, -r);
		else {
			SETW(R_AX, r);
			SETW(R_DX, r >> 16);
		}
		return 1;
	case 0x43:
		if ((e = dos_hostpath(dsdx(c), host, sizeof host, 0)) != 0) {
			fail(c, e);
			return 1;
		}
		if (stat(host, &st) < 0) {
			fail(c, doserr(errno));
			return 1;
		}
		if (AL == 0)
			SETW(R_CX, (S_ISDIR(st.st_mode) ? 0x10 : 0x20) | (st.st_mode & 0222 ? 0 : 1));
		return 1;
	case 0x44:
		switch (AL) {
		case 0x00:
			if (BX < NFILE && fds[BX] == -2)
				SETW(R_DX, 0x80d3);	/* the console */
			else if (BX < NFILE && fds[BX] >= 0)
				SETW(R_DX, fnames[BX][0] ? fnames[BX][0] - 'A' : dos_drive);
			else
				fail(c, 6);
			return 1;
		case 0x08:			/* removable: no */
			SETW(R_AX, 1);
			return 1;
		case 0x09:			/* remote: no */
			SETW(R_DX, 0);
			return 1;
		}
		fail(c, 1);
		return 1;
	case 0x45:
		if (BX >= NFILE || fds[BX] == -1 || (e = newhandle()) < 0) {
			fail(c, 6);
			return 1;
		}
		fds[e] = fds[BX] >= 0 ? dup(fds[BX]) : -2;
		strcpy(fnames[e], fnames[BX]);
		SETW(R_AX, e);
		return 1;
	case 0x46:
		if (BX >= NFILE || CX >= NFILE || fds[BX] == -1) {
			fail(c, 6);
			return 1;
		}
		dos_close(CX);
		fds[CX] = fds[BX] >= 0 ? dup(fds[BX]) : -2;
		return 1;
	case 0x47:
		e = (c->r[R_DX] & 0xff) ? (c->r[R_DX] & 0xff) - 1 : dos_drive;
		if (e >= 26 || !drive_root[e]) {
			fail(c, 15);
			return 1;
		}
		if ((a = lin(c->s[S_DS].sel, c->r[R_SI] & 0xffff)) != 0)
			strcpy((char *)M + a, curdir[e][0] ? curdir[e] + 1 : "");
		c->r[R_AX] = (c->r[R_AX] & ~0xffff) | 0x0100;	/* as DOS leaves it: AL 0 */
		return 1;
	case 0x4c:
		w16_exit(AL);
		return 1;
	case 0x4e:
		if ((e = findfirst(dsdx(c), CX)) != 0)
			fail(c, e);
		return 1;
	case 0x4f:
		a = lin(FPSEL(dta), FPOFF(dta));
		if (!a || M[a + 1] != 0x5a || (e = findnext(M[a])) != 0)
			fail(c, 18);
		return 1;
	case 0x56:
		{
			char h2[1024];
			u32 p2 = FP(c->s[S_ES].sel, c->r[R_DI] & 0xffff);

			if ((e = dos_hostpath(dsdx(c), host, sizeof host, 0)) != 0 ||
			    (e = dos_hostpath(gptr(p2) ? gptr(p2) : "", h2, sizeof h2, 1)) != 0) {
				fail(c, e);
				return 1;
			}
			if (rename(host, h2) < 0)
				fail(c, doserr(errno));
		}
		return 1;
	case 0x57:
		if (BX >= NFILE || fds[BX] < 0) {
			if (BX < NFILE && fds[BX] == -2 && AL == 0) {
				SETW(R_CX, 0);
				SETW(R_DX, 0);
				return 1;
			}
			fail(c, 6);
			return 1;
		}
		if (AL == 0) {
			fstat(fds[BX], &st);
			dosdate(st.st_mtime, &date, &dtime);
			SETW(R_CX, dtime);
			SETW(R_DX, date);
		}
		return 1;
	case 0x59:
		SETW(R_AX, lasterr);
		SETW(R_BX, 0);
		SETW(R_CX, 0);
		return 1;
	case 0x62:
	case 0x51:
		SETW(R_BX, curtask ? curtask->t_psp : 0);
		return 1;
	case 0x67:
	case 0x68:
		return 1;
	case 0x6c:
		{
			char *p = gptr(FP(c->s[S_DS].sel, c->r[R_SI] & 0xffff));
			int act = DX, ex;

			if (!p) {
				fail(c, 2);
				return 1;
			}
			ex = dos_hostpath(p, host, sizeof host, 0) == 0;
			if (ex && (act & 0x0f) == 0) {
				fail(c, 80);
				return 1;
			}
			if (!ex && (act & 0xf0) == 0) {
				fail(c, 2);
				return 1;
			}
			if (ex && (act & 0x0f) == 2)
				r = dos_creat(p, CX), e = 3;
			else if (ex)
				r = dos_open(p, BX & 3), e = 1;
			else
				r = dos_creat(p, CX), e = 2;
			if (r < 0)
				fail(c, -r);
			else {
				SETW(R_AX, r);
				SETW(R_CX, e);
			}
		}
		return 1;
	}
	w16_log("INT 21h AX=%04x not done\n", AX);
	fail(c, 1);
	return 1;
}

/* for the list box's LB_DIR */
int
dos_83(h, out)
	char *h, *out;
{
	return dosname(h, out);
}

int
dos_wild(pat, name)
	char *pat, *name;
{
	return wild(pat, name);
}

int
dos_isdir(host)
	char *host;
{
	struct stat st;

	return stat(host, &st) == 0 && S_ISDIR(st.st_mode);
}

int
dos_chdir(p)
	char *p;
{
	char full[300], host[1024];

	if (dos_fullpath(p, full) != 0 || dos_hostpath(full, host, sizeof host, 0) != 0 || !dos_isdir(host))
		return 3;
	strcpy(curdir[full[0] - 'A'], full[3] ? full + 2 : "");
	dos_drive = full[0] - 'A';
	return 0;
}
