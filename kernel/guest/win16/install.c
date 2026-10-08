/*
 * install.c -- `startwin -install SRC': the user's own Windows 3.1 files
 * into C:\WINDOWS, as Wabi set its users up.  Everything of Windows is
 * used but the core we have ourselves (KERNEL, USER, GDI, the timer and
 * TOOLHELP) and what only makes sense on a PC (the 386 enhanced mode
 * VxDs, DOS swappers, WINOLDAP).
 *
 * SRC is an installed Windows directory (one with WIN.COM or
 * SYSTEM\USER.EXE, as copied from a PC), or the setup disks: their
 * images (FAT floppy images, as many as there are) or a directory
 * holding their files, together or in subdirectories (DISK1, ...).  Compressed files (SZDD or KWAJ, a name ending in `_')
 * are expanded.  Where SETUP.INF puts a file decides windows or system;
 * a file it does not list goes by its extension.  WIN.SRC becomes
 * WIN.INI; SYSTEM.INI is ours, naming our drivers.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "w16.h"

/* not copied: ours, or a PC's alone */
static char *skip[] = {
	"krnl286.exe", "krnl386.exe", "kernel.exe", "user.exe", "gdi.exe", "timer.drv", "toolhelp.dll",
	"winoldap.mod", "dswap.exe", "wswap.exe", "win386.exe", "win.com", "win.cnf", "vga.drv",
	"ega.drv", "vga.3gr", "vgalogo.lgo", "vgalogo.rle", "egalogo.lgo", "egalogo.rle",
	"keyboard.drv", "mouse.drv", "system.drv", "sound.drv", "comm.drv", "lmouse.drv",
	"system.ini", "system.src", "setup.exe", "setup.hlp", "setup.txt",
	"expand.exe", "dosx.exe", "smartdrv.exe", "himem.sys", "emm386.exe", "ramdrive.sys",
	"mscdex.exe", "msd.exe", "msd.ini", "drwatson.exe", "winsetup.exe", "decompr.exe",
	"cpwin386.cpl", 0
};
static char *skipext[] = { ".386", ".3gr", ".lgo", ".rle", ".sys", ".com", ".gr2", ".gr3", ".mod", 0 };

/* extensions that go in SYSTEM when SETUP.INF does not say */
static char *sysext[] = { ".dll", ".drv", ".fon", ".fot", ".ttf", ".cpl", ".acm", ".nls", 0 };

/* SETUP.INF sections that go in SYSTEM; the others listing files go in WINDOWS */
static char *syssect[] = { "windows.system", "win.other", "fonts", "ttfonts", "sysfonts",
	"fixedfonts", "oemfonts", "display", "keyboard.drivers", "pointing.device", "sound",
	"network", "drivers", "system", 0 };

#define	MAXF	1500

struct file {
	char	name[16];	/* lower case, expanded */
	char	path[512];	/* on the host */
	int	insys;		/* -1 not said, 0 windows, 1 system */
};
static struct file *files;
static int nfiles;

static void
lower(s)
	char *s;
{
	for (; *s; s++)
		if (*s >= 'A' && *s <= 'Z')
			*s += 32;
}

static char *
ext(s)
	char *s;
{
	char *p = strrchr(s, '.');

	return p ? p : "";
}

static int
inlist(s, l)
	char *s, **l;
{
	for (; *l; l++)
		if (strcmp(s, *l) == 0)
			return 1;
	return 0;
}

/* ---- Microsoft's compressed files: SZDD (COMPRESS -r) and KWAJ ---- */

static FILE *zin, *zout;
static u32 zbuf, zsize, zn;
static int zbits, zeof;
static u8 zwin[4096];
static int zpos;

static int
getbits(k)
	int k;
{
	int c;

	while (zbits < k) {
		if ((c = getc(zin)) == EOF) {
			zeof = 1;
			return 0;
		}
		zbuf = zbuf << 8 | c;
		zbits += 8;
	}
	zbits -= k;
	return zbuf >> zbits & ((1 << k) - 1);
}

