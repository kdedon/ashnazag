/*
 * profile.c -- WIN.INI and private .INI files: GetProfileString and
 * friends over host text files.  A name without a directory is in the
 * Windows directory.  Sections and keys match without regard to case;
 * a write rewrites the file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"

#define	MAXLINE	1024

static int
inipath(file, host)
	char *file, *host;
{
	char dos[300];

	if (!file || !*file)
		file = "WIN.INI";
	if (!strchr(file, '\\') && !strchr(file, ':'))
		sprintf(dos, "%s\\%.200s", windir, file);
	else
		strncpy(dos, file, sizeof dos - 1), dos[sizeof dos - 1] = 0;
	return dos_hostpath(dos, host, 1024, 1);
}

static char *
trim(s)
	char *s;
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
		*--e = 0;
	return s;
}

/* [section] line? its name in out */
static int
issection(l, out)
	char *l, *out;
{
	char *e;

	l = trim(l);
	if (*l != '[' || (e = strchr(l, ']')) == 0)
		return 0;
	memcpy(out, l + 1, e - l - 1);
	out[e - l - 1] = 0;
	strcpy(out, trim(out));
	return 1;
}

/*
 * The value of key in section, or def; key 0: every key of the section,
 * section 0: every section, each NUL-terminated and one more NUL after.
 * Returns the characters copied, without the last NUL.
 */
int
profile_get(file, sect, key, def, out, size)
	char *file, *sect, *key, *def, *out;
	u32 size;
{
	char host[1024], line[MAXLINE], name[MAXLINE], *eq, *v;
	FILE *fp;
	int in = 0;
	u32 n = 0, l;

	if (size == 0)
		return 0;
	out[0] = 0;
	if (inipath(file, host) != 0 || (fp = fopen(host, "r")) == 0)
		goto dflt;
	while (fgets(line, sizeof line, fp)) {
		if (issection(line, name)) {
			if (!sect) {
				l = strlen(name);
				if (n + l + 2 > size)
					break;
				strcpy(out + n, name);
				n += l + 1;
				continue;
			}
			in = w16_stricmp(name, sect) == 0;
			continue;
		}
		if (!in || !sect)
			continue;
		v = trim(line);
		if (*v == ';' || (eq = strchr(v, '=')) == 0)
			continue;
		*eq = 0;
		strcpy(name, trim(v));
		v = trim(eq + 1);
		if (!key) {
			l = strlen(name);
			if (n + l + 2 > size)
				break;
			strcpy(out + n, name);
			n += l + 1;
			continue;
		}
		if (w16_stricmp(name, key) == 0) {
			fclose(fp);
			l = strlen(v);
			if (l > 1 && (v[0] == '"' || v[0] == '\'') && v[l - 1] == v[0]) {
				v[l - 1] = 0;
				v++;
				l -= 2;
			}
			if (l > size - 1)
				l = size - 1;
			memcpy(out, v, l);
			out[l] = 0;
			return l;
		}
	}
	fclose(fp);
	if (!key || !sect) {
		out[n] = 0;
		if (n + 1 < size)
			out[n + 1] = 0;
		return n ? n - 1 : 0;
	}
dflt:
	if (!key || !sect) {
		out[0] = 0;
		if (size > 1)
			out[1] = 0;
		return 0;
	}
	strncpy(out, def ? def : "", size - 1);
	out[size - 1] = 0;
	return strlen(out);
}

/* key's value set (value 0: the key goes; key 0: the section goes) */
int
profile_put(file, sect, key, value)
	char *file, *sect, *key, *value;
{
	char host[1024], tmp[1100], line[MAXLINE], name[MAXLINE], *eq, *v;
	FILE *fp, *out;
	int in = 0, done = 0, seen = 0;

	if (!sect || inipath(file, host) != 0)
		return 0;
	sprintf(tmp, "%s.new", host);
	if ((out = fopen(tmp, "w")) == 0)
		return 0;
	if ((fp = fopen(host, "r")) != 0) {
		while (fgets(line, sizeof line, fp)) {
			if (issection(line, name)) {
				if (in && !done && key && value) {
					fprintf(out, "%s=%s\n", key, value);
					done = 1;
				}
				in = w16_stricmp(name, sect) == 0;
				seen |= in;
				if (in && !key)
					continue;
				fputs(line, out);
				continue;
			}
			if (in && !key)
				continue;
			if (in && !done) {
				strcpy(name, line);
				v = trim(name);
				if (*v != ';' && (eq = strchr(v, '=')) != 0) {
					*eq = 0;
					if (w16_stricmp(trim(v), key) == 0) {
						if (value)
							fprintf(out, "%s=%s\n", key, value);
						done = 1;
						continue;
					}
				}
			}
			fputs(line, out);
		}
		fclose(fp);
	}
	if (!done && key && value) {
		if (!in)
			fprintf(out, "\n[%s]\n", sect);
		fprintf(out, "%s=%s\n", key, value);
	}
	(void)seen;
	fclose(out);
	return rename(tmp, host) == 0;
}
