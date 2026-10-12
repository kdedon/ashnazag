#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/procfs.h>
#include <sys/procset.h>
#include <sys/priocntl.h>
#include <sys/tspriocntl.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "lisa_runtime.h"
#include "lisa_wire.h"
#include "lisa_services.h"

#define NSLOT 16
#define SLOTBYTES 0x20000UL
#define DATABASE 0xd60000UL
extern int lisa_lock_code(unsigned long);
extern int lisa_resident(void);
extern int lisa_slot_resident(unsigned long, unsigned long);
extern int gettimeofday(struct timeval *, void *);
extern int fchmod();

struct file_ref {
	int used, fd, mode, console, no_block, auto_lf;
	unsigned long mark;
	dev_t dev;
	ino_t ino;
	char name[256];
};
struct data_ref {
	int used, fd, ldsn, opens, readonly, deleted;
	unsigned long memory, disk;
	unsigned char *saved;
	char name[33];
};
struct channel {
	int used, head, count;
	unsigned char event[32][48];
};
struct binding { const char *name, *file; int state, writable, data; };
static const struct binding bindings[] = {
	{"FONT.HEUR", "font.heur", 0, 0, 0},
	{"FONT.LIB", "font.lib", 0, 0, 0},
	{"SYSTEM.DMALERTSPHRASE", "dmalerts.phr", 0, 0, 0},
	{"SYSTEM.PR.PHRASE", "pr.phr", 0, 0, 0},
	{"SYSTEM.BADPAGEMSG.LOTUS", "badpage.lotus", 0, 0, 0},
	{"{T101}ICON", "t101.icon", 0, 0, 0},
	{"{T140}ICON", "t140.icon", 0, 0, 0},
	{"{T271828}ICON", "t271828.icon", 0, 0, 0},
	{"{!SAVEDESKTOP}", "savedesktop", 1, 1, 0},
	{"{!CATALOG}", "catalog", 1, 1, 0},
	{"{!CATALOG}$R", "catalog.r", 1, 1, 0},
	{"SCRAP1.LOTUS", "s1lotus", 1, 1, 0},
	{"SCRAP1.FIGURES", "s1figures", 1, 1, 0},
	{"SCRAP2.LOTUS", "s2lotus", 1, 1, 0},
	{"SCRAP2.FIGURES", "s2figures", 1, 1, 0},
	{"{!DMHEAP}", "dmheap.seg", 1, 1, 1},
	{"SCRAPONE", "scrapone.seg", 1, 1, 1},
	{"SCRAPTWO", "scraptwo.seg", 1, 1, 1},
	{"{!DBBUFFER}", "dbbuffer.seg", 1, 1, 1},
	{"UTDSEG", "utd.seg", 1, 1, 1},
	{"<REWRITETEMPFILE>", "rwtemp", 1, 1, 0}
};
static struct file_ref files[16];
static struct data_ref objects[16];
static struct channel channels[4];
static char resource_root[1024], state_root[1024];
static unsigned char volume_header[512];
static char program_name[256];
static int initialized, media_fd = -1, console_fd[2] = {-1,-1};
static int console_no_block[2], console_auto_lf[2];
static int fence;

static unsigned int
get16(p)
const unsigned char *p;
{
	return ((unsigned int)p[0] << 8) | p[1];
}

static void
put16(p, n)
unsigned char *p;
unsigned int n;
{
	p[0] = n >> 8;
	p[1] = n;
}

static void *
argument(s, offset, size, write)
unsigned char *s;
int offset, write;
unsigned long size;
{
	unsigned long address;
	address = lisa_get32(s + offset);
	return lisa_pointer(address, size, write);
}

static int
error_out(s, offset, error)
unsigned char *s;
int offset, error;
{
	unsigned char *p;
	if (lisa_get32(s + offset) & 1)
		return -1;
	p = argument(s, offset, 2, 1);
	if (!p)
		return -1;
	put16(p, error);
	return 0;
}

static int
pascal(address, out, maximum, empty, data)
unsigned long address;
char *out;
int maximum, empty, data;
{
	unsigned char *p;
	int n, i, c;
	p = lisa_pointer(address, 1, 0);
	if (!p)
		return -1;
	n = p[0];
	if (n > maximum || (!empty && !n))
		return -1;
	p = lisa_pointer(address, n + 1, 0);
	if (!p)
		return -1;
	for (i = 0; i < n; ++i) {
		c = p[i + 1];
		if (c < 32 || c > 126 || (data && c == '-'))
			return -1;
		if (c >= 'a' && c <= 'z')
			c -= 'a' - 'A';
		out[i] = c;
	}
	out[n] = 0;
	return 0;
}

static void
put_string(p, text, maximum)
unsigned char *p;
const char *text;
int maximum;
{
	unsigned int n;
	n = strlen(text);
	if (n > maximum)
		n = maximum;
	p[0] = n;
	memcpy(p + 1, text, n);
}

static const char *
logical_name(name)
const char *name;
{
	if (!strncmp(name, "-PROFILE-", 9))
		return name + 9;
	if (!strncmp(name, "-#BOOT-", 7)) {
		if (!strcmp(name + 7, "FONT.HEUR") || !strcmp(name + 7, "FONT.LIB"))
			return name + 7;
	}
	if (!strncmp(name, "-AOS 3.0-", 9)) {
		if (!strcmp(name + 9, "FONT.HEUR") || !strcmp(name + 9, "FONT.LIB"))
			return name + 9;
	}
	return name;
}

static const struct binding *
binding(name, path)
const char *name;
char *path;
{
	int i;
	name = logical_name(name);
	for (i = 0; i < sizeof(bindings)/sizeof(bindings[0]); ++i)
		if (!strcmp(name, bindings[i].name)) {
			if (path)
				sprintf(path, "%s/%s", bindings[i].state ? state_root : resource_root,
					bindings[i].file);
			return &bindings[i];
		}
	return 0;
}

static int
access_error()
{
	return errno == EACCES || errno == EPERM || errno == EROFS;
}

static int
read_full(fd, bytes, count)
int fd;
unsigned char *bytes;
unsigned long count;
{
	long n;
	while (count) {
		n = read(fd, bytes, count);
		if (n <= 0)
			return -1;
		bytes += n;
		count -= n;
	}
	return 0;
}

static int
write_full(fd, bytes, count)
int fd;
const unsigned char *bytes;
unsigned long count;
{
	long n;
	while (count) {
		n = write(fd, bytes, count);
		if (n <= 0)
			return -1;
		bytes += n;
		count -= n;
	}
	return 0;
}

