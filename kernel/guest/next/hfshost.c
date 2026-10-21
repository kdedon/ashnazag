/*
 * The host side of the guest's filesystem: each request in the mailbox is
 * one operation on the environment folder.  A node is a small number the
 * guest keeps in its vnode; the host maps it to a path below the folder.
 * Owners, modes and device nodes come from the folder's .meta sidecar
 * ("mode uid gid mtime ./path" per line, written by makenext, later lines
 * win); changes the guest makes are appended to it.  A device node is a
 * NAME.__special__ file holding "mode=M raw=HEX" (the first word of raw is
 * the device number).
 */
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "hfswire.h"

#define NHASH 8192
#define SPECIAL ".__special__"
#define NX_IFSOCK 0140000
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif

struct ment {
	char *path;
	unsigned long mode, uid, gid, mtime;
	struct ment *next;
};

struct idn {
	char *path;
	unsigned long id;
	struct idn *next;
};

static char *root, *metapath;
static struct ment *mt[NHASH];
static struct idn *byid[NHASH], **ids;
static unsigned long nids, capids;
static FILE *metaout;

static unsigned
hash(s)
	char *s;
{
	unsigned h = 5381;

	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h % NHASH;
}

static char *
copy(s)
	char *s;
{
	char *p = malloc(strlen(s) + 1);

	if (p)
		strcpy(p, s);
	return p;
}

static void
metaset(path, mode, uid, gid, mtime)
	char *path;
	unsigned long mode, uid, gid, mtime;
{
	struct ment *m;
	unsigned h = hash(path);

	for (m = mt[h]; m; m = m->next)
		if (strcmp(m->path, path) == 0)
			break;
	if (!m) {
		if ((m = malloc(sizeof *m)) == 0 || (m->path = copy(path)) == 0)
			return;
		m->next = mt[h];
		mt[h] = m;
	}
	m->mode = mode;
	m->uid = uid;
	m->gid = gid;
	m->mtime = mtime;
}

static struct ment *
metaget(path)
	char *path;
{
	struct ment *m;

	for (m = mt[hash(path)]; m; m = m->next)
		if (strcmp(m->path, path) == 0)
			return m;
	return 0;
}

/* appends the current entry of path to the sidecar */
static void
metasave(path)
	char *path;
{
	struct ment *m = metaget(path);

	if (m && metaout) {
		fprintf(metaout, "%lo %lu %lu %lu ./%s\n", m->mode, m->uid, m->gid, m->mtime, path);
		fflush(metaout);
	}
}

/* rel is the sidecar's path without the leading "./" */
static void
metaload()
{
	char line[1400], *p;
	unsigned long mode, uid, gid, mtime;
	FILE *f = fopen(metapath, "r");

	if (f) {
		while (fgets(line, sizeof line, f)) {
			if ((p = strchr(line, '\n')) != 0)
				*p = 0;
			if (sscanf(line, "%lo %lu %lu %lu", &mode, &uid, &gid, &mtime) != 4)
				continue;
			if ((p = strstr(line, " ./")) != 0)
				metaset(p + 3, mode, uid, gid, mtime);
		}
		fclose(f);
	}
	metaout = fopen(metapath, "a");
}

int
hf_open(folder)
	char *folder;
{
	char *s;

	root = copy(folder);
	metapath = malloc(strlen(folder) + 8);
	if (!root || !metapath)
		return -1;
	sprintf(metapath, "%s.meta", folder);
	metaload();
	capids = 1024;
	ids = calloc(capids, sizeof *ids);
	nids = 2;			/* node 1 is the root, "" */
	if (!ids)
		return -1;
	s = copy("");
	ids[1] = malloc(sizeof **ids);
	if (!s || !ids[1])
		return -1;
	ids[1]->path = s;
	ids[1]->id = 1;
	ids[1]->next = byid[hash("")];
	byid[hash("")] = ids[1];
	return 0;
}

static unsigned long
idof(path)
	char *path;
{
	struct idn *n;
	unsigned h = hash(path);

	for (n = byid[h]; n; n = n->next)
		if (strcmp(n->path, path) == 0)
			return n->id;
	if (nids == capids) {
		struct idn **g = realloc(ids, capids * 2 * sizeof *ids);

		if (!g)
			return 0;
		memset(g + capids, 0, capids * sizeof *g);
		ids = g;
		capids *= 2;
	}
	if ((n = malloc(sizeof *n)) == 0 || (n->path = copy(path)) == 0)
		return 0;
	n->id = nids++;
	n->next = byid[h];
	byid[h] = n;
	ids[n->id] = n;
	return n->id;
}

