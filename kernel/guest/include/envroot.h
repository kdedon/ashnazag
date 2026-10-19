/*
 * Guest environments: the root of environment <id> comes from the
 * family's layout policy, /etc/default/<family> (literal KEY=value
 * lines).  A writable session holds a write lock on the root's .env;
 * the kernel drops it when the process exits.  Without -e a launcher
 * runs the user's default (~/.default/<family>), else the system's.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <time.h>

/* key's value in file path */
static int
envkey(path, key, out, size)
	char *path, *key, *out;
	unsigned int size;
{
	char line[1100];
	FILE *fp;
	unsigned int n = strlen(key);
	int r = -1;

	if ((fp = fopen(path, "r")) == 0)
		return -1;
	while (fgets(line, sizeof line, fp))
		if (strncmp(line, key, n) == 0 && line[n] == '=') {
			line[strcspn(line, "\n")] = 0;
			if (line[n + 1] && strlen(line + n + 1) < size) {
				strcpy(out, line + n + 1);
				r = 0;
			}
			break;
		}
	fclose(fp);
	return r;
}

/* key's value from the policy, else def; a leading ~/ is home */
static int
envpolicy(family, key, def, home, out, size)
	char *family, *key, *def, *home, *out;
	unsigned int size;
{
	char path[64], val[1100], *v = def;

	sprintf(path, "/etc/default/%.32s", family);
	if (envkey(path, key, val, sizeof val) == 0)
		v = val;
	if (v[0] == '~' && v[1] == '/') {
		if (home == 0 || home[0] != '/' || strlen(home) + strlen(v) > size)
			return -1;
		sprintf(out, "%s%s", home, v + 1);
	} else if (strlen(v) < size)
		strcpy(out, v);
	else
		return -1;
	return 0;
}

/* an id names one directory: a letter or digit, then letters, digits, - _ . */
static int
envid(id)
	char *id;
{
	static char shared[] = "shared";
	char *p;
	int i;

	for (i = 0; shared[i] && (id[i] | 040) == shared[i]; i++)
		;
	if (strlen(id) > 64 || (i == 6 && id[6] == 0))
		return 0;
	for (p = id; *p; p++)
		if (!isalnum((unsigned char)*p) && (p == id || strchr("-_.", *p) == 0))
			return 0;
	return p > id;
}

/* the family's folder in home */
static char *
envhomedir(family)
	char *family;
{
	return strcmp(family, "mac") == 0 ? "~/Mac" :
	    strcmp(family, "tos") == 0 ? "~/TOS" :
	    strcmp(family, "cpm") == 0 ? "~/CPM" :
	    strcmp(family, "win16") == 0 ? "~/WIN16" :
	    strcmp(family, "smsq") == 0 ? "~/SMSQ" :
	    strcmp(family, "lisa") == 0 ? "~/Lisa" :
	    strcmp(family, "voyage200") == 0 ? "~/Voyage200" :
	    strcmp(family, "next") == 0 ? "~/NeXT" : "~/Amiga";
}

/* the root of environment id; no id is the legacy tree */
static int
envroot(family, id, home, out, size)
	char *family, *id, *home, *out;
	unsigned int size;
{
	if (id == 0)
		return envpolicy(family, "LEGACY_ROOT",
		    strcmp(family, "mac") == 0 ? "~/System Folder" :
		    strcmp(family, "next") == 0 ? "~/NeXT/openstep" : envhomedir(family), home, out, size);
	if (!envid(id) || envpolicy(family, "ENVIRONMENT_ROOT", envhomedir(family), home, out, size) < 0 ||
	    strlen(out) + strlen(id) + 2 > size)
		return -1;
	strcat(out, "/");
	strcat(out, id);
	return 0;
}

/*
 * The session's write lock on file, which names root in messages: the
 * descriptor, -2 when the file can't be opened for writing (no
 * writable session to guard), -1 when another session holds it.
 */
static int
envlockfile(prog, root, path, flags)
	char *prog, *root, *path;
	int flags;
{
	struct flock fl;
	int fd;

	if ((fd = open(path, O_RDWR | flags, 0644)) < 0)
		return -2;
	memset((char *)&fl, 0, sizeof fl);
	fl.l_type = F_WRLCK;
	fl.l_whence = 0;
	if (fcntl(fd, F_SETLK, &fl) == 0)
		return fd;
	if (errno != EAGAIN && errno != EACCES) {
		fprintf(stderr, "%s: %s: no session lock: %s\n", prog, path, strerror(errno));
		close(fd);
		return -2;
	}
	fl.l_type = F_WRLCK;
	if (fcntl(fd, F_GETLK, &fl) < 0 || fl.l_type == F_UNLCK)
		fl.l_pid = 0;
	fprintf(stderr, "%s: %s is in use by process %ld; one session at a time\n",
	    prog, root, (long)fl.l_pid);
	close(fd);
	return -1;
}