static void
zput(c)
	int c;
{
	if (zn < zsize) {
		putc(c, zout);
		zn++;
	}
	zwin[zpos] = c;
	zpos = (zpos + 1) & 4095;
}

/* KWAJ's LZ+Huffman: a canonical code over n symbols, its lengths sent one of four ways */
struct huff {
	u8	len[256];
	int	n;
	u16	first[17], count[17], start[17];	/* by length: first code, how many, index in sym */
	u16	sym[256];
};

static void
hufflens(h, type, n)
	struct huff *h;
	int type, n;
{
	int i, c, s;

	h->n = n;
	switch (type) {
	case 0:
		for (i = 0; i < n; i++)
			h->len[i] = n == 16 ? 4 : n == 32 ? 5 : n == 64 ? 6 : 8;
		break;
	case 1:
		h->len[0] = c = getbits(4);
		for (i = 1; i < n; i++)
			if (getbits(1) == 0)
				h->len[i] = c;
			else if (getbits(1) == 0)
				h->len[i] = ++c;
			else
				h->len[i] = c = getbits(4);
		break;
	case 2:
		h->len[0] = c = getbits(4);
		for (i = 1; i < n; i++) {
			if ((s = getbits(2)) == 3)
				c = getbits(4);
			else
				c += s - 1;
			h->len[i] = c;
		}
		break;
	default:
		for (i = 0; i < n; i++)
			h->len[i] = getbits(4);
		break;
	}
	/* canonical codes: shorter first, then by symbol */
	{
		int l, code = 0, k = 0;

		for (l = 1; l <= 16; l++) {
			h->first[l] = code;
			h->start[l] = k;
			h->count[l] = 0;
			for (i = 0; i < n; i++)
				if (h->len[i] == l) {	/* a length that went below 0 (type 2): no code */
					h->sym[k++] = i;
					h->count[l]++;
					code++;
				}
			code <<= 1;
		}
	}
}

static int
huffsym(h)
	struct huff *h;
{
	int l, c = 0;

	for (l = 1; l <= 16 && !zeof; l++) {
		c = c << 1 | getbits(1);
		if (c - h->first[l] < h->count[l])
			return h->sym[h->start[l] + c - h->first[l]];
	}
	zeof = 1;
	return 0;
}

static void
kwajlzh()
{
	static struct huff t[5];
	static int nsym[5] = { 16, 16, 32, 64, 256 };
	int types[6], i, len, off, lit = 0;

	for (i = 0; i < 6; i++)
		types[i] = getbits(4);
	for (i = 0; i < 5; i++)
		hufflens(&t[i], types[i], nsym[i]);
	memset(zwin, ' ', sizeof zwin);
	zpos = 0;
	while (!zeof && zn < zsize) {
		len = huffsym(&t[lit ? 1 : 0]);
		if (len > 0) {
			len += 2;
			lit = 0;
			off = huffsym(&t[3]) << 6;
			off |= getbits(6);
			while (len-- > 0 && !zeof)
				zput(zwin[(zpos + 4096 - off) & 4095]);
		} else {
			len = huffsym(&t[2]) + 1;
			lit = len != 32;
			while (len-- > 0 && !zeof)
				zput(huffsym(&t[4]));
		}
	}
}

/* SZDD's LZSS: a flag byte, then eight literals or (offset, length) pairs */
static void
lzss()
{
	int flags, i, c, k, off, len;

	memset(zwin, ' ', sizeof zwin);
	zpos = 4096 - 16;
	while (zn < zsize && (flags = getc(zin)) != EOF)
		for (i = 0; i < 8 && zn < zsize; i++) {
			if (flags >> i & 1) {
				if ((c = getc(zin)) == EOF)
					return;
				zput(c);
			} else {
				if ((c = getc(zin)) == EOF || (k = getc(zin)) == EOF)
					return;
				off = c | (k & 0xf0) << 4;
				for (len = (k & 15) + 3; len > 0; len--, off = (off + 1) & 4095)
					zput(zwin[off]);
			}
		}
}