static char *
pathof(id)
	unsigned long id;
{
	return id > 0 && id < nids && ids[id] ? ids[id]->path : 0;
}

/*
 * The host path of rel; with special, the NAME.__special__ file when NAME
 * is absent.  0 (ELOOP) if a directory on the way is a host link: the
 * guest follows its own links, so one here (a directory the guest removed
 * and made a link while still using it) would lead outside the folder.
 */
static char *
hostpath(rel, special)
	char *rel;
	int special;
{
	static char p[2200];
	struct stat st;
	char *c;

	sprintf(p, "%s/%.1000s", root, rel);
	if (rel[0] == 0)
		sprintf(p, "%s/.", root);
	for (c = p + strlen(root) + 1; (c = strchr(c, '/')) != 0; *c++ = '/') {
		*c = 0;
		if (lstat(p, &st) == 0 && S_ISLNK(st.st_mode)) {
			errno = ELOOP;
			return 0;
		}
	}
	if (special && lstat(p, &st) < 0 && errno == ENOENT)
		strcat(p, SPECIAL);
	return p;
}

/* hostpath for an operation that would follow a link in the last place too */
static char *
nolink(rel, special)
	char *rel;
	int special;
{
	struct stat st;
	char *p = hostpath(rel, special);

	if (p && lstat(p, &st) == 0 && S_ISLNK(st.st_mode)) {
		errno = ELOOP;
		return 0;
	}
	return p;
}

/* n blocks of size bytes as 8 KB blocks, at most what the guest's signed longs hold */
static unsigned long
blocks8k(n, size)
	unsigned long n, size;
{
	if (size >= 8192)
		return n > 0x7fffffffUL / (size / 8192) ? 0x7fffffffUL : n * (size / 8192);
	n /= size ? 8192 / size : 8192;
	return n > 0x7fffffffUL ? 0x7fffffffUL : n;
}

static int
errmap(e)
	int e;
{
	switch (e) {
	case 0: return 0;
	case EPERM: return 1;
	case ENOENT: return 2;
	case EACCES: return 13;
	case EEXIST: return 17;
	case ENOTDIR: return 20;
	case EISDIR: return 21;
	case EINVAL: return 22;
	case ENOSPC: return 28;
	case EROFS: return 30;
	case ENAMETOOLONG: return 63;
	case ELOOP: return 62;
	case ENOTEMPTY: return 66;
	}
	return 5;
}

static unsigned long
vtype(mode)
	unsigned long mode;
{
	switch (mode & S_IFMT) {
	case S_IFREG: return HF_VREG;
	case S_IFDIR: return HF_VDIR;
	case S_IFBLK: return HF_VBLK;
	case S_IFCHR: return HF_VCHR;
	case S_IFLNK: return HF_VLNK;
	case S_IFIFO: return HF_VFIFO;
	case NX_IFSOCK: return HF_VSOCK;
	}
	return HF_VREG;
}

/* the attributes of node id into the reply; an error number, 0 if fine */
static int
attrs(b, id)
	struct hf_box *b;
	unsigned long id;
{
	struct stat st;
	struct ment *m;
	char *rel = pathof(id), *hp;
	unsigned long mode;
	unsigned long raw = 0;
	FILE *f;
	char line[100];
	int special = 0;

	if (!rel)
		return 2;
	if ((hp = hostpath(rel, 1)) == 0 || lstat(hp, &st) < 0)
		return errmap(errno);
	mode = st.st_mode;
	if (S_ISREG(mode) && strlen(hp) > strlen(SPECIAL) && strcmp(hp + strlen(hp) - strlen(SPECIAL), SPECIAL) == 0) {
		special = 1;
		mode = S_IFCHR;
		if ((f = fopen(hp, "r")) != 0) {
			if (fgets(line, sizeof line, f)) {
				sscanf(line, "mode=%lo raw=%8lx", &mode, &raw);
			}
			fclose(f);
		}
	}
	b->mtime = st.st_mtime;
	b->auid = 0;
	b->agid = 0;
	if ((m = metaget(rel)) != 0) {
		mode = special ? mode : (m->mode & ~(unsigned long)S_IFMT) | (mode & S_IFMT);
		if (special)
			mode = m->mode;
		b->auid = m->uid;
		b->agid = m->gid;
		b->mtime = m->mtime;
	} else if (!special) {
		mode = (mode & S_IFMT) | (mode & 07777);
	}
	b->type = vtype(mode);
	b->amode = mode;
	b->nlink = special ? 1 : st.st_nlink;
	b->size = special ? 0 : st.st_size;
	b->blocks = special ? 0 : st.st_blocks;
	b->atime = st.st_atime;
	b->ctime = st.st_ctime;
	b->rdev = raw;
	b->ino = id;
	b->bsize = 8192;
	return 0;
}

