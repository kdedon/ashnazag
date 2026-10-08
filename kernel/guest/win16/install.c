/*
 * install.c -- `startwin -install SRC': the user's own Windows 3.1 files
 * into C:\WINDOWS, as Wabi set its users up.  Everything of Windows is
 * used but the core we have ourselves (KERNEL, USER, GDI, the timer and
 * TOOLHELP) and what only makes sense on a PC (the 386 enhanced mode
 * VxDs, DOS swappers, WINOLDAP).
 *
 * SRC is either an installed Windows directory (one with WIN.COM or
 * SYSTEM\USER.EXE, as copied from a PC) or the setup disks: a directory
 * holding SETUP.INF and the disks' files, together or in subdirectories
 * (DISK1, DISK2, ...).  Compressed files (SZDD, a name ending in `_')
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
	"system.ini", "system.src", "setup.exe", "setup.inf", "setup.hlp", "setup.txt",
	"expand.exe", "dosx.exe", "smartdrv.exe", "himem.sys", "emm386.exe", "ramdrive.sys",
	"mscdex.exe", "msd.exe", "msd.ini", "drwatson.exe", "winsetup.exe", "decompr.exe", 0
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

/* ---- SZDD ---- */

static int
expand(src, dst, namep)
	char *src, *dst;
	char *namep;		/* the name, whose last letter the header holds */
{
	FILE *in, *out;
	u8 h[14], win[4096];
	int pos = 4096 - 16, c, flags, i, k, len, off;
	u32 size, n = 0;

	if ((in = fopen(src, "rb")) == 0)
		return -1;
	if (fread(h, 1, 14, in) != 14 || memcmp(h, "SZDD\x88\xf0\x27\x33", 8) != 0) {
		fclose(in);
		return 1;	/* not compressed */
	}
	if (namep && namep[strlen(namep) - 1] == '_' && h[9]) {
		namep[strlen(namep) - 1] = h[9];
		lower(namep);
	}
	if (!dst) {
		fclose(in);
		return 0;
	}
	size = h[10] | h[11] << 8 | (u32)h[12] << 16 | (u32)h[13] << 24;
	if ((out = fopen(dst, "wb")) == 0) {
		fclose(in);
		return -1;
	}
	memset(win, ' ', sizeof win);
	while (n < size && (flags = getc(in)) != EOF) {
		for (i = 0; i < 8 && n < size; i++) {
			if (flags >> i & 1) {
				if ((c = getc(in)) == EOF)
					break;
				putc(c, out);
				win[pos] = c;
				pos = (pos + 1) & 4095;
				n++;
			} else {
				if ((c = getc(in)) == EOF || (k = getc(in)) == EOF)
					break;
				off = c | (k & 0xf0) << 4;
				len = (k & 15) + 3;
				while (len-- > 0 && n < size) {
					c = win[off];
					off = (off + 1) & 4095;
					putc(c, out);
					win[pos] = c;
					pos = (pos + 1) & 4095;
					n++;
				}
			}
		}
	}
	fclose(in);
	fclose(out);
	return n == size ? 0 : -1;
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
	fprintf(fp, "[keyboard]\r\ntype=4\r\n\r\n[drivers]\r\n\r\n[mci]\r\n");
	fclose(fp);
}

int
win_install(src, cdir)
	char *src, *cdir;
{
	char wdir[512], sdir[512], dst[600], name[64], *x;
	struct file *f;
	int i, n = 0, bad = 0, disks, r;
	struct stat st;

	if (stat(src, &st) < 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "startwin: %s is not a directory\n", src);
		return 1;
	}
	files = (struct file *)calloc(MAXF, sizeof *files);
	scan(src, 2, -1);
	disks = lookup("setup.inf") != 0;
	if (!disks && !lookup("user.exe") && !lookup("win.com") && !lookup("progman.exe")) {
		fprintf(stderr, "startwin: %s has neither Windows' setup disks nor an installed Windows\n", src);
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
		if (inlist(name, skip) || inlist(x, skipext))
			continue;
		if (strcmp(name, "win.src") == 0 || strcmp(name, "control.src") == 0)
			strcpy(x, ".ini");
		else if (strcmp(x, ".src") == 0)
			continue;
		if (f->insys < 0)
			f->insys = inlist(x, sysext);
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
	sprintf(dst, "%s/system.ini", wdir);
	if (access(dst, 0) != 0)
		writesysini(dst);
	sprintf(dst, "%s/progman.exe", wdir);
	printf("startwin: %d files of Windows in %s%s\n", n, wdir,
	    access(dst, 0) == 0 ? "" : " (no Program Manager among them)");
	return bad ? 1 : 0;
}