/*
 * src expanded into dst (when dst is not 0); namep, the file's name
 * ending in `_', gets its real name from the header.  1: src is not
 * compressed.
 */
static int
expand(src, dst, namep)
	char *src, *dst;
	char *namep;
{
	u8 h[14];
	char nm[16], ex[8], *p;
	int method, i, c, flags, data;

	if ((zin = fopen(src, "rb")) == 0)
		return -1;
	if (fread(h, 1, 14, zin) != 14) {
		fclose(zin);
		return 1;
	}
	if (memcmp(h, "SZDD\x88\xf0\x27\x33", 8) == 0) {
		method = -1;
		if (namep && namep[strlen(namep) - 1] == '_' && h[9]) {
			namep[strlen(namep) - 1] = h[9];
			lower(namep);
		}
		zsize = h[10] | h[11] << 8 | (u32)h[12] << 16 | (u32)h[13] << 24;
	} else if (memcmp(h, "KWAJ\x88\xf0\x27\xd1", 8) == 0) {
		method = h[8] | h[9] << 8;
		data = h[10] | h[11] << 8;
		flags = h[12] | h[13] << 8;
		zsize = 0xffffffff;
		if (flags & 1) {
			zsize = getc(zin);
			zsize |= getc(zin) << 8;
			zsize |= (u32)getc(zin) << 16;
			zsize |= (u32)getc(zin) << 24;
		}
		if (flags & 2)
			getc(zin), getc(zin);
		if (flags & 4) {
			c = getc(zin);
			c |= getc(zin) << 8;
			while (c-- > 0)
				getc(zin);
		}
		nm[0] = ex[0] = 0;
		if (flags & 8) {
			for (i = 0; (c = getc(zin)) > 0; i++)
				if (i < 8)
					nm[i] = c, nm[i + 1] = 0;
		}
		if (flags & 16) {
			for (i = 0; (c = getc(zin)) > 0; i++)
				if (i < 3)
					ex[i] = c, ex[i + 1] = 0;
		}
		if (namep && namep[strlen(namep) - 1] == '_') {
			if (nm[0])
				sprintf(namep, ex[0] ? "%s.%s" : "%s", nm, ex);
			else if ((p = strrchr(namep, '.')) != 0 && ex[0])
				strcpy(p + 1, ex);
			lower(namep);
		}
		fseek(zin, (long)data, 0);
		if (method > 3) {
			fclose(zin);
			fprintf(stderr, "startwin: %s: KWAJ method %d (MSZIP) not handled\n", src, method);
			return -1;
		}
	} else {
		fclose(zin);
		return 1;	/* not compressed */
	}
	if (!dst) {
		fclose(zin);
		return 0;
	}
	if ((zout = fopen(dst, "wb")) == 0) {
		fclose(zin);
		return -1;
	}
	zn = 0;
	zbits = 0;
	zeof = 0;
	switch (method) {
	case -1:
	case 2:
		lzss();
		break;
	case 0:
	case 1:
		while ((c = getc(zin)) != EOF && zn < zsize)
			putc(method ? c ^ 0xff : c, zout), zn++;
		break;
	case 3:
		kwajlzh();
		break;
	}
	fclose(zin);
	fclose(zout);
	return zn == zsize || (zsize == 0xffffffff && zn > 0) ? 0 : -1;
}

static int
copy(src, dst)
	char *src, *dst;
{
	FILE *in, *out;
	char buf[8192];
	int n;

	if ((in = fopen(src, "rb")) == 0)
		return -1;
	if ((out = fopen(dst, "wb")) == 0) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
	return 0;
}

/* ---- finding the files ---- */

static struct file *
lookup(name)
	char *name;
{
	int i;

	for (i = 0; i < nfiles; i++)
		if (strcmp(files[i].name, name) == 0)
			return &files[i];
	return 0;
}