/* child name of dir node d: a path in buf; 0 if the name is not usable */
static char *
child(d, name, buf)
	unsigned long d;
	char *name, *buf;
{
	char *rel = pathof(d), *p;

	if (!rel || !name[0] || strchr(name, '/'))
		return 0;
	if (strcmp(name, ".") == 0) {
		strcpy(buf, rel);
		return buf;
	}
	if (strcmp(name, "..") == 0) {
		strcpy(buf, rel);
		if ((p = strrchr(buf, '/')) != 0)
			*p = 0;
		else
			buf[0] = 0;
		return buf;
	}
	if (strlen(rel) + strlen(name) + 2 > 1000)
		return 0;
	if (rel[0])
		sprintf(buf, "%s/%s", rel, name);
	else
		strcpy(buf, name);
	return buf;
}

static void
setmeta(rel, mode, uid, gid, mtime)
	char *rel;
	unsigned long mode, uid, gid, mtime;
{
	metaset(rel, mode, uid, gid, mtime);
	metasave(rel);
}

/* the nodes at or below old now lie at new */
static void
moveids(old, new)
	char *old, *new;
{
	unsigned long i, ol = strlen(old);
	struct idn **pp, *n;
	char *p;

	for (i = 1; i < nids; i++) {
		n = ids[i];
		if (!n || strncmp(n->path, old, ol) != 0 || (n->path[ol] != 0 && n->path[ol] != '/'))
			continue;
		for (pp = &byid[hash(n->path)]; *pp && *pp != n; pp = &(*pp)->next)
			;
		if (*pp)
			*pp = n->next;
		if ((p = malloc(strlen(new) + strlen(n->path + ol) + 1)) == 0)
			continue;
		sprintf(p, "%s%s", new, n->path + ol);
		n->path = p;
		n->next = byid[hash(p)];
		byid[hash(p)] = n;
	}
}

/*
 * The window server's pointer setup copies an 8-bit pointer's image and mask
 * into the event globals under a second test for 2 bits, never true there:
 *
 *	moveq #3,d7; cmpl a0@,d7; bnes; pea 0x200 ...	16 bits
 *	moveq #1,d1; cmpl a0@,d1; bnes; pea 0x100 ...	8 bits (should be #2)
 *
 * Its code as read gets the 8-bit test; the file stays as it is.
 */
static void
wsfix(fd, off, buf, n)
	int fd;
	long off;
	unsigned char *buf;
	int n;
{
	static unsigned char p16[] = { 0x7e, 0x03, 0xbe, 0x90, 0x66, 0x16, 0x48, 0x78, 0x02, 0x00 };
	static unsigned char p8[] = { 0x72, 0x01, 0xb2, 0x90, 0x66, 0x22, 0x48, 0x78, 0x01, 0x00 };
	unsigned char t[48 + HF_DATA + 10];
	int h = off < 48 ? (int)off : 48, m, k;

	/* the read with the bytes either side, for a match across its edges */
	memcpy(t + h, buf, n);
	m = h + n;
	if (h && (lseek(fd, (off_t)(off - h), SEEK_SET) < 0 || read(fd, t, h) != h))
		return;
	if (lseek(fd, (off_t)(off + n), SEEK_SET) >= 0 && (k = read(fd, t + m, 10)) > 0)
		m += k;
	for (k = 36; k + 10 <= m; k++)
		if (t[k] == 0x72 && !memcmp(t + k, p8, sizeof p8) && !memcmp(t + k - 36, p16, sizeof p16) &&
		    k + 1 >= h && k + 1 < h + n)
			buf[k + 1 - h] = 2;
}

/* one request */
void
hf_serve(b)
	struct hf_box *b;
{
	char buf[1100], buf2[1100], *rel, *hp;
	unsigned long id, op = b->op;
	int e = 0, fd, n;

