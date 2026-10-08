/*
 * wabicfg.c -- WABICFG, the configuration interface of Wabi's engine,
 * which Wabi's own Win16 files call: its Configuration Manager
 * (CPLCFG.CPL), PWI (the localised resources of its printer drivers,
 * installer and the rest), Wabi Registration.  The configuration is
 * Wabi's, WABI.INI beside WIN.INI: the keys of a platform's section, here
 * [Unknown], Wabi's own for a platform it did not know; their defaults
 * are those of the WABI.INI Wabi shipped (W:\WBIN\WABI.INI).  The
 * session's drives come from its Drives.X keys (startwin.c), and a change
 * to one the Configuration Manager tells us of maps the drive again.
 *
 * Values are accepted as given: Wabi's validation codes (its SVA_ names)
 * are not known by number.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "w16.h"

#define	INI	"C:\\WINDOWS\\WABI.INI"
#define	DEFINI	"W:\\WBIN\\WABI.INI"
#define	SECT	"Unknown"
#define	NONE	"\001"
#define	ENOTFOUND 1		/* any not 0: the caller only tests it */

extern char **environ;
static char remote[26], remoteok;	/* drives WABI.INI calls Remote: read once, again when one changes */

/* a key's value in an ini, its [Unknown] then [CommonSettings]; 0 if neither has it */
static int
getkey(ini, key, out, size)
	char *ini, *key, *out;
	int size;
{
	profile_get(ini, SECT, key, NONE, out, (u32)size);
	if (strcmp(out, NONE) == 0)
		profile_get(ini, "CommonSettings", key, NONE, out, (u32)size);
	if (strcmp(out, NONE) == 0) {
		out[0] = 0;
		return 0;
	}
	return 1;
}

/* the host directory of $WABIHOME: drive W:'s */
static char *
wabihome()
{
	return drive_root['W' - 'A'] ? drive_root['W' - 'A'] : "";
}

/* a value with $NAME and ${NAME} put in (WABIHOME ours, the rest the environment's) */
void
wabi_expand(v, out, size)
	char *v, *out;
	int size;
{
	char name[64], cwd[512], *e, *q = out;
	int n;

	while (*v && q < out + size - 1) {
		if (*v != '$') {
			*q++ = *v++;
			continue;
		}
		v++;
		n = 0;
		if (*v == '{') {
			for (v++; *v && *v != '}' && n < (int)sizeof name - 1; v++)
				name[n++] = *v;
			if (*v == '}')
				v++;
		} else
			while ((*v == '_' || (*v >= 'A' && *v <= 'Z') || (*v >= 'a' && *v <= 'z') ||
			    (*v >= '0' && *v <= '9')) && n < (int)sizeof name - 1)
				name[n++] = *v++;
		name[n] = 0;
		if (strcmp(name, "WABIHOME") == 0)
			e = wabihome();
		else if (strcmp(name, "PWD") == 0 && !getenv("PWD"))
			e = getcwd(cwd, sizeof cwd) ? cwd : "";
		else
			e = getenv(name);
		for (; e && *e && q < out + size - 1; e++)
			*q++ = *e;
	}
	*q = 0;
}

/* Drives.X's directory for drive X, 0 if it names none */
char *
wabi_drive(letter)
	int letter;
{
	static char path[26][512];
	char key[16], v[512];
	struct stat st;
	int i = (letter | 0x20) - 'a';

	if (i < 0 || i >= 26)
		return 0;
	sprintf(key, "Drives.%c", 'A' + i);
	if (!getkey(INI, key, v, sizeof v) || !v[0])
		return 0;
	wabi_expand(v, path[i], sizeof path[i]);
	if (stat(path[i], &st) != 0 || !S_ISDIR(st.st_mode))
		return 0;
	return path[i];
}

/* CFGGETENTRY(key, buf, size): 0 found */
static u32
c_GetEntry(a)
	u32 *a;
{
	char *k = gptr(a[0]), *b = gptr(a[1]);

	if (!k || !b || !(a[2] & 0xffff))
		return ENOTFOUND;
	return getkey(INI, k, b, (int)(a[2] & 0xffff)) ? 0 : ENOTFOUND;
}