/* root's metadata file */
static int
envmeta(family, root, out, size)
	char *family, *root, *out;
	unsigned int size;
{
	char meta[64];

	if (envpolicy(family, "ENVIRONMENT_METADATA", ".env", "/", meta, sizeof meta) < 0 ||
	    strlen(root) + strlen(meta) + 2 > size)
		return -1;
	sprintf(out, "%s/%s", root, meta);
	return 0;
}

/* the lock on root's metadata file, made if missing */
static int
envlock(prog, family, root)
	char *prog, *family, *root;
{
	char path[1100];

	if (envmeta(family, root, path, sizeof path) < 0)
		return -2;
	return envlockfile(prog, root, path, O_CREAT);
}

static int
envin(id, list)
	char *id, **list;
{
	for (; *list; list++)
		if (strcmp(id, *list) == 0)
			return 1;
	return 0;
}

/* the root of environment id; base's is the legacy tree */
static int
envhome(family, id, base, home, out, size)
	char *family, *id, *base, *home, *out;
	unsigned int size;
{
	return envroot(family, strcmp(id, base) == 0 ? (char *)0 : id, home, out, size);
}

/* the environment to run: id, else the user's default, else the system's, else base */
static char *
envchoose(family, id, home, base)
	char *family, *id, *home, *base;
{
	static char def[68];
	char path[1100];

	if (id)
		return id;
	if (home && home[0] == '/' && strlen(home) < 1000) {
		sprintf(path, "%s/.default/%.32s", home, family);
		if (envkey(path, "DEFAULT_ENVIRONMENT", def, sizeof def) == 0 && envid(def))
			return def;
	}
	sprintf(path, "/etc/default/%.32s", family);
	if (envkey(path, "DEFAULT_ENVIRONMENT", def, sizeof def) == 0 && envid(def))
		return def;
	return base;
}

/* -d: id becomes the user's default */
static int
envsetdefault(prog, family, home, id)
	char *prog, *family, *home, *id;
{
	char path[1100];
	FILE *fp;

	if (!envid(id)) {
		fprintf(stderr, "%s: %s: not an environment name\n", prog, id);
		return -1;
	}
	if (home == 0 || home[0] != '/' || strlen(home) > 1000) {
		fprintf(stderr, "%s: no home directory for the default\n", prog);
		return -1;
	}
	sprintf(path, "%s/.default", home);
	mkdir(path, 0755);
	sprintf(path, "%s/.default/%.32s", home, family);
	if ((fp = fopen(path, "w")) == 0) {
		fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
		return -1;
	}
	fprintf(fp, "DEFAULT_ENVIRONMENT=%s\n", id);
	if (fclose(fp) != 0) {
		fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
		return -1;
	}
	return 0;
}

/* -l: the installed environments, then the user's own, the default marked */
static void
envlist(family, home, base, installed)
	char *family, *home, *base, **installed;
{
	char root[1100], dir[1200], meta[1300], *def = envchoose(family, (char *)0, home, base), **p;
	struct dirent *d;
	struct stat sb;
	DIR *dp;

	for (p = installed; *p; p++)
		printf("%s%s\n", *p, strcmp(*p, def) ? "" : " (default)");
	if (envpolicy(family, "ENVIRONMENT_ROOT", envhomedir(family), home, root, sizeof root) < 0 ||
	    (dp = opendir(root)) == 0)
		return;
	while ((d = readdir(dp)) != 0) {
		if (envin(d->d_name, installed) || !envid(d->d_name) || strlen(root) + strlen(d->d_name) > 1000)
			continue;
		sprintf(dir, "%s/%s", root, d->d_name);
		if (envmeta(family, dir, meta, sizeof meta) == 0 && stat(meta, &sb) == 0)
			printf("%s%s\n", d->d_name, strcmp(d->d_name, def) ? "" : " (default)");
	}
	closedir(dp);
}

/*
 * Copies from into root through find | cpio, replacing root's copies of
 * from's files and keeping its others; filter (a pipeline stage, or 0)
 * drops names.  With id, the metadata, written last when missing, names
 * the environment and its base.
 */