	b->error = 0;
	b->result = 0;
	b->name[HF_NAME - 1] = 0;
	b->name2[HF_NAME - 1] = 0;
	switch (op) {
	case HF_ROOT:
		b->node = 1;
		e = attrs(b, 1);
		break;
	case HF_LOOKUP:
		if (!child(b->node, b->name, buf)) {
			e = 2;
			break;
		}
		if ((id = idof(buf)) == 0) {
			e = 12;
			break;
		}
		b->node2 = id;
		e = attrs(b, id);
		break;
	case HF_GETATTR:
		e = attrs(b, b->node);
		break;
	case HF_SETATTR:
		/* in: flags, mode (permission bits), uid, gid, length (size), offset (mtime) */
		if (!(rel = pathof(b->node))) {
			e = 2;
			break;
		}
		{
			unsigned long fl = b->flags, sm = b->mode, su = b->uid, sg = b->gid, ss = b->length, stm = b->offset;
			struct ment *m;

			if ((e = attrs(b, b->node)) != 0)
				break;
			if (fl & (HF_SET_MODE | HF_SET_OWNER | HF_SET_TIMES)) {
				m = metaget(rel);
				setmeta(rel, (fl & HF_SET_MODE) ? (b->amode & S_IFMT) | (sm & 07777) : b->amode,
				    (fl & HF_SET_OWNER) && su != 0xffff ? su : b->auid, (fl & HF_SET_OWNER) && sg != 0xffff ? sg : b->agid,
				    (fl & HF_SET_TIMES) ? stm : b->mtime);
				(void)m;
			}
			if ((fl & HF_SET_SIZE) && b->type != HF_VCHR && b->type != HF_VBLK && ((hp = nolink(rel, 0)) == 0 || truncate(hp, (off_t)ss) < 0))
				e = errmap(errno);
		}
		if (!e)
			e = attrs(b, b->node);
		break;
	case HF_READ:
	case HF_WRITE:
		if (!(rel = pathof(b->node)) || b->length > HF_DATA) {
			e = 22;
			break;
		}
		if ((hp = nolink(rel, 0)) == 0 || (fd = open(hp, op == HF_READ ? O_RDONLY : O_WRONLY)) < 0) {
			e = errmap(errno);
			break;
		}
		lseek(fd, (off_t)b->offset, SEEK_SET);
		n = op == HF_READ ? read(fd, b->data, b->length) : write(fd, b->data, b->length);
		if (n < 0)
			e = errmap(errno);
		else
			b->result = n;
		if (op == HF_READ && n > 0)
			wsfix(fd, (long)b->offset, b->data, n);
		close(fd);
		break;
	case HF_READDIR: {
		/* entries from index b->offset: ino, namlen, name\0 packed; result = count */
		DIR *d;
		struct dirent *de;
		unsigned long i = 0, used = 0, count = 0, len;
		unsigned char *o = b->data;
		char nm[HF_NAME];

		if (!(rel = pathof(b->node))) {
			e = 2;
			break;
		}
		if ((hp = nolink(rel, 0)) == 0 || (d = opendir(hp)) == 0) {
			e = errmap(errno);
			break;
		}
		b->flags = 1;		/* set to 0 when the end is reached */
		while ((de = readdir(d)) != 0) {
			strncpy(nm, de->d_name, HF_NAME - 1);
			nm[HF_NAME - 1] = 0;
			n = strlen(nm);
			if (n > (int)strlen(SPECIAL) && strcmp(nm + n - strlen(SPECIAL), SPECIAL) == 0)
				nm[n - strlen(SPECIAL)] = 0;
			if (nm[0] == 0)
				continue;
			if (i++ < b->offset)
				continue;
			if (!child(b->node, nm, buf)) {
				continue;
			}
			len = (8 + strlen(nm) + 1 + 3) & ~3UL;
			if (used + len > b->length || used + len > HF_DATA)
				goto more;
			{
				unsigned long ino = idof(buf);

				o[used] = ino >> 24; o[used + 1] = ino >> 16; o[used + 2] = ino >> 8; o[used + 3] = ino;
			}
			o[used + 4] = len >> 8;
			o[used + 5] = len;
			o[used + 6] = strlen(nm) >> 8;
			o[used + 7] = strlen(nm);
			memset(o + used + 8, 0, len - 8);
			strcpy((char *)o + used + 8, nm);
			used += len;
			count++;
			b->offset = i;
		}
		b->flags = 0;
	more:
		closedir(d);
		if (b->flags)
			b->offset = i - 1;	/* the entry that did not fit is read next time */
		else
			b->offset = i;
		b->result = used;
		break;
	}
	case HF_CREATE:
	case HF_MKDIR:
	case HF_SYMLINK:
		if (!child(b->node, b->name, buf) || buf[0] == 0) {
			e = 22;
			break;
		}
		if ((hp = hostpath(buf, 0)) == 0) {
			e = errmap(errno);
			break;
		}
		if (op == HF_CREATE && (b->flags == HF_VCHR || b->flags == HF_VBLK || b->flags == HF_VSOCK)) {
			char sp[PATH_MAX + 32];
			struct stat st;
			FILE *sf;

			sprintf(sp, "%s%s", hp, SPECIAL);
			sf = lstat(sp, &st) == 0 && S_ISLNK(st.st_mode) ? (errno = ELOOP, (FILE *)0) : fopen(sp, "w");
			n = sf ? 0 : -1;
			if (sf) {
				fprintf(sf, "mode=%lo raw=%08lx\n", (b->flags == HF_VCHR ? S_IFCHR : b->flags == HF_VBLK ? S_IFBLK :
				    NX_IFSOCK) | (b->mode & 07777),
				    b->rdev & 0xffff);
				fclose(sf);
			}
		} else if (op == HF_CREATE)
			fd = open(hp, O_WRONLY | O_CREAT | O_EXCL, 0600), n = fd < 0 ? -1 : (close(fd), 0);
		else if (op == HF_MKDIR)
			n = mkdir(hp, 0700);
		else {
			b->data[b->length < HF_DATA ? b->length : HF_DATA - 1] = 0;
			n = symlink((char *)b->data, hp);
		}
		if (n < 0) {
			e = errmap(errno);
			break;
		}
		{
			unsigned long mt = op == HF_MKDIR ? S_IFDIR : op == HF_SYMLINK ? S_IFLNK :
			    b->flags == HF_VCHR ? S_IFCHR : b->flags == HF_VBLK ? S_IFBLK : b->flags == HF_VSOCK ? NX_IFSOCK :
			    S_IFREG;

			setmeta(buf, mt | (b->mode & 07777), b->uid, b->gid, time(0));
		}
		if ((id = idof(buf)) == 0) {
			e = 12;
			break;
		}
		b->node2 = id;
		e = attrs(b, id);
		break;
	case HF_READLINK:
		if (!(rel = pathof(b->node))) {
			e = 2;
			break;
		}
		n = (hp = hostpath(rel, 0)) == 0 ? -1 : readlink(hp, (char *)b->data, HF_DATA - 1);
		if (n < 0)
			e = errmap(errno);
		else
			b->result = n;
		break;
	case HF_REMOVE:
	case HF_RMDIR:
		if (!child(b->node, b->name, buf) || buf[0] == 0) {
			e = 22;
			break;
		}
		hp = hostpath(buf, 1);
		n = !hp ? -1 : op == HF_REMOVE ? unlink(hp) : rmdir(hp);
		if (n < 0)
			e = errmap(errno);
		break;
	case HF_RENAME:
	case HF_LINK: {
		struct ment *m;

		if (op == HF_LINK) {
			if (!(rel = pathof(b->node)) || !child(b->node2, b->name, buf2)) {
				e = 22;
				break;
			}
			strcpy(buf, rel);
		} else if (!child(b->node, b->name, buf) || !child(b->node2, b->name2, buf2) || !buf[0] || !buf2[0]) {
			e = 22;
			break;
		}
		/* the host's link() may follow a link it is given */
		hp = op == HF_LINK ? nolink(buf, 1) : hostpath(buf, 1);
		{
			char dst[2200];

			if (!hp)
				n = -1;
			else {
				strcpy(dst, hp);
				n = (hp = hostpath(buf2, 0)) == 0 ? -1 : op == HF_LINK ? link(dst, hp) : rename(dst, hp);
			}
		}
		if (n < 0) {
			e = errmap(errno);
			break;
		}
		if (op == HF_RENAME) {
			moveids(buf, buf2);
			if ((m = metaget(buf)) != 0)
				setmeta(buf2, m->mode, m->uid, m->gid, m->mtime);
		} else if ((m = metaget(buf)) != 0)
			setmeta(buf2, m->mode, m->uid, m->gid, m->mtime);
		break;
	}
	case HF_FSYNC:
	case HF_INACTIVE:
		break;
	case HF_STATFS: {
		struct statvfs sv;

		if (statvfs(root, &sv) < 0) {
			e = errmap(errno);
			break;
		}
		b->bsize = 8192;
		b->fblocks = blocks8k(sv.f_blocks, sv.f_frsize);
		b->ffree = blocks8k(sv.f_bavail, sv.f_frsize);
		b->files = sv.f_files > 0x7fffffffUL ? 0x7fffffffUL : sv.f_files;
		b->nfree = sv.f_favail > 0x7fffffffUL ? 0x7fffffffUL : sv.f_favail;
		break;
	}
	default:
		e = 22;
	}
	b->error = e;
	b->state = HF_REPLY;
}