int
lisa_services_init(root, media)
const char *root, *media;
{
	struct stat st;
	char path[1200];
	unsigned char sentry[512], program_header[512];
	unsigned long hint;
	int i;
	if (initialized || !root || root[0] != '/' || strlen(root) > 950)
		return -1;
	if (stat(root, &st) || !S_ISDIR(st.st_mode) || access(root, R_OK|X_OK))
		return -1;
	sprintf(resource_root, "%s/resources", root);
	sprintf(state_root, "%s/state", root);
	if (stat(resource_root, &st) || !S_ISDIR(st.st_mode) || access(resource_root, R_OK|X_OK))
		return -1;
	if (stat(state_root, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
	    access(state_root, R_OK|W_OK|X_OK))
		return -1;
	if (!media) {
		sprintf(path, "%s/media", root);
		media = path;
	}
	media_fd = open(media, O_RDONLY);
	if (media_fd < 0)
		return -1;
	if (fstat(media_fd, &st) || !S_ISREG(st.st_mode) || st.st_size != 10424128 ||
	    lseek(media_fd, 30L * 536 + 24, SEEK_SET) < 0 ||
	    read_full(media_fd, volume_header, 512) || get16(volume_header) != 17 ||
	    lisa_get32(volume_header + 0x94) != 6 || get16(volume_header + 0x98) != 36) {
		close(media_fd);
		media_fd = -1;
		return -1;
	}
	if (lseek(media_fd, (30L + 6 + 41 / 36) * 536 + 24, SEEK_SET) < 0 ||
	    read_full(media_fd, sentry, 512))
		goto bad_media;
	hint = lisa_get32(sentry + (41 % 36) * 14);
	if (hint >= 19448 - 30 || !lisa_get32(sentry + (41 % 36) * 14 + 8) ||
	    lseek(media_fd, (30L + hint) * 536 + 24, SEEK_SET) < 0 ||
	    read_full(media_fd, program_header, 512) || !program_header[0])
		goto bad_media;
	for (i = 0; i < program_header[0]; ++i) {
		if (program_header[i + 1] < 32 || program_header[i + 1] > 126)
			goto bad_media;
		program_name[i] = program_header[i + 1];
	}
	program_name[i] = 0;
	lisa_program = program_name;
	initialized = 1;
	return 0;
bad_media:
	close(media_fd);
	media_fd = -1;
	return -1;
}

static struct file_ref *
file(reference)
unsigned int reference;
{
	if (reference < 0x101 || reference > 0x110 || !files[reference - 0x101].used)
		return 0;
	return &files[reference - 0x101];
}

static int
console_index(name)
const char *name;
{
	if (!strcmp(name, "MAINCONSOLE") || !strcmp(name, "-MAINCONSOLE") ||
	    !strncmp(name, "-MAINCONSOLE-", 13))
		return 0;
	if (!strcmp(name, "ALTCONSOLE") || !strcmp(name, "-ALTCONSOLE") ||
	    !strncmp(name, "-ALTCONSOLE-", 12))
		return 1;
	return -1;
}

static int
open_file(s, shift, protected)
unsigned char *s;
int shift, protected;
{
	char name[256], path[1200];
	const struct binding *b;
	struct stat st;
	struct flock lock;
	unsigned char *ref_out, *allowed;
	unsigned int mode;
	int fd, flags, i, j, ci, error;
	mode = get16(s + 4 + shift);
	ref_out = argument(s, 6 + shift, 2, 1);
	allowed = protected ? argument(s, 4, 1, 1) : 0;
	if (!ref_out || (protected && !allowed) || mode & ~31 || !(mode & 7) ||
	    pascal(lisa_get32(s + 10 + shift), name, 255, 0, 0))
		return -1;
	if (protected)
		*allowed = 0;
	ci = console_index(name);
	b = binding(name, path);
	if (!b && ci < 0)
		return -1;
	if (b && (!b->writable && (mode & 6)))
		return error_out(s, 14 + shift, 1130);
	for (i = 0; i < 16 && files[i].used; ++i)
		;
	if (i == 16)
		return error_out(s, 14 + shift, 947);
	flags = mode & 6 ? (mode & 1 ? O_RDWR : O_WRONLY) : O_RDONLY;
	if (mode & 4)
		flags |= O_APPEND;
	fd = ci < 0 ? open(path, flags) : (console_fd[ci] < 0 ? -1 : dup(console_fd[ci]));
	if (fd < 0) {
		error = access_error() ? 1130 : errno == ENOENT && ci < 0 ? 888 : -1;
		return error < 0 ? -1 : error_out(s, 14 + shift, error);
	}
	if (fstat(fd, &st) || (ci < 0 ? !S_ISREG(st.st_mode) : !S_ISCHR(st.st_mode))) {
		close(fd);
		return -1;
	}
	for (j = 0; j < 16; ++j)
		if (files[j].used && files[j].dev == st.st_dev && files[j].ino == st.st_ino &&
		    ((files[j].mode | mode) & 8)) {
			close(fd);
			return error_out(s, 14 + shift, 873);
		}
	if (mode & 8) {
		memset(&lock, 0, sizeof(lock));
		lock.l_type = mode & 6 ? F_WRLCK : F_RDLCK;
		lock.l_whence = SEEK_SET;
		if (fcntl(fd, F_SETLK, &lock) < 0) {
			error = errno == EAGAIN || errno == EACCES ? 873 : -1;
			close(fd);
			return error < 0 ? -1 : error_out(s, 14 + shift, error);
		}
	}
	memset(&files[i], 0, sizeof(files[i]));
	files[i].used = 1;
	files[i].fd = fd;
	files[i].mode = mode;
	files[i].console = ci >= 0;
	if (ci >= 0) {
		files[i].no_block = console_no_block[ci];
		files[i].auto_lf = console_auto_lf[ci];
	}
	files[i].dev = st.st_dev;
	files[i].ino = st.st_ino;
	strcpy(files[i].name, logical_name(name));
	put16(ref_out, 0x101 + i);
	if (allowed)
		*allowed = 1;
	return error_out(s, 14 + shift, 0);
}

static int
io_file(s, writing)
unsigned char *s;
int writing;
{
	struct file_ref *f;
	struct stat st;
	unsigned char *buffer, *actual, *scratch;
	unsigned long count, mark;
	long offset, done, n;
	int mode, i;
	f = file(get16(s + 22));
	if (!f)
		return error_out(s, 24, 961);
	if (writing ? !(f->mode & 6) : !(f->mode & 1))
		return error_out(s, 24, writing ? 962 : 955);
	count = lisa_get32(s + 14);
	mode = s[8];
	mark = lisa_get32(s + 4);
	offset = mark & 0x80000000UL ? -(long)(~mark & 0xffffffffUL) - 1 : (long)mark;
	actual = argument(s, 10, 4, 1);
	buffer = count ? argument(s, 18, count, !writing) : s;
	if (count > SLOTBYTES || !actual || !buffer || mode > 2)
		return -1;
	mark = f->mark;
	if (mode == 0) {
		if (offset < 0)
			return -1;
		mark = offset;
	} else if (mode == 1) {
		if ((offset < 0 && (unsigned long)(-(offset + 1)) + 1 > mark) ||
		    (offset >= 0 && (unsigned long)offset > 0x7fffffffUL - mark))
			return -1;
		mark += offset;
	}
	if (writing && (f->mode & 4) && !(f->mode & 2) && mode != 2)
		return -1;
	if (writing && !f->console && (f->mode & 4)) {
		int flags;
		flags = fcntl(f->fd, F_GETFL, 0);
		if (flags < 0 || fcntl(f->fd, F_SETFL,
		    mode == 2 ? flags | O_APPEND : flags & ~O_APPEND) < 0)
			return -1;
	}
	if (!f->console && lseek(f->fd, mark, SEEK_SET) < 0)
		return -1;
	scratch = malloc(count ? count : 1);
	if (!scratch)
		return -1;
	if (writing)
		memcpy(scratch, buffer, count);
	done = 0;
	if (writing && f->console && f->auto_lf) {
		for (i = 0; i < count; ++i) {
			n = write(f->fd, scratch + i, 1);
			if (n != 1)
				break;
			++done;
			if (scratch[i] == 13 && write(f->fd, "\n", 1) != 1)
				break;
		}
	} else {
		done = writing ? write(f->fd, scratch, count) : read(f->fd, scratch, count);
		if (done < 0 && f->console && f->no_block && (errno == EAGAIN || errno == EWOULDBLOCK))
			done = 0;
	}
	if (done < 0) {
		free(scratch);
		return -1;
	}
	if (!writing)
		memcpy(buffer, scratch, done);
	free(scratch);
	if (writing && (f->mode & 4) && mode == 2 && !f->console) {
		if (fstat(f->fd, &st) || st.st_size > 0x7fffffffL)
			return -1;
		f->mark = st.st_size;
	} else {
		if (mark > 0x7fffffffUL - done)
			return -1;
		f->mark = mark + done;
	}
	lisa_put32(actual, done);
	return error_out(s, 24, done != count && !f->console ? 848 : 0);
}

static unsigned long
epoch(seconds)
time_t seconds;
{
	return seconds < 315532800L ? 0 : seconds - 315532800L;
}

static void
file_fields(p, st)
unsigned char *p;
struct stat *st;
{
	lisa_put32(p + 290, st->st_dev);
	p[299] = 2;
	lisa_put32(p + 300, st->st_size);
	lisa_put32(p + 304, st->st_blocks * 512);
	put16(p + 308, 512);
	p[310] = 14;
	p[311] = 3;
	lisa_put32(p + 312, epoch(st->st_ctime));
	lisa_put32(p + 316, epoch(st->st_atime));
	lisa_put32(p + 320, epoch(st->st_mtime));
	lisa_put32(p + 346, st->st_dev);
	lisa_put32(p + 350, st->st_ino);
}

static int
info_file(s)
unsigned char *s;
{
	struct file_ref *f;
	struct stat st;
	unsigned char *p;
	int i, readers, writers, users;
	f = file(get16(s + 8));
	if (!f)
		return error_out(s, 10, 961);
	p = argument(s, 4, 380, 1);
	if (!p || fstat(f->fd, &st))
		return -1;
	memset(p, 0, 380);
	put_string(p, f->name, 32);
	file_fields(p, &st);
	put16(p + 332, get16(s + 8));
	lisa_put32(p + 334, f->mark);
	put16(p + 338, f->mode);
	readers = writers = users = 0;
	for (i = 0; i < 16; ++i)
		if (files[i].used && files[i].dev == st.st_dev && files[i].ino == st.st_ino) {
			++users;
			if (files[i].mode & 1)
				++readers;
			if (files[i].mode & 6)
				++writers;
		}
	put16(p + 340, readers);
	put16(p + 342, writers);
	put16(p + 344, users);
	p[360] = f->mark >= st.st_size;
	p[363] = !(f->mode & 16);
	return error_out(s, 10, 0);
}

static int
lookup(s)
unsigned char *s;
{
	static const unsigned short copy[][3] = {
		{398,0x00,2}, {400,0x0a,2}, {402,0x02,8}, {410,0x118,8},
		{418,0x136,2}, {420,0x7c,6}, {426,0xb0,2}, {428,0x120,2},
		{430,0xba,4}, {434,0x58,16}, {450,0x54,4}, {454,0x68,4},
		{458,0xc4,8}, {466,0x126,4}
	};
	char name[256], path[1200];
	struct stat st;
	const struct binding *b;
	unsigned char *p;
	int i, refs;
	p = argument(s, 4, 478, 1);
	if (!p || pascal(lisa_get32(s + 8), name, 255, 0, 0))
		return -1;
	if (!strcmp(name, "-PROFILE") || !strcmp(name, "PROFILE")) {
		memset(p, 0, 478);
		put_string(p, "profile", 32);
		put_string(p + 34, "-", 255);
		lisa_put32(p + 290, lisa_get32(volume_header + 0x50));
		memcpy(p + 294, volume_header + 0x122, 4);
		p[299] = 1;
		put16(p + 302, 1);
		lisa_put32(p + 310, lisa_get32(volume_header + 0x74) + 1);
		lisa_put32(p + 314, lisa_get32(volume_header + 0x70) + 1);
		p[318] = 1;
		p[319] = initialized && media_fd >= 0;
		refs = 1;
		for (i = 0; i < 16; ++i) {
			refs += files[i].used;
			refs += objects[i].used && objects[i].fd >= 0;
		}
		lisa_put32(p + 320, refs);
		if (volume_header[12] > 32)
			return -1;
		memcpy(p + 330, volume_header + 12, volume_header[12] + 1);
		for (i = 0; i < sizeof(copy)/sizeof(copy[0]); ++i)
			memcpy(p + copy[i][0], volume_header + copy[i][1], copy[i][2]);
		p[470] = getuid() == 0;
		p[471] = access(state_root, W_OK) != 0;
		p[474] = volume_header[0x113];
		p[475] = volume_header[0x112];
		p[476] = volume_header[0x138] != 0;
		return error_out(s, 12, 0);
	}
	b = binding(name, path);
	if (!b || b->data)
		return -1;
	if (stat(path, &st) || !S_ISREG(st.st_mode))
		return -1;
	memset(p, 0, 478);
	put_string(p, b->name, 32);
	put_string(p + 34, "-profile", 255);
	file_fields(p, &st);
	for (i = 0; i < 16; ++i)
		if (files[i].used && files[i].dev == st.st_dev && files[i].ino == st.st_ino) {
			put16(p + 340, get16(p + 340) + !!(files[i].mode & 1));
			put16(p + 342, get16(p + 342) + !!(files[i].mode & 6));
			put16(p + 344, get16(p + 344) + 1);
		}
	return error_out(s, 12, 0);
}

static struct data_ref *
data(reference)
unsigned int reference;
{
	return reference >= 1 && reference <= 16 && objects[reference - 1].used && objects[reference - 1].opens ?
		&objects[reference - 1] : 0;
}

static unsigned long
slot_base(ldsn)
int ldsn;
{
	return DATABASE + (ldsn - 1) * SLOTBYTES;
}

static int
slot_free(ldsn)
int ldsn;
{
	int i;
	for (i = 0; i < 16; ++i)
		if (objects[i].used && objects[i].ldsn == ldsn)
			return 0;
	return 1;
}

static int
data_sync(d)
struct data_ref *d;
{
	unsigned char *bytes, *scratch;
	unsigned long n;
	int result;
	if (d->fd < 0 || d->deleted)
		return 0;
	n = d->memory < d->disk ? d->memory : d->disk;
	bytes = d->ldsn > 0 ? lisa_pointer(slot_base(d->ldsn), n, 0) : d->saved;
	if (!bytes)
		return -1;
	scratch = malloc(n ? n : 1);
	if (!scratch)
		return -1;
	memcpy(scratch, bytes, n);
	result = lseek(d->fd, 0, SEEK_SET) < 0 || write_full(d->fd, scratch, n) || fsync(d->fd);
	free(scratch);
	return result ? -1 : 0;
}

static int
data_bind(d, ldsn)
struct data_ref *d;
int ldsn;
{
	unsigned char *p;
	if (lisa_protect(slot_base(ldsn), SLOTBYTES, PROT_READ|PROT_WRITE))
		return -1;
	p = lisa_pointer(slot_base(ldsn), SLOTBYTES, 1);
	if (!p)
		goto failed;
	memset(p, 0, SLOTBYTES);
	if (d->saved)
		memcpy(p, d->saved, d->memory);
	else if (d->fd >= 0) {
		unsigned char *scratch;
		unsigned long n;
		n = d->memory < d->disk ? d->memory : d->disk;
		scratch = malloc(n);
		if (!scratch)
			goto failed;
		if (lseek(d->fd, 0, SEEK_SET) < 0 || read_full(d->fd, scratch, n)) {
			free(scratch);
			goto failed;
		}
		memcpy(p, scratch, n);
		free(scratch);
	}
	if (d->readonly && lisa_protect(slot_base(ldsn), SLOTBYTES, PROT_READ))
		goto failed;
	free(d->saved);
	d->saved = 0;
	d->ldsn = ldsn;
	return 0;
failed:
	if (lisa_protect(slot_base(ldsn), SLOTBYTES, PROT_NONE))
		fprintf(stderr, "lisa: failed to revoke data slot %d\n", ldsn);
	return -1;
}

static int
data_open(s, creating)
unsigned char *s;
int creating;
{
	char name[33], path[1200];
	const struct binding *b;
	struct data_ref *d;
	struct stat st;
	unsigned char *base_out, *ref_out;
	unsigned long memory, disk;
	int ldsn, i, existing, fd, bo, ro, no, eo;
	bo = creating ? 8 : 6;
	ro = creating ? 12 : 10;
	no = creating ? 24 : 14;
	eo = creating ? 28 : 18;
	ldsn = get16(s + (creating ? 6 : 4));
	base_out = argument(s, bo, 4, 1);
	ref_out = argument(s, ro, 2, 1);
	if (!base_out || !ref_out || ldsn < 1 || ldsn > 16 ||
	    pascal(lisa_get32(s + no), name, 32, 0, 1))
		return -1;
	put16(ref_out, 0xffff);
	if (creating && s[4] != 0)
		return -1;
	if (!slot_free(ldsn))
		return error_out(s, eo, 304);
	b = binding(name, path);
	existing = -1;
	for (i = 0; i < 16; ++i)
		if (objects[i].used && !objects[i].deleted && !strcmp(objects[i].name, name))
			existing = i;
	if (creating && existing >= 0)
		return error_out(s, eo, 308);
	if (!creating && existing >= 0) {
		d = &objects[existing];
		if (d->ldsn > 0)
			return error_out(s, eo, 304);
		if (d->disk && d->fd < 0) {
			if (!b || !b->data)
				b = binding(name, path);
			if (!b || !b->data)
				return -1;
			d->fd = open(path, O_RDWR);
			if (d->fd < 0)
				return errno == ENOENT ? error_out(s, eo, 313) : -1;
		}
		if (data_bind(d, ldsn))
			return -1;
		++d->opens;
		put16(ref_out, existing + 1);
		lisa_put32(base_out, slot_base(ldsn));
		return error_out(s, eo, 0);
	}
	for (i = 0; i < 16 && objects[i].used; ++i)
		;
	if (i == 16)
		return error_out(s, eo, 311);
	fd = -1;
	if (creating) {
		memory = lisa_get32(s + 20);
		disk = lisa_get32(s + 16);
		if (!memory || memory > SLOTBYTES || disk > SLOTBYTES || (disk && (!b || !b->data)))
			return -1;
		memory = (memory + 511) & ~511UL;
		disk = (disk + 511) & ~511UL;
		if (disk) {
			fd = open(path, O_RDWR|O_CREAT|O_EXCL, 0600);
			if (fd < 0)
				return errno == EEXIST ? error_out(s, eo, 308) : -1;
			if (ftruncate(fd, disk)) {
				close(fd);
				unlink(path);
				return -1;
			}
		}
	} else {
		if (!b || !b->data)
			return error_out(s, eo, 313);
		fd = open(path, O_RDWR);
		if (fd < 0)
			return errno == ENOENT ? error_out(s, eo, 313) : -1;
		if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
		    st.st_size > SLOTBYTES || st.st_size & 511) {
			close(fd);
			return -1;
		}
		memory = disk = st.st_size;
	}
	d = &objects[i];
	memset(d, 0, sizeof(*d));
	d->fd = fd;
	d->memory = memory;
	d->disk = disk;
	strcpy(d->name, name);
	if (data_bind(d, ldsn)) {
		if (fd >= 0)
			close(fd);
		if (creating && disk)
			unlink(path);
		memset(d, 0, sizeof(*d));
		return -1;
	}
	d->used = 1;
	d->opens = 1;
	put16(ref_out, i + 1);
	lisa_put32(base_out, slot_base(ldsn));
	return error_out(s, eo, 0);
}