/* CFGGETDEFAULTENTRY(key, buf, size): the value Wabi shipped */
static u32
c_GetDefaultEntry(a)
	u32 *a;
{
	char *k = gptr(a[0]), *b = gptr(a[1]);

	if (!k || !b || !(a[2] & 0xffff))
		return ENOTFOUND;
	return getkey(DEFINI, k, b, (int)(a[2] & 0xffff)) ? 0 : ENOTFOUND;
}

/* CFGSETENTRY(key, value, size, validate only): 0 accepted */
static u32
c_SetEntry(a)
	u32 *a;
{
	char *k = gptr(a[0]), *v = gptr(a[1]), sect[64];

	if (!k || !v)
		return ENOTFOUND;
	if (a[3] & 0xffff)
		return 0;
	/* where it is, in [CommonSettings] if only that has it */
	strcpy(sect, SECT);
	{
		char t[8];

		profile_get(INI, SECT, k, NONE, t, (u32)sizeof t);
		if (strcmp(t, NONE) == 0) {
			profile_get(INI, "CommonSettings", k, NONE, t, (u32)sizeof t);
			if (strcmp(t, NONE) != 0)
				strcpy(sect, "CommonSettings");
		}
	}
	return profile_put(INI, sect, k, v) ? 0 : ENOTFOUND;
}

/* CFGNOTIFYWABI(key, flags): a key changed; a drive's is mapped again */
static u32
c_NotifyWabi(a)
	u32 *a;
{
	char *k = gptr(a[0]), *p;
	int i;

	if (k && w16_strnicmp(k, "DriveFlags.", 11) == 0)
		remoteok = 0;
	if (!k || w16_strnicmp(k, "Drives.", 7) != 0 || !k[7] || k[8])
		return 0;
	i = (k[7] | 0x20) - 'a';
	/* not C:, where WABI.INI is, nor W:, Wabi's own */
	if (i < 0 || i >= 26 || i == 2 || i == 'w' - 'a')
		return 0;
	p = wabi_drive(k[7]);
	drive_root[i] = p;
	if (w16_debug)
		w16_log("startwin: drive %c: %s\n", 'A' + i, p ? p : "(none)");
	return 0;
}

/*
 * GETWABIENV(HGLOBAL far *): a block of two words, the second a block of
 * the environment's strings (NAME=value, each NUL-ended, an empty one
 * last); 0 done.
 */
static u32
c_GetWabiEnv(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0])), n = 1, h, he;
	char **e, *q;

	if (!p)
		return 1;
	for (e = environ; e && *e; e++)
		n += strlen(*e) + 1;
	if ((h = g_alloc(GMEM_MOVEABLE | GMEM_ZEROINIT, (u32)4, 0)) == 0)
		return 1;
	if ((he = g_alloc(GMEM_MOVEABLE | GMEM_ZEROINIT, n, 0)) == 0) {
		g_free(h);
		return 1;
	}
	q = (char *)M + sel_base(he);
	for (e = environ; e && *e; e++) {
		strcpy(q, *e);
		q += strlen(q) + 1;
	}
	*q = 0;
	PW(sel_base(h), 0);
	PW(sel_base(h) + 2, he);
	PW(p, h);
	return 0;
}

/* RELEASEWABIENV(HGLOBAL far *) */
static u32
c_ReleaseWabiEnv(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0])), h;

	if (!p || (h = GW(p)) == 0)
		return 0;
	if (GW(sel_base(h) + 2))
		g_free(GW(sel_base(h) + 2));
	g_free(h);
	PW(p, 0);
	return 0;
}

/* a name matching a pattern of * and ?; an empty pattern matches all */
static int
match(p, s)
	char *p, *s;
{
	if (!*p)
		return 1;
	for (; *p; p++, s++) {
		if (*p == '*') {
			for (p++; *s; s++)
				if (match(p, s))
					return 1;
			return !*p;
		}
		if (!*s || (*p != '?' && *p != *s))
			return 0;
	}
	return !*s;
}