/* every file under dir, depth levels down; a name seen already (an earlier disk's) stays */
static void
scan(dir, depth, insys)
	char *dir;
	int depth, insys;
{
	DIR *d;
	struct dirent *e;
	struct stat st;
	char p[512], name[300];
	struct file *f;

	if ((d = opendir(dir)) == 0)
		return;
	while ((e = readdir(d)) != 0) {
		if (e->d_name[0] == '.' || strlen(dir) + strlen(e->d_name) + 2 > sizeof p)
			continue;
		sprintf(p, "%s/%s", dir, e->d_name);
		if (stat(p, &st) < 0)
			continue;
		strncpy(name, e->d_name, sizeof name - 1);
		name[sizeof name - 1] = 0;
		lower(name);
		if (S_ISDIR(st.st_mode)) {
			if (depth > 0)
				scan(p, depth - 1, strcmp(name, "system") == 0 ? 1 : insys);
			continue;
		}
		if (strlen(name) > 12 || nfiles >= MAXF)
			continue;
		expand(p, (char *)0, name);	/* the real name of a compressed file */
		if (lookup(name))
			continue;
		f = &files[nfiles++];
		strcpy(f->name, name);
		strcpy(f->path, p);
		f->insys = insys;
	}
	closedir(d);
}

/* SETUP.INF: which section lists each file */
static void
readinf(path)
	char *path;
{
	FILE *fp;
	char line[512], sect[64], name[64], *p, *q;
	struct file *f;
	int sys = 0;

	if ((fp = fopen(path, "r")) == 0)
		return;
	sect[0] = 0;
	while (fgets(line, sizeof line, fp)) {
		for (p = line; *p == ' ' || *p == '\t'; p++)
			;
		if (*p == '[') {
			if ((q = strchr(p, ']')) != 0)
				*q = 0;
			strncpy(sect, p + 1, sizeof sect - 1);
			sect[sizeof sect - 1] = 0;
			lower(sect);
			sys = inlist(sect, syssect);
			continue;
		}
		if (*p == ';' || !sect[0])
			continue;
		/* `n:file.ext, ...' : the disk, then the file */
		if (!(*p >= '0' && *p <= '9') || (q = strchr(p, ':')) == 0 || q - p > 2)
			continue;
		p = q + 1;
		for (q = name; *p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
		    q < name + sizeof name - 1; p++)
			*q++ = *p;
		*q = 0;
		lower(name);
		if ((f = lookup(name)) != 0 && f->insys < 0)
			f->insys = sys;
	}
	fclose(fp);
}

/* ---- FAT disk images ---- */

static u8 *img;
static long imgsize;
static int bps, spc, nfat, nroot, spf, rootsec, datasec, fat16;

static int
fat12(c)
	int c;
{
	u8 *f = img + bps * (img[14] | img[15] << 8);
	int v = f[c * 3 / 2] | f[c * 3 / 2 + 1] << 8;

	if (fat16)
		return f[2 * c] | f[2 * c + 1] << 8;
	return c & 1 ? v >> 4 : v & 0xfff;
}