static int
data_unbind(d)
struct data_ref *d;
{
	unsigned char *p, *saved;
	if (data_sync(d))
		return -1;
	p = lisa_pointer(slot_base(d->ldsn), d->memory, 0);
	saved = malloc(SLOTBYTES);
	if (!p || !saved) {
		free(saved);
		return -1;
	}
	memcpy(saved, p, d->memory);
	memset(saved + d->memory, 0, SLOTBYTES - d->memory);
	if (lisa_protect(slot_base(d->ldsn), SLOTBYTES, PROT_NONE)) {
		free(saved);
		return -1;
	}
	d->saved = saved;
	/* Retain the LDSN for rebinding while its address is inaccessible. */
	d->ldsn = -d->ldsn;
	return 0;
}

static int
data_size(s)
unsigned char *s;
{
	struct data_ref *d;
	unsigned char *mo, *do_, *p;
	unsigned long md, dd, memory, disk;
	d = data(get16(s + 20));
	if (!d)
		return error_out(s, 22, 303);
	mo = argument(s, 12, 4, 1);
	do_ = argument(s, 4, 4, 1);
	md = lisa_get32(s + 16);
	dd = lisa_get32(s + 8);
	if (!mo || !do_ || md > SLOTBYTES - d->memory || dd > SLOTBYTES - d->disk ||
	    (dd && d->fd < 0))
		return -1;
	memory = (d->memory + md + 511) & ~511UL;
	disk = (d->disk + dd + 511) & ~511UL;
	if (d->readonly && md)
		return -1;
	if (dd && ftruncate(d->fd, disk))
		return -1;
	if (md) {
		p = d->ldsn > 0 ? lisa_pointer(slot_base(d->ldsn), memory, 1) : d->saved;
		if (!p)
			return -1;
		memset(p + d->memory, 0, memory - d->memory);
	}
	d->memory = memory;
	d->disk = disk;
	lisa_put32(mo, memory);
	lisa_put32(do_, disk);
	return error_out(s, 22, 0);
}

