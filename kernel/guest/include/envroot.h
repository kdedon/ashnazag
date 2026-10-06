/*
 * Guest environments: the root of environment <id> comes from the
 * family's layout policy, /etc/default/<family> (literal KEY=value
 * lines).  A writable session holds a write lock on the root's .env;
 * the kernel drops it when the process exits.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>

/* key's value from the policy, else def; a leading ~/ is home */
static int
envpolicy(family, key, def, home, out, size)
	char *family, *key, *def, *home, *out;
	unsigned int size;
{
	char path[64], line[1100], *v = def;
	FILE *fp;
	unsigned int n = strlen(key);

	sprintf(path, "/etc/default/%.32s", family);
	if ((fp = fopen(path, "r")) != 0) {
		while (fgets(line, sizeof line, fp))
			if (strncmp(line, key, n) == 0 && line[n] == '=') {
				line[strcspn(line, "\n")] = 0;
				if (line[n + 1])
					v = line + n + 1;
				break;
			}
		fclose(fp);
	}
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

/* the root of environment id; no id or "default" is the legacy tree */
static int
envroot(family, id, home, out, size)
	char *family, *id, *home, *out;
	unsigned int size;
{
	char *er = strcmp(family, "mac") == 0 ? "~/Mac" :
	    strcmp(family, "tos") == 0 ? "~/TOS" :
	    strcmp(family, "cpm") == 0 ? "~/CPM" : "~/Amiga";

	if (id == 0 || strcmp(id, "default") == 0)
		return envpolicy(family, "LEGACY_ROOT",
		    strcmp(family, "mac") == 0 ? "~/System Folder" : er, home, out, size);
	if (!envid(id) || envpolicy(family, "ENVIRONMENT_ROOT", er, home, out, size) < 0 ||
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

/* the lock on root's metadata file, made if missing */
static int
envlock(prog, family, root)
	char *prog, *family, *root;
{
	char meta[64], path[1100];

	if (envpolicy(family, "ENVIRONMENT_METADATA", ".env", "/", meta, sizeof meta) < 0 ||
	    strlen(root) + strlen(meta) + 2 > sizeof path)
		return -2;
	sprintf(path, "%s/%s", root, meta);
	return envlockfile(prog, root, path, O_CREAT);
}