/* a directory's entries (n of them at d, or a chain from cluster c) into host dir out */
static int
fatdir(d, n, c, out)
	u8 *d;
	int n, c;
	char *out;
{
	u8 buf[32 * 512];
	int i, k, cl, left, eoc = fat16 ? 0xfff8 : 0xff8;
	long size, at;
	char name[16], p[600];
	FILE *fp;

	if (!d) {
		/* a subdirectory: gather its clusters (small ones only) */
		for (n = 0, cl = c; cl >= 2 && cl < eoc && n + bps * spc / 32 <= (int)(sizeof buf / 32); cl = fat12(cl)) {
			at = (long)(datasec + (cl - 2) * spc) * bps;
			if (at + bps * spc > imgsize)
				return -1;
			memcpy(buf + n * 32, img + at, bps * spc);
			n += bps * spc / 32;
		}
		d = buf;
	}
	for (i = 0; i < n; i++, d += 32) {
		if (d[0] == 0)
			break;
		if (d[0] == 0xe5 || d[0] == '.' || (d[11] & 0x08) || d[11] == 0x0f)
			continue;
		for (k = 0; k < 8 && d[k] != ' '; k++)
			name[k] = d[k];
		if (d[8] != ' ') {
			name[k++] = '.';
			for (c = 8; c < 11 && d[c] != ' '; c++)
				name[k++] = d[c];
		}
		name[k] = 0;
		sprintf(p, "%s/%s", out, name);
		cl = d[26] | d[27] << 8;
		if (d[11] & 0x10) {
			mkdir(p, 0755);
			fatdir((u8 *)0, 0, cl, p);
			continue;
		}
		size = d[28] | d[29] << 8 | (long)d[30] << 16 | (long)d[31] << 24;
		if ((fp = fopen(p, "wb")) == 0)
			return -1;
		for (left = size; left > 0 && cl >= 2 && cl < eoc; cl = fat12(cl)) {
			at = (long)(datasec + (cl - 2) * spc) * bps;
			k = left < bps * spc ? left : bps * spc;
			if (at + k > imgsize)
				break;
			fwrite(img + at, 1, k, fp);
			left -= k;
		}
		fclose(fp);
	}
	return 0;
}

/* the files of a FAT image into the host directory out */
static int
fatimage(path, out)
	char *path, *out;
{
	FILE *fp;
	int nsec, r;

	if ((fp = fopen(path, "rb")) == 0)
		return -1;
	fseek(fp, 0L, 2);
	imgsize = ftell(fp);
	fseek(fp, 0L, 0);
	if (imgsize < 2048 || imgsize > 64L << 20 || (img = (u8 *)malloc(imgsize)) == 0) {
		fclose(fp);
		return -1;
	}
	r = fread(img, 1, imgsize, fp) == imgsize ? 0 : -1;
	fclose(fp);
	bps = img[11] | img[12] << 8;
	spc = img[13];
	nfat = img[16];
	nroot = img[17] | img[18] << 8;
	spf = img[22] | img[23] << 8;
	nsec = img[19] | img[20] << 8;
	if (r || (bps != 512 && bps != 1024 && bps != 2048) || !spc || !nfat || !spf || !nroot) {
		free(img);
		return -1;
	}
	if (!nsec)
		nsec = img[32] | img[33] << 8 | (long)img[34] << 16;
	rootsec = (img[14] | img[15] << 8) + nfat * spf;
	datasec = rootsec + (nroot * 32 + bps - 1) / bps;
	fat16 = (nsec - datasec) / spc >= 4085;
	mkdir(out, 0755);
	r = fatdir(img + (long)rootsec * bps, nroot, 0, out);
	free(img);
	return r;
}

/* remove what fatimage made */
static void
rmtree(dir)
	char *dir;
{
	DIR *d;
	struct dirent *e;
	struct stat st;
	char p[600];

	if ((d = opendir(dir)) != 0) {
		while ((e = readdir(d)) != 0) {
			if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
				continue;
			sprintf(p, "%s/%s", dir, e->d_name);
			if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
				rmtree(p);
			else
				unlink(p);
		}
		closedir(d);
	}
	rmdir(dir);
}

/* what Setup adds to WIN.INI: the country settings (United States) */
static void
addintl(path)
	char *path;
{
	FILE *fp;
	char line[256];

	if ((fp = fopen(path, "r")) != 0) {
		while (fgets(line, sizeof line, fp))
			if (strncmp(line, "[intl]", 6) == 0) {
				fclose(fp);
				return;
			}
		fclose(fp);
	}
	if ((fp = fopen(path, "a")) == 0)
		return;
	fprintf(fp, "\r\n[intl]\r\nsLanguage=enu\r\nsCountry=United States\r\niCountry=1\r\n");
	fprintf(fp, "iDate=0\r\niTime=0\r\niTLZero=0\r\niCurrency=0\r\niCurrDigits=2\r\n");
	fprintf(fp, "iNegCurr=0\r\niLzero=0\r\niDigits=2\r\niMeasure=1\r\ns1159=AM\r\ns2359=PM\r\n");
	fprintf(fp, "sCurrency=$\r\nsThousand=,\r\nsDecimal=.\r\nsDate=/\r\nsTime=:\r\nsList=,\r\n");
	fprintf(fp, "sShortDate=M/d/yy\r\nsLongDate=dddd, MMMM dd, yyyy\r\n");
	fclose(fp);
}

