/*
 * profile.c -- WIN.INI and private .INI files: GetProfileString and
 * friends over host text files.  A name without a directory is in the
 * Windows directory.  Sections and keys match without regard to case;
 * a write rewrites the file.  Files are kept in memory, checked against
 * the host's inode, size and time on each use.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "w16.h"

#define	MAXLINE	1024
#define	NINI	8

static struct ini {
	char dos[128], host[1024];
	ino_t ino;
	off_t size;
	time_t mtime;
	char *buf;
	long len;
} inis[NINI];
static int nextini, inierr;	/* inierr: a file there but unread */

static void
inidrop(f)
	struct ini *f;
{
	free(f->buf);
	f->buf = 0;
	f->dos[0] = 0;
}

/* a DOS file written, created, renamed or deleted (0: any) */
void
profile_touch(dos)
	char *dos;
{
	int i;

	for (i = 0; i < NINI; i++)
		if (inis[i].dos[0] && (!dos || strcmp(inis[i].dos, dos) == 0))
			inidrop(&inis[i]);
}

static void
inistat(f)
	struct ini *f;
{
	struct stat st;

	if (stat(f->host, &st) == 0) {
		f->ino = st.st_ino;
		f->size = st.st_size;
		f->mtime = st.st_mtime;
	} else
		inidrop(f);
}

/*
 * The cached file for file, its host path in host; 0 if it cannot be
 * read (host then empty when the name has no host path).
 */
static struct ini *
iniopen(file, host)
	char *file, *host;
{
	char dos[300], full[128];
	struct ini *f;
	struct stat st;
	int i, fd;

	if (!file || !*file)
		file = "WIN.INI";
	if (!strchr(file, '\\') && !strchr(file, ':'))
		sprintf(dos, "%s\\%.200s", windir, file);
	else
		strncpy(dos, file, sizeof dos - 1), dos[sizeof dos - 1] = 0;
	host[0] = 0;
	inierr = 0;
	if (dos_fullpath(dos, full) != 0)
		full[0] = 0;
	for (i = 0; full[0] && i < NINI; i++) {
		f = &inis[i];
		if (!f->dos[0] || strcmp(f->dos, full) != 0)
			continue;
		if (stat(f->host, &st) == 0 && st.st_ino == f->ino &&
		    st.st_size == f->size && st.st_mtime == f->mtime) {
			strcpy(host, f->host);
			return f;
		}
		inidrop(f);
	}
	if (dos_hostpath(dos, host, 1024, 1) != 0) {
		host[0] = 0;
		return 0;
	}
	if (!full[0] || (fd = open(host, O_RDONLY)) < 0)
		return 0;
	f = &inis[nextini];
	nextini = (nextini + 1) % NINI;
	if (f->dos[0])
		inidrop(f);
	if (fstat(fd, &st) < 0 || (f->buf = malloc(st.st_size + 1)) == 0) {
		inierr = 1;
		close(fd);
		return 0;
	}
	for (f->len = 0; f->len < st.st_size; f->len += i)
		if ((i = read(fd, f->buf + f->len, st.st_size - f->len)) <= 0)
			break;
	close(fd);
	strcpy(f->dos, full);
	strcpy(f->host, host);
	f->ino = st.st_ino;
	f->size = st.st_size;
	f->mtime = st.st_mtime;
	return f;
}