static int
cmp(a, b)
	const void *a, *b;
{
	return strcmp(*(char **)a, *(char **)b);
}

/* a list of NUL-ended names, an empty one last, in order */
static void
sortlist(buf)
	char *buf;
{
	char **v, *p, *out;
	int n = 0, i;
	long len;

	for (p = buf; *p; p += strlen(p) + 1)
		n++;
	len = p - buf;
	if (n < 2 || (v = (char **)malloc(n * sizeof *v)) == 0)
		return;
	if ((out = (char *)malloc(len + 1)) == 0) {
		free(v);
		return;
	}
	for (i = 0, p = buf; *p; p += strlen(p) + 1)
		v[i++] = p;
	qsort((char *)v, n, sizeof *v, cmp);
	for (i = 0, p = out; i < n; i++) {
		strcpy(p, v[i]);
		p += strlen(p) + 1;
	}
	memcpy(buf, out, len);
	free(out);
	free(v);
}

/*
 * GETFILES(HGLOBAL far *list, dir, pattern, type): the names in a Unix
 * directory, as Configuration Manager browses for a drive's directory
 * (type 3: the directories) or a port's device (2: character devices);
 * NUL-ended, an empty one last.  0 done.
 */
static u32
c_GetFiles(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[0]), FPOFF(a[0])), h, n = 1, size = 1024;
	char *dir = gptr(a[1]), *pat = gptr(a[2]), path[1024], *buf, *nb;
	int type = a[3] & 0xffff, ok;
	DIR *d;
	struct dirent *e;
	struct stat st;

	if (!p || !dir || (d = opendir(*dir ? dir : "/")) == 0)
		return 1;
	buf = (char *)malloc(size);
	while ((e = readdir(d)) != 0) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || !match(pat ? pat : "", e->d_name))
			continue;
		if (strlen(dir) + strlen(e->d_name) + 2 > sizeof path)
			continue;
		sprintf(path, "%s/%s", dir, e->d_name);
		if (stat(path, &st) != 0)
			continue;
		ok = type == 3 ? S_ISDIR(st.st_mode) : type == 2 ? S_ISCHR(st.st_mode) : 1;
		if (!ok)
			continue;
		while (n + strlen(e->d_name) + 2 > size) {
			size *= 2;
			if ((nb = (char *)realloc(buf, size)) == 0)
				break;
			buf = nb;
		}
		if (n + strlen(e->d_name) + 2 > size)
			break;
		strcpy(buf + n - 1, e->d_name);
		n += strlen(e->d_name) + 1;
	}
	closedir(d);
	buf[n - 1] = 0;
	sortlist(buf);
	h = g_alloc(GMEM_MOVEABLE | GMEM_ZEROINIT, n + 1, 0);
	if (h)
		memcpy(M + sel_base(h), buf, n);
	free(buf);
	if (!h)
		return 1;
	PW(p, h);
	return 0;
}

/* the locale's directory of Wabi's home, with a backslash: W:\LIB\LOCALE\xx\ */
static u32
localepath(buf, size, deflt)
	u32 buf, size;
	int deflt;
{
	char path[600], dos[64], *lang = "en_us", *e = getenv("LANG");
	struct stat st;
	int n;

	if (!deflt && e && *e && strlen(e) < 20 && !strchr(e, '/')) {
		sprintf(path, "%s/lib/locale/%s/wabi", wabihome(), e);
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			lang = e;
	}
	sprintf(dos, "W:\\LIB\\LOCALE\\%s\\", lang);
	w16_upper(dos);
	n = strlen(dos);
	if (!buf || (u32)n + 1 > size)
		return 0;
	strcpy((char *)M + buf, dos);
	return n;
}

/* CFGGETLOCALEPATH(buf, size): its length */
static u32
c_GetLocalePath(a)
	u32 *a;
{
	return localepath(lin(FPSEL(a[0]), FPOFF(a[0])), a[1] & 0xffff, 0);
}