static int
data_info(s)
unsigned char *s;
{
	struct data_ref *d;
	unsigned char *p;
	int resident;
	d = data(get16(s + 8));
	if (!d)
		return error_out(s, 10, 303);
	p = argument(s, 4, 54, 1);
	if (!p)
		return -1;
	resident = d->saved != 0;
	if (d->ldsn > 0) {
		resident = lisa_slot_resident(slot_base(d->ldsn), d->memory);
		if (resident < 0)
			return -1;
	}
	memset(p, 0, 54);
	lisa_put32(p, d->memory);
	lisa_put32(p + 4, d->disk);
	put16(p + 8, d->opens);
	put16(p + 10, d->ldsn > 0 ? d->ldsn : 0xffff);
	p[12] = d->ldsn > 0;
	p[13] = resident;
	p[14] = 1;
	p[15] = !d->readonly;
	lisa_put32(p + 16, d->ldsn > 0 ? slot_base(d->ldsn) : 0);
	put_string(p + 20, d->name, 32);
	return error_out(s, 10, 0);
}

static struct channel *
channel(reference)
unsigned int reference;
{
	return reference >= 0x201 && reference <= 0x204 && channels[reference - 0x201].used ?
		&channels[reference - 0x201] : 0;
}

static int
make_channel(s)
unsigned char *s;
{
	char name[256];
	unsigned char *out;
	int i;
	if (s[4] > 1 || pascal(lisa_get32(s + 6), name, 255, 1, 0) || name[0] ||
	    pascal(lisa_get32(s + 14), name, 255, 1, 0) || name[0])
		return -1;
	out = argument(s, 10, 2, 1);
	if (!out)
		return -1;
	if (!s[4])
		return error_out(s, 18, 420);
	for (i = 0; i < 4 && channels[i].used; ++i)
		;
	if (i == 4)
		return error_out(s, 18, 403);
	memset(&channels[i], 0, sizeof(channels[i]));
	channels[i].used = 1;
	put16(out, 0x201 + i);
	return error_out(s, 18, 0);
}