/* the next line of f from *pos, as fgets gives it */
static int
inigets(line, size, f, pos)
	char *line;
	int size;
	struct ini *f;
	long *pos;
{
	char *s = f->buf + *pos, *e = f->buf + f->len, *d = line;

	if (s >= e)
		return 0;
	while (s < e && d < line + size - 1)
		if ((*d++ = *s++) == '\n')
			break;
	*d = 0;
	*pos = s - f->buf;
	return 1;
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
issection(line, out)
	char *line, *out;
{
	char buf[MAXLINE], *l, *e;
	int n;

	for (l = line; *l == ' ' || *l == '\t'; l++)
		;
	if (*l != '[')
		return 0;
	/* a copy: the line is written back as it is */
	n = strlen(line);
	if (n > sizeof buf - 1)
		n = sizeof buf - 1;
	memcpy(buf, line, n);
	buf[n] = 0;
	l = trim(buf);
	if (*l != '[' || (e = strchr(l, ']')) == 0)
		return 0;
	memcpy(out, l + 1, e - l - 1);
	out[e - l - 1] = 0;
	l = trim(out);
	memmove(out, l, strlen(l) + 1);
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
	struct ini *f;
	long pos = 0;
	int in = 0;
	u32 n = 0, l;

	if (size == 0)
		return 0;
	out[0] = 0;
	if ((f = iniopen(file, host)) == 0)
		goto dflt;
	while (inigets(line, sizeof line, f, &pos)) {
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

struct obuf {
	char *buf;
	long len, cap;
	int err;
};

static void
oput(o, s)
	struct obuf *o;
	char *s;
{
	long n = strlen(s);
	char *b;

	if (o->len + n > o->cap) {
		if ((b = realloc(o->buf, o->cap * 2 + n + 1024)) == 0) {
			o->err = 1;
			return;
		}
		o->buf = b;
		o->cap = o->cap * 2 + n + 1024;
	}
	memcpy(o->buf + o->len, s, n);
	o->len += n;
}

static void
okey(o, key, value)
	struct obuf *o;
	char *key, *value;
{
	oput(o, key);
	oput(o, "=");
	oput(o, value);
	oput(o, "\r\n");
}

/* key's value set (value 0: the key goes; key 0: the section goes) */
int
profile_put(file, sect, key, value)
	char *file, *sect, *key, *value;
{
	char host[1024], tmp[1100], line[MAXLINE], name[MAXLINE], *eq, *v;
	struct ini *f;
	struct obuf o;
	FILE *out;
	long pos = 0;
	int in = 0, done = 0, blanks = 0, i, ok;

	if (!sect)
		return 0;
	f = iniopen(file, host);
	if (!host[0])
		return 0;
	sprintf(tmp, "%s.new", host);
	if ((out = fopen(tmp, "w")) == 0)
		return 0;
	o.buf = 0;
	o.len = o.cap = 0;
	o.err = inierr;
	if (f) {
		while (inigets(line, sizeof line, f, &pos)) {
			/* blank lines held back: a new key goes after the section's last line, before them */
			strcpy(name, line);
			if (!*trim(name)) {
				blanks++;
				continue;
			}
			if (issection(line, name)) {
				if (in && !done && key && value) {
					okey(&o, key, value);
					done = 1;
				}
				for (i = 0; i < blanks; i++)
					oput(&o, "\r\n");
				blanks = 0;
				in = w16_stricmp(name, sect) == 0;
				if (in && !key)
					continue;
				oput(&o, line);
				continue;
			}
			if (in && !key) {
				blanks = 0;
				continue;
			}
			for (i = 0; i < blanks; i++)
				oput(&o, "\r\n");
			blanks = 0;
			if (in && !done) {
				strcpy(name, line);
				v = trim(name);
				if (*v != ';' && (eq = strchr(v, '=')) != 0) {
					*eq = 0;
					if (w16_stricmp(trim(v), key) == 0) {
						if (value)
							okey(&o, key, value);
						done = 1;
						continue;
					}
				}
			}
			oput(&o, line);
		}
	}
	if (!done && key && value) {
		if (!in) {
			oput(&o, o.len ? "\r\n[" : "[");
			oput(&o, sect);
			oput(&o, "]\r\n");
		}
		okey(&o, key, value);
	}
	for (i = 0; i < blanks; i++)
		oput(&o, "\r\n");
	if (o.len)
		fwrite(o.buf, 1, o.len, out);
	fclose(out);
	ok = !o.err && rename(tmp, host) == 0;
	if (o.err)
		unlink(tmp);
	if (ok && f) {
		free(f->buf);
		f->buf = o.buf;
		f->len = o.len;
		inistat(f);
	} else
		free(o.buf);
	return ok;
}