/*
 * What Setup writes in WIN.INI's [fonts] for a VGA display: the entries of
 * SETUP.INF's [fonts] for the 96 dpi resolution ("100,96,96") and the
 * plotter fonts, each "description=FILE", then the TrueType fonts of its
 * [ttfonts] ("Arial (TrueType)=ARIAL.FOT").
 */
static void
addfonts(path, inf)
	char *path, *inf;
{
	FILE *fp, *in;
	char line[512], desc[128], file[32], *p, *q;
	int sect = 0, n = 0;

	if ((fp = fopen(path, "r")) != 0) {
		while (fgets(line, sizeof line, fp))
			if (strncmp(line, "[fonts]", 7) == 0) {
				fclose(fp);
				return;
			}
		fclose(fp);
	}
	if (!inf || (in = fopen(inf, "r")) == 0 || (fp = fopen(path, "a")) == 0) {
		if (in)
			fclose(in);
		return;
	}
	fprintf(fp, "\r\n[fonts]\r\n");
	while (fgets(line, sizeof line, in)) {
		for (p = line; *p == ' ' || *p == '\t'; p++)
			;
		if (*p == '[') {
			sect = strncmp(p, "[fonts]", 7) == 0 ? 1 : strncmp(p, "[ttfonts]", 9) == 0 ? 2 : 0;
			continue;
		}
		if (!sect || (sect == 1 && !(strstr(p, "\"100,96,96\"") || strstr(p, "CONTINUOUSSCALING"))))
			continue;
		/* n:FILE.FON, "description", "resolution"; n:FILE.FOT, "description", n:file.ttf, "" */
		if (*p == ';' || !(q = strchr(p, ':')))
			continue;
		p = q + 1;
		for (q = file; *p && *p != ',' && *p != ' ' && q < file + sizeof file - 1; p++)
			*q++ = *p;
		*q = 0;
		if (!(p = strchr(p, '"')))
			continue;
		for (p++, q = desc; *p && *p != '"' && q < desc + sizeof desc - 1; p++)
			*q++ = *p;
		*q = 0;
		fprintf(fp, "%s=%s\r\n", desc, file);
		n++;
	}
	fclose(in);
	fclose(fp);
	(void)n;
}

static void
writesysini(path)
	char *path;
{
	FILE *fp;

	if ((fp = fopen(path, "w")) == 0)
		return;
	fprintf(fp, "[boot]\r\nshell=progman.exe\r\nnetwork.drv=\r\n");
	fprintf(fp, "display.drv=display.drv\r\nkeyboard.drv=keyboard.drv\r\nmouse.drv=mouse.drv\r\n");
	fprintf(fp, "sound.drv=sound.drv\r\ncomm.drv=comm.drv\r\nsystem.drv=system.drv\r\n");
	fprintf(fp, "fonts.fon=vgasys.fon\r\nfixedfon.fon=vgafix.fon\r\noemfonts.fon=vgaoem.fon\r\n");
	fprintf(fp, "language.dll=\r\ndrivers=mmsystem.dll\r\n\r\n");
	fprintf(fp, "[boot.description]\r\ndisplay.drv=Ash Nazag display\r\n\r\n");
	fprintf(fp, "[keyboard]\r\ntype=4\r\n\r\n");
	/* the multimedia drivers as Setup lists them; the timer and the wave output are ours (mmdrv.c) */
	fprintf(fp, "[drivers]\r\ntimer=timer.drv\r\nwave=ashaudio.drv\r\nmidimapper=midimap.drv\r\n\r\n");
	fprintf(fp, "[mci]\r\nWaveAudio=mciwave.drv\r\nSequencer=mciseq.drv\r\n");
	fclose(fp);
}