static int
send_event(s)
unsigned char *s;
{
	struct channel *c;
	unsigned char *event, *interval, *payload;
	c = channel(get16(s + 16));
	if (!c)
		return error_out(s, 18, 401);
	interval = argument(s, 8, 6, 0);
	payload = argument(s, 12, 40, 0);
	if (!interval || !payload || !argument(s, 4, 10, 0))
		return -1;
	if (lisa_get32(interval) || get16(interval + 4))
		return error_out(s, 18, 1199);
	if (c->count == 32)
		return error_out(s, 18, 402);
	event = c->event[(c->head + c->count) % 32];
	lisa_put32(event, getpid());
	lisa_put32(event + 4, 1);
	memcpy(event + 8, payload, 40);
	++c->count;
	return error_out(s, 18, 0);
}

static int
wait_event(s)
unsigned char *s;
{
	struct channel *c;
	unsigned char *list, *out, *ref_out;
	unsigned int count, references[11];
	int i;
	list = argument(s, 12, 24, 0);
	out = argument(s, 4, 48, 1);
	ref_out = argument(s, 8, 2, 1);
	if (!list || !out || !ref_out)
		return -1;
	count = get16(list);
	if (count < 1 || count > 11)
		return error_out(s, 16, 427);
	for (i = 0; i < count; ++i) {
		references[i] = get16(list + 2 + 2 * i);
		if (!channel(references[i]))
			return error_out(s, 16, 401);
	}
	for (;;) {
		for (i = 0; i < count; ++i) {
			c = channel(references[i]);
			if (!c)
				return error_out(s, 16, 401);
			if (c->count) {
				memcpy(out, c->event[c->head], 48);
				c->head = (c->head + 1) % 32;
				--c->count;
				put16(ref_out, references[i]);
				return error_out(s, 16, 0);
			}
		}
		if (lisa_wait_tick())
			return errno == EINTR ? error_out(s, 16, EINTR) : -1;
	}
}

static int
pm_transfer(bytes, writing)
unsigned char *bytes;
int writing;
{
	char path[1200];
	struct stat st;
	unsigned char scratch[64];
	int fd, result;
	sprintf(path, "%s/pmem.raw", state_root);
	fd = open(path, writing ? O_WRONLY|O_CREAT|O_TRUNC : O_RDONLY, 0600);
	if (fd < 0)
		return !writing && errno == ENOENT ? -622 : -1;
	if (writing) {
		memcpy(scratch, bytes, 64);
		result = fchmod(fd, 0600) || write_full(fd, scratch, 64) || fsync(fd) ? -1 : 0;
	} else {
		result = fstat(fd, &st) ? -1 : st.st_size != 64 ? -622 : read_full(fd, scratch, 64);
		if (!result)
			memcpy(bytes, scratch, 64);
	}
	if (close(fd) && !result)
		result = -1;
	return result;
}

static int
process_info(s)
unsigned char *s;
{
	char path[80], name[256];
	unsigned char *out;
	prpsinfo_t info;
	int fd, result, resident;
	if (lisa_get32(s + 8) != (unsigned long)getpid())
		return error_out(s, 12, 100);
	out = argument(s, 4, 268, 1);
	if (!out || !lisa_program || strlen(lisa_program) > 246)
		return -1;
	sprintf(path, "/proc/%ld", (long)getpid());
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	result = ioctl(fd, PIOCPSINFO, &info);
	close(fd);
	resident = lisa_resident();
	if (result || resident < 0)
		return -1;
	memset(out, 0, 268);
	sprintf(name, "-profile-%s", lisa_program);
	put_string(out, name, 255);
	lisa_put32(out + 256, getpid());
	lisa_put32(out + 260, getppid());
	out[264] = info.pr_pri;
	out[265] = info.pr_sname == 'S' ? 2 : info.pr_sname == 'T' ? 1 : 0;
	out[266] = resident;
	return error_out(s, 12, 0);
}