static u32
c_GetDefLocalePath(a)
	u32 *a;
{
	return localepath(lin(FPSEL(a[0]), FPOFF(a[0])), a[1] & 0xffff, 1);
}

/* WABI_GETENV(name): the value of a variable of the environment, 0 if unset */
static u32
c_GetEnv(a)
	u32 *a;
{
	static struct { char name[64]; u32 p; } got[16];
	static int ngot;
	char *n = gptr(a[0]), *v;
	int i;

	if (!n || (v = getenv(n)) == 0)
		return 0;
	for (i = 0; i < ngot; i++)
		if (strcmp(got[i].name, n) == 0)
			break;
	if (i == ngot) {
		if (ngot == 16 || strlen(n) >= sizeof got[0].name)
			return 0;
		/* kept: the program holds on to it */
		if ((got[i].p = g_alloc(0, (u32)strlen(v) + 1, 0)) == 0)
			return 0;
		strcpy(got[i].name, n);
		strcpy((char *)M + sel_base(got[i].p), v);
		got[i].p = FP(got[i].p, 0);
		ngot++;
	}
	return got[i].p;
}

/* a drive WABI.INI's DriveFlags.X calls Remote: a network drive, as Wabi's were */
int
wabi_remote(i)
	int i;
{
	char key[16], v[64], *p;
	int k;

	if (i < 0 || i >= 26 || !drive_root[i])
		return 0;
	if (!remoteok) {
		for (k = 0; k < 26; k++) {
			sprintf(key, "DriveFlags.%c", 'A' + k);
			remote[k] = 0;
			if (getkey(INI, key, v, sizeof v))
				for (p = v; *p; p++)
					if (w16_strnicmp(p, "remote", 6) == 0)
						remote[k] = 1;
		}
		remoteok = 1;
	}
	return remote[i];
}

/*
 * WNetGetConnection(local, remote, &size): a network drive's name, as
 * Wabi gave it, its Unix directory; W:'s, where Wabi's home was (WABI.INI
 * [Ash Nazag] WabiHome), which documents made under Wabi name.
 */
static u32
c_WNetGetConnection(a)
	u32 *a;
{
	char *l = gptr(a[0]), *r = gptr(a[1]), name[600];
	u32 np = lin(FPSEL(a[2]), FPOFF(a[2]));
	int i, n;

	if (!l || !np || !l[0] || l[1] != ':' || l[2])
		return 0x33;			/* WN_BAD_LOCALNAME */
	i = (l[0] | 0x20) - 'a';
	if (!wabi_remote(i))
		return 0x30;			/* WN_NOT_CONNECTED */
	strncpy(name, drive_root[i], sizeof name - 1);
	name[sizeof name - 1] = 0;
	if (i == 'w' - 'a') {
		profile_get(INI, "Ash Nazag", "WabiHome", "", name, (u32)sizeof name);
		if (!name[0])
			strncpy(name, drive_root[i], sizeof name - 1);
	}
	n = strlen(name) + 1;
	if (!r || n > (int)GW(np)) {
		PW(np, n);
		return 3;			/* WN_MORE_DATA */
	}
	strcpy(r, name);
	PW(np, n);
	return 0;
}

struct impl wc_impl[] = {
	{ "WABICFG", "CFGGETENTRY", c_GetEntry },
	{ "WABICFG", "CFGGETDEFAULTENTRY", c_GetDefaultEntry },
	{ "WABICFG", "CFGSETENTRY", c_SetEntry },
	{ "WABICFG", "CFGNOTIFYWABI", c_NotifyWabi },
	{ "WABICFG", "GETWABIENV", c_GetWabiEnv },
	{ "WABICFG", "RELEASEWABIENV", c_ReleaseWabiEnv },
	{ "WABICFG", "GETFILES", c_GetFiles },
	{ "WABICFG", "CFGGETLOCALEPATH", c_GetLocalePath },
	{ "WABICFG", "CFGGETDEFLOCALEPATH", c_GetDefLocalePath },
	{ "WABICFG", "WABI_GETENV", c_GetEnv },
	{ "USER", "WNetGetConnection", c_WNetGetConnection },
	{ 0 }
};