int
win_install(nsrc, src, cdir)
	int nsrc;
	char **src, *cdir;
{
	char wdir[512], sdir[512], dst[700], name[64], tmp[600], *x;
	struct file *f;
	int i, n = 0, bad = 0, disks, r, nimg = 0;
	struct stat st;

	files = (struct file *)calloc(MAXF, sizeof *files);
	sprintf(tmp, "%s/temp/install", cdir);
	rmtree(tmp);
	for (i = 0; i < nsrc; i++) {
		if (stat(src[i], &st) < 0) {
			perror(src[i]);
			return 1;
		}
		if (S_ISDIR(st.st_mode)) {
			scan(src[i], 2, -1);
			continue;
		}
		/* a disk image: its files to a scratch directory, scanned like the others */
		mkdir(tmp, 0755);
		sprintf(dst, "%s/%d", tmp, ++nimg);
		if (fatimage(src[i], dst) != 0) {
			fprintf(stderr, "startwin: %s is not a FAT disk image\n", src[i]);
			rmtree(tmp);
			return 1;
		}
		scan(dst, 2, -1);
	}
	disks = lookup("setup.inf") != 0;
	if (!disks && !lookup("user.exe") && !lookup("win.com") && !lookup("progman.exe")) {
		fprintf(stderr, "startwin: neither Windows' setup disks nor an installed Windows\n");
		rmtree(tmp);
		return 1;
	}
	if (disks)
		readinf(lookup("setup.inf")->path);
	sprintf(wdir, "%s/windows", cdir);
	sprintf(sdir, "%s/windows/system", cdir);
	mkdir(cdir, 0755);
	mkdir(wdir, 0755);
	mkdir(sdir, 0755);
	for (i = 0; i < nfiles; i++) {
		f = &files[i];
		strcpy(name, f->name);
		x = ext(name);
		/* skipped too: the disks' labels (DISK1 ...) */
		if (inlist(name, skip) || inlist(x, skipext) ||
		    (strncmp(name, "disk", 4) == 0 && name[4] >= '0' && name[4] <= '9' && !*x))
			continue;
		if (strcmp(name, "win.src") == 0 || strcmp(name, "control.src") == 0)
			strcpy(x, ".ini");
		else if (strcmp(x, ".src") == 0)
			continue;
		if (f->insys < 0)
			f->insys = inlist(x, sysext);
		/* SETUP.INF stays in SYSTEM, as Setup leaves it: Program Manager's groups come from it */
		if (strcmp(name, "setup.inf") == 0)
			f->insys = 1;
		sprintf(dst, "%s/%s", f->insys ? sdir : wdir, name);
		/* an ini the user has changed stays */
		if (strcmp(x, ".ini") == 0 && access(dst, 0) == 0)
			continue;
		r = expand(f->path, dst, (char *)0);
		if (r == 1)
			r = copy(f->path, dst);
		if (r != 0) {
			fprintf(stderr, "startwin: %s: cannot copy\n", f->path);
			bad++;
			continue;
		}
		chmod(dst, 0644);
		n++;
	}
	rmtree(tmp);
	sprintf(dst, "%s/win.ini", wdir);
	addintl(dst);
	{
		char inf[700];

		sprintf(inf, "%s/setup.inf", sdir);
		addfonts(dst, access(inf, 0) == 0 ? inf : (char *)0);
	}
	sprintf(dst, "%s/system.ini", wdir);
	if (access(dst, 0) != 0)
		writesysini(dst);
	sprintf(dst, "%s/progman.exe", wdir);
	printf("startwin: %d files of Windows in %s%s\n", n, wdir,
	    access(dst, 0) == 0 ? "" : " (no Program Manager among them)");
	return bad ? 1 : 0;
}