static int
set_priority(s)
unsigned char *s;
{
	pcparms_t current;
	pcinfo_t classinfo;
	tsparms_t *ts;
	unsigned int priority;
	if (lisa_get32(s + 6) != (unsigned long)getpid())
		return error_out(s, 10, 100);
	priority = get16(s + 4);
	if (priority < 1 || priority > 225)
		return error_out(s, 10, 110);
	memset(&classinfo, 0, sizeof(classinfo));
	strcpy(classinfo.pc_clname, "TS");
	if (priocntl(0, 0, PC_GETCID, (char *)&classinfo) < 0)
		return -1;
	memset(&current, 0, sizeof(current));
	current.pc_cid = classinfo.pc_cid;
	if (priocntl(P_PID, getpid(), PC_GETPARMS, (char *)&current) < 0)
		return -1;
	ts = (tsparms_t *)current.pc_clparms;
	ts->ts_uprilim = TS_NOCHANGE;
	ts->ts_upri = ((priority - 1) * 40 / 224) - 20;
	if (priocntl(P_PID, getpid(), PC_SETPARMS, (char *)&current) < 0)
		return -1;
	return error_out(s, 10, 0);
}

static int
mount_console(s)
unsigned char *s;
{
	char name[33];
	struct stat st;
	unsigned char *out, *password;
	int ci, fd, i, flags, tty_error, password_length;
	if (pascal(lisa_get32(s + 4), name, 32, 0, 0)) {
		fprintf(stderr, "lisa: MountConsole invalid device pointer %lx\n", lisa_get32(s + 4));
		return -1;
	}
	ci = !strcmp(name, "MAINCONSOLE") ? 0 : !strcmp(name, "ALTCONSOLE") ? 1 : -1;
	if (ci < 0) {
		fprintf(stderr, "lisa: MountConsole unsupported device %s\n", name);
		return -1;
	}
	password = argument(s, 8, 1, 0);
	password_length = password ? password[0] : -1;
	if (password_length < 0 || password_length > 32 ||
	    !argument(s, 8, password_length + 1, 0)) {
		fprintf(stderr, "lisa: MountConsole password pointer %lx length %d\n",
			lisa_get32(s + 8), password_length);
		return -1;
	}
	out = argument(s, 12, strlen(name) + 1, 1);
	if (!out) {
		fprintf(stderr, "lisa: MountConsole output pointer %lx size %lu\n",
			lisa_get32(s + 12), (unsigned long)strlen(name) + 1);
		return -1;
	}
	if (console_fd[ci] >= 0)
		return error_out(s, 16, 1052);
	fd = open("/dev/tty", O_RDWR);
	tty_error = fd < 0 ? errno : 0;
	if (fd >= 0) {
		if (fstat(fd, &st) || !S_ISCHR(st.st_mode)) {
			tty_error = errno;
			close(fd);
			fd = -1;
		}
	}
	if (fd < 0) {
		for (i = 0; i < 3; ++i) {
			flags = fcntl(i, F_GETFL, 0);
			if (flags < 0 || (flags & O_ACCMODE) != O_RDWR || fstat(i, &st) || !S_ISCHR(st.st_mode))
				continue;
			fd = dup(i);
			if (fd >= 0)
				break;
		}
	}
	if (fd < 0) {
		fprintf(stderr, "lisa: MountConsole %s no read/write character vnode; tty errno %d\n",
			name, tty_error);
		for (i = 0; i < 3; ++i) {
			flags = fcntl(i, F_GETFL, 0);
			if (fstat(i, &st))
				fprintf(stderr, "lisa: inherited fd %d flags %x stat errno %d\n", i, flags, errno);
			else
				fprintf(stderr, "lisa: inherited fd %d flags %x mode %lo dev %lu inode %lu\n",
					i, flags, (unsigned long)st.st_mode, (unsigned long)st.st_dev,
					(unsigned long)st.st_ino);
		}
		return -1;
	}
	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		fprintf(stderr, "lisa: MountConsole %s flags errno %d\n", name, errno);
		close(fd);
		return -1;
	}
	console_fd[ci] = fd;
	console_no_block[ci] = !!(flags & O_NONBLOCK);
	console_auto_lf[ci] = 0;
	put_string(out, name, 32);
	return error_out(s, 16, 0);
}

static int
control_console(s)
unsigned char *s;
{
	char name[256];
	unsigned char *p;
	int i, ci, operation, enabled, flags;
	if (pascal(lisa_get32(s + 8), name, 255, 0, 0)) {
		fprintf(stderr, "lisa: DeviceControl invalid path pointer %lx\n", lisa_get32(s + 8));
		return -1;
	}
	ci = console_index(name);
	p = argument(s, 4, 44, 0);
	if (ci < 0 || !p || console_fd[ci] < 0) {
		fprintf(stderr, "lisa: DeviceControl path %s index %d record %lx mounted %d\n",
			name, ci, lisa_get32(s + 4), ci >= 0 && console_fd[ci] >= 0);
		return -1;
	}
	operation = get16(p + 2);
	enabled = lisa_get32(p + 4) != 0;
	if (get16(p) != 2 || (operation != 6 && operation != 17)) {
		fprintf(stderr, "lisa: DeviceControl version %u code %d value %lu\n",
			get16(p), operation, lisa_get32(p + 4));
		return -1;
	}
	if (operation == 6) {
		flags = fcntl(console_fd[ci], F_GETFL, 0);
		if (flags < 0 || fcntl(console_fd[ci], F_SETFL,
		    enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) < 0) {
			fprintf(stderr, "lisa: DeviceControl %s native flags errno %d\n", name, errno);
			return -1;
		}
		console_no_block[ci] = enabled;
	} else
		console_auto_lf[ci] = enabled;
	for (i = 0; i < 16; ++i)
		if (files[i].used && files[i].console && console_index(files[i].name) == ci) {
			files[i].no_block = console_no_block[ci];
			files[i].auto_lf = console_auto_lf[ci];
		}
	return error_out(s, 12, 0);
}

static int
kill_data(s)
unsigned char *s;
{
	char name[33], path[1200];
	const struct binding *b;
	int i, found;
	if (pascal(lisa_get32(s + 4), name, 32, 0, 1))
		return -1;
	b = binding(name, path);
	found = 0;
	for (i = 0; i < 16; ++i)
		if (objects[i].used && !objects[i].deleted && !strcmp(objects[i].name, name))
			found = 1;
	if (b && b->data) {
		if (unlink(path))
			return error_out(s, 8, errno == ENOENT ? 313 : 312);
		found = 1;
	}
	if (!found)
		return error_out(s, 8, 313);
	for (i = 0; i < 16; ++i)
		if (objects[i].used && !objects[i].deleted && !strcmp(objects[i].name, name)) {
			objects[i].deleted = 1;
			if (!objects[i].opens) {
				free(objects[i].saved);
				memset(&objects[i], 0, sizeof(objects[i]));
			}
		}
	return error_out(s, 8, 0);
}