static int
envcopy(prog, family, id, base, from, root, filter)
	char *prog, *family, *id, *base, *from, *root, *filter;
{
	char cmd[2600], meta[1100], ts[32];
	struct stat sb;
	time_t now = time((time_t *)0);
	FILE *fp;

	if (strchr(from, '\'') || strchr(root, '\'') || strlen(from) + 3 * strlen(root) +
	    (filter ? strlen(filter) : 0) > 2400)
		return -1;
	sprintf(cmd, "{ [ -d '%s' ] || mkdir -p '%s'; } && cd '%s' && find . -print | %s cpio -pdmu '%s' 2> /dev/null",
	    root, root, from, filter ? filter : "", root);
	if (system(cmd) != 0) {
		fprintf(stderr, "%s: %s: cannot copy %s\n", prog, root, from);
		return -1;
	}
	if (id == 0 || envmeta(family, root, meta, sizeof meta) < 0 || stat(meta, &sb) == 0)
		return 0;
	strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
	if ((fp = fopen(meta, "w")) == 0 ||
	    fprintf(fp, "id=%s\nname=%s\nbase=%s\ntemplate=%s\ncreated=%s\n", id, id, base, from, ts) < 0 ||
	    fclose(fp) != 0) {
		fprintf(stderr, "%s: %s: %s\n", prog, meta, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * The installed environment id copies: its own if installed, a user's
 * its metadata's base, a new one base, else the default's.  0, with a
 * message, if there is none.
 */
static char *
envbase(prog, family, id, base, home, legacy, installed)
	char *prog, *family, *id, *base, *home, *legacy, **installed;
{
	static char b[68];
	char root[1100], meta[1200];
	struct stat sb;

	if (envroot(family, envin(id, installed) ? (char *)0 : id, home, root, sizeof root) < 0 ||
	    envmeta(family, root, meta, sizeof meta) < 0) {
		fprintf(stderr, "%s: %s: not an environment name\n", prog, id);
		return 0;
	}
	if (envin(id, installed) || stat(meta, &sb) == 0) {
		if (base) {
			fprintf(stderr, "%s: %s exists; -b makes a new environment\n", prog, id);
			return 0;
		}
		if (envin(id, installed))
			return id;
		return envkey(meta, "base", b, sizeof b) == 0 && envin(b, installed) ? b : legacy;
	}
	if (base == 0)
		base = envchoose(family, (char *)0, home, legacy);
	if (envin(base, installed))
		return base;
	if (envid(base) && strcmp(base, id) && envroot(family, base, home, root, sizeof root) == 0 &&
	    envmeta(family, root, meta, sizeof meta) == 0 &&
	    envkey(meta, "base", b, sizeof b) == 0 && envin(b, installed))
		return b;
	fprintf(stderr, "%s: no environment %s to copy\n", prog, base);
	return 0;
}

/*
 * -b where environments have no template: a copy of the user's
 * environment base into root, less its metadata and the environments
 * nested in it, root among them.
 */
/* name as an egrep pattern after |, dots escaped */
static void
envpat(out, name)
	char *out, *name;
{
	out += strlen(out);
	if (out[-1] != '(')
		*out++ = '|';
	for (; *name; name++) {
		if (*name == '.')
			*out++ = '\\';
		*out++ = *name;
	}
	*out = 0;
}

static int
envclone(prog, family, id, base, home, legacy, root)
	char *prog, *family, *id, *base, *home, *legacy, *root;
{
	char from[1100], meta[1200], path[1300], filter[2000];
	struct dirent *d;
	struct stat sb;
	DIR *dp;

	if (!envid(base) || envhome(family, base, legacy, home, from, sizeof from) < 0 ||
	    stat(from, &sb) < 0 || (sb.st_mode & S_IFMT) != S_IFDIR ||
	    envmeta(family, "", meta, sizeof meta) < 0 || strlen(meta) > 60 ||
	    strchr(meta, '\'') || (dp = opendir(from)) == 0) {
		fprintf(stderr, "%s: no environment %s to copy\n", prog, base);
		return -1;
	}
	strcpy(filter, "egrep -v '^\\./(");
	envpat(filter, meta + 1);
	envpat(filter, id);
	while ((d = readdir(dp)) != 0) {
		if (!envid(d->d_name) || strlen(from) + strlen(d->d_name) + strlen(meta) > 1200 ||
		    strlen(filter) + 2 * strlen(d->d_name) > sizeof filter - 20)
			continue;
		sprintf(path, "%s/%s%s", from, d->d_name, meta);
		if (stat(path, &sb) == 0)
			envpat(filter, d->d_name);
	}
	closedir(dp);
	strcat(filter, ")(/|$)' |");
	return envcopy(prog, family, id, base, from, root, filter);
}