static int
close_data(s)
unsigned char *s;
{
	struct data_ref *d;
	d = data(get16(s + 4));
	if (!d || !d->opens)
		return error_out(s, 6, 303);
	if (data_sync(d))
		return -1;
	if (d->ldsn > 0 && data_unbind(d))
		return -1;
	--d->opens;
	if (!d->opens) {
		if (d->fd >= 0 && close(d->fd))
			return -1;
		d->fd = -1;
		if (d->deleted) {
			free(d->saved);
			memset(d, 0, sizeof(*d));
		}
	}
	return error_out(s, 6, 0);
}

static int
get_time(s)
unsigned char *s;
{
	unsigned char *p;
	struct timeval now;
	struct tm *utc;
	p = argument(s, 4, 10, 1);
	if (!p || gettimeofday(&now, 0))
		return -1;
	utc = gmtime(&now.tv_sec);
	if (!utc)
		return -1;
	put16(p, utc->tm_year + 1900);
	put16(p + 2, utc->tm_yday + 1);
	p[4] = utc->tm_hour;
	p[5] = utc->tm_min;
	p[6] = utc->tm_sec;
	p[7] = 0;
	put16(p + 8, now.tv_usec / 1000);
	return error_out(s, 8, 0);
}

static int
error_offset(selector)
unsigned int selector;
{
	switch (selector) {
	case 15: case 31: case 34: case 51: case 71:
	case 143: case 145: case 146: case 147: case 150: return 8;
	case 16: case 17: case 25: case 53: case 82: case 90: case 126: case 133: return 12;
	case 18: case 57: case 73: case 75: case 99: case 100:
		return selector == 73 || selector == 99 ? 6 : 10;
	case 19: return 14;
	case 20: case 28: case 29: case 58: case 61: case 77: case 78: case 102: return 6;
	case 21: case 22: return 24;
	case 33: case 101: case 148: case 149: return 16;
	case 50: case 72: case 98: case 103: case 121: case 135: return 18;
	case 70: return 28;
	case 74: return 22;
	case 76: return 8;
	case 105: return 8;
	case 115: return 4;
	default: return -1;
	}
}

int
lisa_os_dispatch(selector, s, regs)
unsigned int selector;
unsigned char *s;
unsigned long regs[16];
{
	unsigned char *p, *q, *r;
	struct file_ref *f;
	struct data_ref *d;
	struct channel *c;
	const struct binding *b;
	char name[256], other[256], path[1200], second[1200];
	unsigned long address;
	int i, eo, result, ldsn;
	eo = error_offset(selector);
	if (!initialized || (eo >= 0 && ((lisa_get32(s + eo) & 1) || !argument(s, eo, 2, 1))))
		return -1;
	switch (selector) {
	case 2:
		address = lisa_get32(s + 4);
		if ((address & 1) || !lisa_executable(address, 2))
			return -1;
		lisa_set_init(address);
		return 0;
	case 15:
		if (pascal(lisa_get32(s + 4), name, 255, 0, 0))
			return -1;
		b = binding(name, path);
		if (!b || strcmp(b->name, "<REWRITETEMPFILE>"))
			return -1;
		if (unlink(path))
			return errno == ENOENT ? error_out(s, 8, 888) : -1;
		return error_out(s, 8, 0);
	case 16:
		if (pascal(lisa_get32(s + 4), name, 255, 0, 0) ||
		    pascal(lisa_get32(s + 8), other, 255, 0, 0) ||
		    strcmp(logical_name(name), "{!CATALOG}") || strcmp(logical_name(other), "{#CATALOG}"))
			return -1;
		sprintf(path, "%s/oldcatalog", state_root);
		sprintf(second, "%s/catalog", state_root);
		if (rename(path, second))
			return errno == ENOENT ? error_out(s, 12, 1022) : -1;
		return error_out(s, 12, 0);
	case 17: return lookup(s);
	case 18: return info_file(s);
	case 19: return open_file(s, 0, 0);
	case 20:
		f = file(get16(s + 4));
		if (!f)
			return error_out(s, 6, 961);
		if (close(f->fd))
			return -1;
		memset(f, 0, sizeof(*f));
		return error_out(s, 6, 0);
	case 21: return io_file(s, 0);
	case 22: return io_file(s, 1);
	case 25: return control_console(s);
	case 28: case 29:
		f = file(get16(s + 4));
		if (!f)
			return error_out(s, 6, 961);
		if (selector == 28) {
			if (!(f->mode & 6))
				return error_out(s, 6, 962);
			if (f->console || ftruncate(f->fd, f->mark))
				return -1;
		}
		if (fsync(f->fd))
			return -1;
		return error_out(s, 6, 0);
	case 31:
		if (pascal(lisa_get32(s + 4), name, 255, 1, 0))
			return -1;
		if (!name[0])
			return error_out(s, 8, 0);
		if (strcmp(name, "-PROFILE"))
			return error_out(s, 8, 1071);
		if (chdir(state_root))
			return -1;
		return error_out(s, 8, 0);
	case 33: return mount_console(s);
	case 34:
		if (pascal(lisa_get32(s + 4), name, 255, 0, 0))
			return -1;
		i = console_index(name);
		return error_out(s, 8, !strcmp(name, "PROFILE") || !strcmp(name, "-PROFILE") ||
			(i >= 0 && console_fd[i] >= 0) ? 1196 : 1061);
	case 50:
		p = argument(s, 14, 4, 1);
		if (!p || !argument(s, 6, 1, 0) || pascal(lisa_get32(s + 10), name, 255, 0, 0) ||
		    (strcmp(name, "SYSTEM.PRINT") && strcmp(name, "-PROFILE-SYSTEM.PRINT")))
			return -1;
		lisa_put32(p, 0xffffffffUL);
		return error_out(s, 18, channel(get16(s + 4)) ? 135 : 136);
	case 51:
		address = lisa_get32(s + 4);
		p = address ? lisa_pointer(address, 40, 0) : 0;
		if (address && !p)
			return -1;
		if (lisa_terminate(p))
			return -1;
		return error_out(s, 8, 0);
	case 53: return process_info(s);
	case 57: return set_priority(s);
	case 58:
		if (s[4] != 1)
			return -1;
		if (lisa_wait_tick())
			return errno == EINTR ? error_out(s, 6, EINTR) : -1;
		return error_out(s, 6, 0);
	case 59:
		p = lisa_pointer(regs[0], 4, 1);
		if (!p)
			return -1;
		lisa_put32(p, getpid());
		return 0;
	case 61:
		if (s[4] > 1 || (s[4] && fence == 0x7fffffff) || (!s[4] && !fence))
			return -1;
		fence += s[4] ? 1 : -1;
		return error_out(s, 6, 0);
	case 70: return data_open(s, 1);
	case 71: return kill_data(s);
	case 72: return data_open(s, 0);
	case 73: return close_data(s);
	case 74: return data_size(s);
	case 75: return data_info(s);
	case 76:
		d = data(get16(s + 6));
		if (!d)
			return error_out(s, 8, 303);
		if (s[4] > 1)
			return -1;
		if (d->ldsn > 0 && lisa_protect(slot_base(d->ldsn), SLOTBYTES,
		    s[4] ? PROT_READ : PROT_READ|PROT_WRITE))
			return -1;
		d->readonly = s[4];
		return error_out(s, 8, 0);
	case 77:
		d = data(get16(s + 4));
		if (!d || d->ldsn <= 0)
			return error_out(s, 6, 303);
		if (data_unbind(d))
			return -1;
		return error_out(s, 6, 0);
	case 78:
		d = data(get16(s + 4));
		if (!d || d->ldsn >= 0)
			return error_out(s, 6, 303);
		ldsn = -d->ldsn;
		if (!slot_free(ldsn))
			return error_out(s, 6, 304);
		if (data_bind(d, ldsn))
			return -1;
		return error_out(s, 6, 0);
	case 82:
		p = argument(s, 4, 2, 1);
		address = lisa_get32(s + 8);
		if (!p || !lisa_pointer(address, 1, 0))
			return -1;
		if (address >= 0xffd60000UL && address < 0xfff60000UL)
			address -= 0xff000000UL;
		for (i = 0; i < 16; ++i)
			if (objects[i].used && objects[i].ldsn > 0 &&
			    address >= slot_base(objects[i].ldsn) &&
			    address - slot_base(objects[i].ldsn) < objects[i].memory) {
				put16(p, i + 1);
				return error_out(s, 12, 0);
			}
		return -1;
	case 90:
		if (pascal(lisa_get32(s + 8), name, 255, 0, 0) || strcmp(name, "SYS_TERMINATE"))
			return -1;
		if (lisa_set_terminate(lisa_get32(s + 4), regs[14]))
			return -1;
		return error_out(s, 12, 0);
	case 98: return make_channel(s);
	case 99: case 102:
		c = channel(get16(s + 4));
		if (!c)
			return error_out(s, 6, 401);
		if (selector == 99)
			memset(c, 0, sizeof(*c));
		else
			c->head = c->count = 0;
		return error_out(s, 6, 0);
	case 100:
		c = channel(get16(s + 8));
		if (!c)
			return error_out(s, 10, 401);
		p = argument(s, 4, 264, 1);
		if (!p)
			return -1;
		memset(p, 0, 264);
		put16(p + 2, c->count);
		put16(p + 4, 1);
		return error_out(s, 10, 0);
	case 101: return wait_event(s);
	case 103: return send_event(s);
	case 105: return get_time(s);
	case 115: return error_out(s, 4, lisa_lock_code(lisa_get32(s)) ? 201 : 0);
	case 121: return open_file(s, 4, 1);
	case 126:
		if (pascal(lisa_get32(s + 8), name, 255, 0, 0) || !lisa_program)
			return -1;
		for (i = 0; lisa_program[i] && i < 255; ++i) {
			int ch;
			ch = lisa_program[i];
			other[i] = ch >= 'a' && ch <= 'z' ? ch - 32 : ch;
		}
		other[i] = 0;
		if (strcmp(logical_name(name), other))
			return -1;
		p = argument(s, 4, 8, 1);
		if (!p)
			return -1;
		memset(p, 0, 8);
		p[0] = 0x6c;
		return error_out(s, 12, 0);
	case 133:
		if (pascal(lisa_get32(s + 4), name, 255, 1, 0) || name[0] ||
		    pascal(lisa_get32(s + 8), name, 255, 0, 0))
			return -1;
		b = binding(name, path);
		if (!b || strncmp(b->name, "SCRAP", 5) || b->data)
			return -1;
		i = open(path, O_WRONLY|O_CREAT|O_EXCL, 0600);
		if (i < 0)
			return errno == EEXIST ? error_out(s, 12, 890) :
				access_error() ? error_out(s, 12, 1130) : -1;
		if (close(i))
			return -1;
		return error_out(s, 12, 0);
	case 135:
		if (pascal(lisa_get32(s + 4), name, 255, 1, 0) || name[0])
			return -1;
		return open_file(s, 4, 0);
	case 143:
		p = argument(s, 4, 12, 1);
		if (!p)
			return -1;
		lisa_put32(p, 68040);
		lisa_put32(p + 4, 0xffffffffUL);
		lisa_put32(p + 8, 0x200000UL);
		return error_out(s, 8, 0);
	case 144:
		if (s[4] > 1)
			return -1;
		p = lisa_pointer(regs[0], 1, 1);
		if (!p)
			return -1;
		*p = 0;
		return 0;
	case 145: case 146:
		p = argument(s, 4, 64, selector == 145);
		if (!p)
			return -1;
		result = pm_transfer(p, selector == 146);
		return result == -1 ? -1 : error_out(s, 8, result);
	case 147:
		p = argument(s, 4, 33, 1);
		if (!p)
			return -1;
		put_string(p, "profile", 32);
		return error_out(s, 8, 0);
	case 148:
		p = argument(s, 4, 50, 1);
		q = argument(s, 8, 64, 0);
		r = argument(s, 12, 4, 1);
		if (!p || !q || !r)
			return -1;
		address = lisa_get32(r);
		if (address)
			return error_out(s, 16, address == 1 ? 799 : 797);
		memset(p, 0, 50);
		p[0] = 1;
		put_string(p + 16, "profile", 32);
		lisa_put32(r, 1);
		return error_out(s, 16, 0);
	case 149:
		q = argument(s, 8, 64, 1);
		r = argument(s, 12, 4, 1);
		if (!q || !r || get16(r + 2))
			return -1;
		q[9] = 0;
		memset(q + 10, 0xff, 50);
		r[0] = 1;
		put16(r + 2, 10);
		return error_out(s, 16, 0);
	case 150:
		p = argument(s, 4, 6, 1);
		if (!p)
			return -1;
		memset(p, 0xff, 6);
		return error_out(s, 8, 0);
	}
	return -1;
}

int
lisa_services_close()
{
	int i, error=0;
	for (i = 0; i < 16; ++i) {
		if (files[i].used && close(files[i].fd) < 0 && !error)
			error=errno;
		if (objects[i].used) {
			if (data_sync(&objects[i])) {
				if(!error)error=errno?errno:EIO;
				fprintf(stderr, "lisa: data persistence failed: %s\n", objects[i].name);
			}
			if (objects[i].fd >= 0 && close(objects[i].fd) < 0 && !error)
				error=errno;
			free(objects[i].saved);
		}
	}
	for (i = 0; i < 2; ++i) {
		if (console_fd[i] >= 0 && close(console_fd[i]) < 0 && !error)
			error=errno;
		console_fd[i] = -1;
		console_no_block[i] = console_auto_lf[i] = 0;
	}
	if (media_fd >= 0 && close(media_fd) < 0 && !error)
		error=errno;
	media_fd = -1;
	memset(files, 0, sizeof(files));
	memset(objects, 0, sizeof(objects));
	memset(channels, 0, sizeof(channels));
	initialized = fence = 0;
	if(error) {errno=error;return -1;}
	return 0;
}
