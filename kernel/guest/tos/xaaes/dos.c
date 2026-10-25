/*
 * dos.c -- GEMDOS and MiNT calls.  dos_tab is the module interface's
 * table, every slot filled; dos_trap is what trap #1 serves, with the
 * calls TOS keeps (Super, the clock) left to it.
 */
#include "xamint.h"

#define	ME	KP(curproc)
#define	MIN_OPEN 6

extern long drvmap(void);
extern int drive_ro(const char *mpath);
extern long dir_change(const char *mpath);
extern long dir_open(DIR *d, const char *mpath, int tos);
extern long dir_read(DIR *d, char *buf, int len, char *st);
extern long dir_rewind(DIR *d);
extern long dir_close(DIR *d);
extern long file_pipe(struct file **rd, struct file **wr);
extern int is_console(struct file *f);
extern void short83(const char *n, char *o);
extern long _cdecl sys_p_exec(short mode, const void *p1, const void *p2, const void *p3);
extern long post_sig(struct kp *k, int sig);
extern void reap_orphans(void);
extern TIMEOUT *_cdecl addtimeout_curproc(long delta, void (*func)(PROC *, long));
extern void _cdecl canceltimeout(TIMEOUT *t);

static char hp[PLEN];

static long
hpath(const char *name)
{
	int r = host_path(name, hp, 1);

	return r == 0 ? 0 : r < 0 ? EPATH : EACCES;
}

/* ---- console ---- */

static long _cdecl
sys_c_conws(const char *s)
{
	return file_write(fd_get(ME, 1), s, strlen(s));
}

static long _cdecl
sys_c_conout(int c)
{
	char b = c;

	return file_write(fd_get(ME, 1), &b, 1);
}

/* Cconin echoes on a terminal the line discipline would echo on */
static long _cdecl
sys_c_conin(void)
{
	struct tty *t = tty_of(fd_get(ME, 0));
	long c = file_getchar(fd_get(ME, 0), RAW);
	char b = c;

	if (t && (t->sg.sg_flags & T_ECHO) && !(t->sg.sg_flags & T_RAW) && b)
		file_write(fd_get(ME, 1), &b, 1);
	return c;
}

static long _cdecl
sys_c_rawcin(void)
{
	return file_getchar(fd_get(ME, 0), RAW);
}

/* Cconrs: a line from handle 0, edited and echoed on handle 1 */
static long _cdecl
sys_c_conrs(char *sbuf)
{
	unsigned char *buf = (unsigned char *)sbuf;
	struct file *in = fd_get(ME, 0), *out = fd_get(ME, 1);
	int n = 0;
	long c;
	char b;

	while (n < buf[0]) {
		c = file_getchar(in, RAW);
		if (c < 0)
			break;
		b = c;
		if (b == '\r' || b == '\n')
			break;
		if (b == 8 || b == 127) {
			if (n > 0) {
				n--;
				file_write(out, "\b \b", 3);
			}
			continue;
		}
		if (b == 3) {
			post_sig(ME, SIGINT);
			break;
		}
		buf[2 + n++] = b;
		file_write(out, &b, 1);
	}
	file_write(out, "\r", 1);
	buf[1] = n;
	return n;
}

/*
 * BIOS console calls of a user process whose handles 0 or 1 are not
 * the console go to them; 0x80000000: the call is TOS's.
 */
long
xa_bios(short *a)
{
	struct file *in = fd_get(ME, 0), *out = fd_get(ME, 1);
	char b;

	if (ME->kthread || curproc == rootproc || (a[1] != 2 && a[1] != 5))
		return 0x80000000L;
	switch (a[0]) {
	case 1:
		if (a[1] != 2 || !in || is_console(in))
			break;
		return file_select(in, 0) ? -1 : 0;
	case 2:
		if (a[1] != 2 || !in || is_console(in))
			break;
		return file_getchar(in, RAW);
	case 3:
		if (!out || is_console(out))
			break;
		b = a[2];
		file_write(out, &b, 1);
		return 0;
	case 8:
		if (!out || is_console(out))
			break;
		return -1;
	}
	return 0x80000000L;
}

static long _cdecl
sys_c_conis(void)
{
	return file_select(fd_get(ME, 0), 0) ? -1 : 0;
}

static long _cdecl
sys_c_conos(void)
{
	return -1;
}

static long _cdecl
sys_c_rawio(int c)
{
	if ((c & 0xff) == 0xff)
		return sys_c_conis() ? sys_c_conin() : 0;
	return sys_c_conout(c);
}

/* ---- drives and directories ---- */

static long _cdecl
sys_d_setdrv(int d)
{
	if (d >= 0 && d < 26)
		ME->p.p_cwd->curdrv = d;
	return drvmap();
}

static long _cdecl
sys_d_getdrv(void)
{
	return ME->p.p_cwd->curdrv;
}

static long _cdecl
sys_d_setpath(const char *path)
{
	return dir_change(path);
}

static long _cdecl
sys_d_getcwd(char *path, int drv, int size)
{
	char *d;

	drv = drv ? drv - 1 : ME->p.p_cwd->curdrv;
	if (drv < 0 || drv >= 26)
		return EDRIVE;
	d = ME->dirs[drv];
	if ((long)strlen(d ? d : "") + 2 > size)
		return ERANGE;
	path[0] = '\\';
	strcpy(path + 1, d ? d : "");
	if (!path[1])
		path[0] = 0;
	return 0;
}

static long _cdecl
sys_d_getpath(char *path, int drv)
{
	return sys_d_getcwd(path, drv, 128);
}

static long _cdecl
sys_d_create(const char *path)
{
	long r;

	if ((r = hpath(path)) != 0)
		return r;
	if (drive_ro(path))
		return EACCES;
	return merr(hc(H_MKDIR, 2, (long)hp, 0777L));
}

static long _cdecl
sys_d_delete(const char *path)
{
	long r;

	if ((r = hpath(path)) != 0)
		return r;
	if (drive_ro(path))
		return EACCES;
	return merr(hc(H_RMDIR, 1, (long)hp));
}

static long _cdecl
sys_d_free(long *buf, int d)
{
	char p[4];
	unsigned long v[40];
	long r;

	p[0] = d ? 'A' + d - 1 : 'A' + ME->p.p_cwd->curdrv;
	p[1] = ':';
	p[2] = '\\';
	p[3] = 0;
	if ((r = hpath(p)) != 0)
		return r;
	if ((r = hc(H_STATVFS, 2, (long)hp, (long)v)) < 0)
		return merr(r);
	buf[0] = v[4];
	buf[1] = v[2];
	buf[2] = v[1] ? v[1] : 512;
	buf[3] = 1;
	return 0;
}

static long _cdecl
sys_d_pathconf(const char *name, int which)
{
	(void)name;
	switch (which) {
	case DP_INQUIRE: return DP_XATTRFIELDS;
	case DP_IOPEN: return NOFILE;
	case DP_MAXLINKS: return 32767;
	case DP_PATHMAX: return PLEN;
	case DP_NAMEMAX: return 255;
	case DP_ATOMIC: return 512;
	case DP_TRUNC: return DP_NOTRUNC;
	case DP_CASE: return DP_CASESENS;
	case DP_MODEATTR: return DP_FT_DIR | DP_FT_REG | DP_FT_LNK | DP_FT_CHR | DP_MODEBITS | 0x11;
	case DP_XATTRFIELDS: return DP_INDEX | DP_DEV | DP_NLINK | DP_UID | DP_GID | DP_BLKSIZE |
		DP_SIZE | DP_NBLOCKS | DP_ATIME | DP_CTIME | DP_MTIME;
	}
	return ENOSYS;
}

static long _cdecl
sys_d_readlabel(const char *path, char *label, int maxlen)
{
	(void)path;
	if (maxlen < 1)
		return ERANGE;
	label[0] = 0;
	return 0;
}

static long _cdecl
sys_d_lock(int mode, int drv)
{
	(void)mode;
	(void)drv;
	return 0;
}

static long _cdecl
sys_d_cntl(int cmd, const char *name, long arg)
{
	(void)name;
	(void)arg;
	xlog("Dcntl 0x%x %s", cmd, name);
	return ENOSYS;
}

static long _cdecl
sys_d_opendir(const char *path, int flags)
{
	DIR *d = kmalloc_o(sizeof *d, ME);
	long r;

	if (!d)
		return ENOMEM;
	r = dir_open(d, path, flags & 1);
	if (r) {
		kfree_any(d);
		return r;
	}
	return (long)d;
}

static long _cdecl
sys_d_readdir(int len, long handle, char *buf)
{
	return dir_read((DIR *)handle, buf, len, 0);
}

static long _cdecl
sys_d_xreaddir(int len, long handle, char *buf, XATTR *x, long *xret)
{
	char st[HST_SIZE];
	long r = dir_read((DIR *)handle, buf, len, st);

	if (r == 0) {
		st_xattr(st, x);
		*xret = 0;
	}
	return r;
}

static long _cdecl
sys_d_rewind(long handle)
{
	return dir_rewind((DIR *)handle);
}

static long _cdecl
sys_d_closedir(long handle)
{
	long r = dir_close((DIR *)handle);

	kfree_any((void *)handle);
	return r;
}

/* ---- files ---- */

static long _cdecl
sys_f_open(const char *name, short mode)
{
	struct file *f;
	long r = file_open(name, mode, 0666, &f);

	if (r)
		return r;
	r = fd_alloc(ME, f, MIN_OPEN);
	if (r < 0)
		file_close(f);
	return r;
}

static long _cdecl
sys_f_create(const char *name, short attrib)
{
	struct file *f;
	long r;

	if (attrib & FA_LABEL)
		return EACCES;
	r = file_open(name, O_RDWR | O_CREAT | O_TRUNC, (attrib & FA_RDONLY) ? 0444 : 0666, &f);
	if (r)
		return r;
	r = fd_alloc(ME, f, MIN_OPEN);
	if (r < 0)
		file_close(f);
	return r;
}

static long _cdecl
sys_f_close(short fd)
{
	struct file *f = fd_get(ME, fd);

	if (!f)
		return EBADF;
	ME->p.p_fd->ofiles[fd] = 0;
	return file_close(f);
}

static long _cdecl
sys_f_read(short fd, long count, void *buf)
{
	return file_read(fd_get(ME, fd), buf, count);
}

static long _cdecl
sys_f_write(short fd, long count, const void *buf)
{
	return file_write(fd_get(ME, fd), buf, count);
}

static long _cdecl
sys_f_seek(long place, short fd, short how)
{
	struct file *f = fd_get(ME, fd);

	if (!f)
		return EBADF;
	return (*f->dev->lseek)(f, place, how);
}

static long _cdecl
sys_f_delete(const char *name)
{
	long r;

	if ((r = hpath(name)) != 0)
		return r;
	if (drive_ro(name))
		return EACCES;
	return merr(hc(H_UNLINK, 1, (long)hp));
}

static long _cdecl
sys_f_rename(short junk, const char *old, const char *new)
{
	char h2[PLEN];
	long r;

	(void)junk;
	if ((r = hpath(new)) != 0)
		return r;
	strcpy(h2, hp);
	if ((r = hpath(old)) != 0)
		return r;
	if (drive_ro(old) || drive_ro(new))
		return EACCES;
	return merr(hc(H_RENAME, 2, (long)hp, (long)h2));
}

static long _cdecl
sys_f_attrib(const char *name, int rwflag, int attr)
{
	char st[HST_SIZE];
	XATTR x;
	long r;

	if ((r = hpath(name)) != 0)
		return r;
	if ((r = hstat(hp, st, 0)) < 0)
		return merr(r) == EPATH ? EFILNF : merr(r);
	st_xattr(st, &x);
	if (rwflag && !drive_ro(name)) {
		long m = HL(st, HST_MODE) & 07777;

		m = (attr & FA_RDONLY) ? m & ~0222 : m | 0200;
		hc(H_CHMOD, 2, (long)hp, m);
		return attr;
	}
	return x.attr;
}

static long _cdecl
sys_f_datime(ushort *t, short fd, short wflag)
{
	struct file *f = fd_get(ME, fd);

	if (!f)
		return EBADF;
	return (*f->dev->datime)(f, t, wflag);
}

static long _cdecl
sys_f_dup(short fd)
{
	struct file *f = fd_get(ME, fd);
	long r;

	if (!f)
		return EBADF;
	r = fd_alloc(ME, f, MIN_OPEN);
	if (r >= 0)
		f->links++;
	return r;
}

static long _cdecl
sys_f_force(short newh, short oldh)
{
	struct file *f = fd_get(ME, oldh), *o;

	if (!f || newh < MIN_HANDLE || newh >= NOFILE)
		return EBADF;
	o = ME->p.p_fd->ofiles[newh];
	f->links++;
	ME->p.p_fd->ofiles[newh] = f;
	if (o)
		file_close(o);
	return 0;
}

static long _cdecl
sys_f_pipe(short fds[2])
{
	struct file *r, *w;
	long a, b;

	if (file_pipe(&r, &w))
		return ENOMEM;
	a = fd_alloc(ME, r, MIN_OPEN);
	b = a < 0 ? a : fd_alloc(ME, w, MIN_OPEN);
	if (a < 0 || b < 0) {
		if (a >= 0)
			ME->p.p_fd->ofiles[a] = 0;
		file_close(r);
		file_close(w);
		return EMFILE;
	}
	fds[0] = a;
	fds[1] = b;
	return 0;
}

static long _cdecl
sys_f_cntl(short fd, long arg, short cmd)
{
	struct file *f = fd_get(ME, fd);

	if (!f)
		return EBADF;
	switch (cmd) {
	case F_DUPFD: {
		long r = fd_alloc(ME, f, arg);

		if (r >= 0)
			f->links++;
		return r;
	}
	case F_GETFD:
		return ME->p.p_fd->ofileflags[fd] & FD_CLOEXEC;
	case F_SETFD:
		ME->p.p_fd->ofileflags[fd] = arg & FD_CLOEXEC;
		return 0;
	case F_GETFL:
		return f->flags;
	case F_SETFL:
		f->flags = (f->flags & O_RWMODE) | (arg & ~O_RWMODE);
		return 0;
	case F_GETLK: case F_SETLK: case F_SETLKW:
		return 0;
	}
	return (*f->dev->ioctl)(f, cmd, (void *)arg);
}

static long _cdecl
sys_f_instat(int fh)
{
	struct file *f = fd_get(ME, fh);
	long n = 0;

	if (!f)
		return EBADF;
	if ((*f->dev->ioctl)(f, FIONREAD, &n) < 0)
		return file_select(f, 0) ? 1 : 0;
	return n;
}

static long _cdecl
sys_f_outstat(int fh)
{
	struct file *f = fd_get(ME, fh);
	long n = 1;

	if (!f)
		return EBADF;
	(*f->dev->ioctl)(f, FIONWRITE, &n);
	return n;
}

static long _cdecl
sys_f_getchar(int fh, int mode)
{
	return file_getchar(fd_get(ME, fh), mode);
}

static long _cdecl
sys_f_putchar(int fh, long c, int mode)
{
	char b = c;

	(void)mode;
	return file_write(fd_get(ME, fh), &b, 1);
}

static long _cdecl
sys_f_xattr(int flag, const char *name, XATTR *x)
{
	char st[HST_SIZE];
	long r;

	if ((r = host_path(name, hp, 1)) == 1) {
		bzero(x, sizeof *x);
		x->mode = S_IFCHR | 0666;
		x->nlink = 1;
		return 0;
	}
	if (r)
		return EPATH;
	if ((r = hstat(hp, st, flag)) < 0)
		return merr(r) == EPATH ? EFILNF : merr(r);
	st_xattr(st, x);
	return 0;
}

static long _cdecl
sys_f_stat64(int flag, const char *name, STAT *s)
{
	char st[HST_SIZE];
	long r;

	if ((r = hpath(name)) != 0)
		return r;
	if ((r = hstat(hp, st, flag)) < 0)
		return merr(r) == EPATH ? EFILNF : merr(r);
	st_stat64(st, s);
	return 0;
}

static long _cdecl
sys_f_fstat(short fd, STAT *s)
{
	struct file *f = fd_get(ME, fd);

	if (!f)
		return EBADF;
	return (*f->dev->ioctl)(f, FSTAT64, s);
}

static long _cdecl
sys_f_link(const char *old, const char *new)
{
	char h2[PLEN];
	long r;

	if ((r = hpath(new)) != 0)
		return r;
	strcpy(h2, hp);
	if ((r = hpath(old)) != 0)
		return r;
	return merr(hc(H_LINK, 2, (long)hp, (long)h2));
}

static long _cdecl
sys_f_symlink(const char *old, const char *new)
{
	long r;

	if ((r = hpath(new)) != 0)
		return r;
	return merr(hc(H_SYMLINK, 2, (long)old, (long)hp));
}

static long _cdecl
sys_f_readlink(int buflen, char *buf, const char *linkfile)
{
	long r;

	if ((r = hpath(linkfile)) != 0)
		return r;
	r = hc(H_READLINK, 3, (long)hp, (long)buf, (long)buflen - 1);
	if (r < 0)
		return merr(r);
	buf[r] = 0;
	return 0;
}

static long _cdecl
sys_f_chmod(const char *name, unsigned short mode)
{
	long r;

	if ((r = hpath(name)) != 0)
		return r;
	return merr(hc(H_CHMOD, 2, (long)hp, (long)(mode & 07777)));
}

static long _cdecl
sys_f_chown(const char *name, int uid, int gid)
{
	(void)name;
	(void)uid;
	(void)gid;
	return 0;
}

static long _cdecl
sys_p_umask(ushort mode)
{
	long o = ME->p.p_cwd->cmask;

	ME->p.p_cwd->cmask = mode & 0777;
	return o;
}

long _cdecl
sys_f_select(unsigned short timeout, long *rfdp, long *wfdp, long *xfdp)
{
	struct kp *k = ME;
	long r = rfdp ? *rfdp : 0, w = wfdp ? *wfdp : 0, rr, ww;
	long end = timeout ? now_ms() + timeout : 0;
	int i, n;

	if (xfdp)
		*xfdp = 0;
	for (;;) {
		rr = ww = 0;
		n = 0;
		for (i = 0; i < 32; i++) {
			if (((r >> i) & 1) && file_select(fd_get(k, i), 0)) {
				rr |= 1L << i;
				n++;
			}
			if (((w >> i) & 1) && file_select(fd_get(k, i), 1)) {
				ww |= 1L << i;
				n++;
			}
		}
		if (n || (end && now_ms() >= end)) {
			if (rfdp)
				*rfdp = rr;
			if (wfdp)
				*wfdp = ww;
			return n;
		}
		k->sel_r = &r;
		k->sel_w = &w;
		sel_wait(end ? end - now_ms() : 0);
		k->sel_r = k->sel_w = 0;
		if (curproc->sigpending & ~curproc->p_sigmask & ~(1UL << SIGCHLD)) {
			if (rfdp)
				*rfdp = 0;
			if (wfdp)
				*wfdp = 0;
			return EINTR;
		}
	}
}

/* ---- Fsfirst/Fsnext ---- */

struct srch {
	long	magic;
	DIR	d;
	char	pat[14];
	short	attr;
};
#define	SMAGIC	0x5352434cL

static int
up83(int c)
{
	return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static int
wild(const char *p, const char *s)
{
	for (; *p; p++, s++) {
		if (*p == '*') {
			while (*s && *s != '.')
				s++;
			while (*p == '*')
				p++;
			if (!*p)
				return !*s;
			p--;
			s--;
			continue;
		}
		if (*p == '?') {
			if (*s && *s != '.')
				continue;
			s--;
			continue;
		}
		if (*p == '.' && !*s) {
			s--;
			continue;
		}
		if (up83(*p) != up83(*s))
			return 0;
	}
	return !*s;
}

static long
snext(DTABUF *dta, struct srch *s)
{
	char name[64], st[HST_SIZE];
	XATTR x;

	for (;;) {
		if (dir_read(&s->d, name, sizeof name, st)) {
			dir_close(&s->d);
			kfree_any(s);
			dta->magic = 0;
			return ENMFILES;
		}
		if (!wild(s->pat, name))
			continue;
		st_xattr(st, &x);
		if ((x.attr & FA_DIR) && !(s->attr & FA_DIR))
			continue;
		dta->dta_attrib = x.attr;
		dta->dta_time = x.mtime;
		dta->dta_date = x.mdate;
		dta->dta_size = x.size;
		strncpy(dta->dta_name, name, sizeof dta->dta_name - 1);
		return 0;
	}
}

static long _cdecl
sys_f_sfirst(const char *path, int attr)
{
	DTABUF *dta = (DTABUF *)ME->dta;
	struct srch *s;
	char dir[PLEN], *p;
	long r;

	strncpy(dir, path, sizeof dir - 1);
	dir[sizeof dir - 1] = 0;
	p = strrchr(dir, '\\');
	if (!p)
		p = strrchr(dir, '/');
	if (!p)
		p = dir[0] && dir[1] == ':' ? dir + 1 : 0;
	if (attr == FA_LABEL) {
		strcpy(dta->dta_name, "ASHNAZAG");
		dta->dta_attrib = FA_LABEL;
		return 0;
	}
	s = kmalloc_o(sizeof *s, ME);
	if (!s)
		return ENOMEM;
	s->magic = SMAGIC;
	s->attr = attr;
	strncpy(s->pat, p ? p + 1 : dir, sizeof s->pat - 1);
	s->pat[sizeof s->pat - 1] = 0;
	if (p)
		p[1] = 0;
	else
		dir[0] = 0;
	r = dir_open(&s->d, dir[0] ? dir : ".", 1);
	if (r) {
		kfree_any(s);
		return r == EPATH ? EPTHNF : r;
	}
	dta->magic = (long)s;
	r = snext(dta, s);
	return r == ENMFILES ? EFILNF : r;
}

static long _cdecl
sys_f_snext(void)
{
	DTABUF *dta = (DTABUF *)ME->dta;
	struct srch *s = (struct srch *)dta->magic;

	if (!s || s->magic != SMAGIC)
		return ENMFILES;
	return snext(dta, s);
}

static long _cdecl
sys_f_setdta(DTABUF *dta)
{
	ME->dta = (char *)dta;
	return 0;
}

static DTABUF *_cdecl
sys_f_getdta(void)
{
	return (DTABUF *)ME->dta;
}

/* ---- memory ---- */

static long _cdecl
sys_m_xalloc(long size, short mode)
{
	(void)mode;
	if (size == -1)
		return mem_largest() > KEEP ? mem_largest() - KEEP : 0;
	return (long)kmalloc_o(size, ME);
}

static long _cdecl
sys_m_alloc(long size)
{
	return sys_m_xalloc(size, 3);
}

static long _cdecl
sys_m_free(void *block)
{
	kfree_any(block);
	return 0;
}

static long _cdecl
sys_m_shrink(short dummy, void *block, long size)
{
	(void)dummy;
	return mem_shrink(block, size);
}

/* ---- processes ---- */

long _cdecl
sys_p_term(short code)
{
	proc_exit(ME, code);
	return 0;
}

static long _cdecl
sys_pterm0(void)
{
	return sys_p_term(0);
}

static long _cdecl
sys_p_termres(long save, short code)
{
	if (ME->base) {
		mem_shrink(ME->base, save);
		mem_give(ME->base, 0);
	}
	return sys_p_term(code);
}

static long _cdecl sys_p_getpid(void) { return curproc->pid; }
static long _cdecl sys_p_getppid(void) { return curproc->ppid; }
static long _cdecl sys_p_getpgrp(void) { return curproc->pgrp; }
static long _cdecl sys_p_zero(void) { return 0; }

static long _cdecl
sys_p_setpgrp(int pid, int newgrp)
{
	struct proc *p = pid ? pid2proc(pid) : curproc;

	if (!p)
		return ESRCH;
	if (newgrp < 0)
		return p->pgrp;
	p->pgrp = newgrp ? newgrp : p->pid;
	return p->pgrp;
}

static long _cdecl
sys_p_setid(unsigned short id)
{
	return id;
}

static long _cdecl
sys_p_domain(int arg)
{
	long o = curproc->domain;

	if (arg >= 0)
		curproc->domain = arg ? DOM_MINT : DOM_TOS;
	return o;
}

static long _cdecl
sys_p_usrval(long arg)
{
	long o = curproc->usrdata;

	if (arg != -1)
		curproc->usrdata = arg;
	return o;
}

static long _cdecl
sys_p_kill(short pid, short sig)
{
	struct kp *k;
	long r = ESRCH;

	if (pid > 0)
		return ikill(pid, sig);
	if (pid == 0)
		pid = -curproc->pgrp;
	for (k = allprocs; k; k = k->all)
		if (k->p.pgrp == -pid && k->p.pid && !k->zombie) {
			post_sig(k, sig);
			r = 0;
		}
	return r;
}

static long _cdecl
sys_p_sigaction(short sig, const struct sigaction *act, struct sigaction *oact)
{
	struct sigaction *s;

	if (sig < 1 || sig >= NSIG)
		return EBADARG;
	s = &curproc->p_sigacts->sigact[sig];
	if (oact)
		*oact = *s;
	if (act && sig != SIGKILL && sig != SIGSTOP) {
		*s = *act;
		if (s->sa_handler == SIG_IGN)
			curproc->sigpending &= ~(1UL << sig);
	}
	return 0;
}

static long _cdecl
sys_p_signal(short sig, long handler)
{
	struct sigaction a, o;
	long r;

	bzero(&a, sizeof a);
	a.sa_handler = handler;
	r = sys_p_sigaction(sig, &a, &o);
	return r ? r : (long)o.sa_handler;
}

static long _cdecl
sys_p_sigblock(ulong mask)
{
	ulong o = curproc->p_sigmask;

	curproc->p_sigmask |= mask & ~((1UL << SIGKILL) | (1UL << SIGSTOP) | 1);
	return o;
}

static long _cdecl
sys_p_sigsetmask(ulong mask)
{
	ulong o = curproc->p_sigmask;

	curproc->p_sigmask = mask & ~((1UL << SIGKILL) | (1UL << SIGSTOP) | 1);
	return o;
}

static long _cdecl
sys_p_sigpending(void)
{
	return curproc->sigpending;
}

static long _cdecl
sys_p_sigpause(ulong mask)
{
	ulong o = curproc->p_sigmask;

	curproc->p_sigmask = mask;
	while (!(curproc->sigpending & ~curproc->p_sigmask))
		sleep(WAIT_Q, (long)&curproc->sigpending);
	curproc->p_sigmask = o;
	return EINTR;
}

static long _cdecl
sys_p_pause(void)
{
	return sys_p_sigpause(curproc->p_sigmask);
}

/* a finished child: pid << 16 | status; 0 if none yet and nohang */
static long _cdecl
sys_p_waitpid(short pid, short nohang, long *rusage)
{
	struct kp *k;
	int any;
	long r;

	if (rusage) {
		rusage[0] = rusage[1] = 0;
	}
	for (;;) {
		any = 0;
		for (k = allprocs; k; k = k->all) {
			if (k->p.ppid != curproc->pid || (k->kthread && !k->hostfd) || k == ME)
				continue;
			if (pid > 0 && k->p.pid != pid)
				continue;
			if (pid < -1 && k->p.pgrp != -pid)
				continue;
			if (pid == 0 && k->p.pgrp != curproc->pgrp)
				continue;
			any = 1;
			if (k->zombie) {
				r = (long)k->p.pid << 16 | (k->exitcode & 0xffff);
				proc_reap(k);
				return r;
			}
		}
		if (!any)
			return ECHILD;
		if (nohang & 1)
			return 0;
		sleep(WAIT_Q, (long)curproc);
		if (curproc->sigpending & ~curproc->p_sigmask & ~(1UL << SIGCHLD))
			return EINTR;
	}
}

static long _cdecl
sys_p_wait3(short nohang, long *rusage)
{
	return sys_p_waitpid(-1, nohang, rusage);
}

static long _cdecl
sys_p_wait(void)
{
	return sys_p_waitpid(-1, 0, 0);
}

static long _cdecl
sys_p_renice(short pid, short inc)
{
	(void)pid;
	(void)inc;
	return 0;
}

static long _cdecl
sys_p_nice(short delta)
{
	(void)delta;
	return 0;
}

static long _cdecl
sys_s_yield(void)
{
	return xa_yield();
}

/* Psemaphore: named locks held by one process */
struct sema {
	long	id;
	short	owner;
	struct sema *next;
};
static struct sema *semas;

static long _cdecl
sys_p_semaphore(int mode, long id, long timeout)
{
	struct sema *s, **sp;
	long end = timeout > 0 ? now_ms() + timeout : 0;

	for (sp = &semas; *sp && (*sp)->id != id; sp = &(*sp)->next)
		;
	s = *sp;
	switch (mode) {
	case 0:
		if (s)
			return EACCES;
		s = kmalloc_o(sizeof *s, 0);
		if (!s)
			return ENOMEM;
		s->id = id;
		s->owner = curproc->pid;
		s->next = semas;
		semas = s;
		return 0;
	case 1:
		if (!s)
			return EBADARG;
		if (s->owner != curproc->pid && s->owner)
			return EACCES;
		*sp = s->next;
		kfree_any(s);
		wake(WAIT_Q, (long)&semas);
		return 0;
	case 2:
		for (;;) {
			for (s = semas; s && s->id != id; s = s->next)
				;
			if (!s)
				return EBADARG;
			if (s->owner == curproc->pid)
				return ELOCKED;
			if (!s->owner || !pid2proc(s->owner)) {
				s->owner = curproc->pid;
				return 0;
			}
			if (timeout == 0 || (end && now_ms() >= end))
				return EACCES;
			KP(curproc)->wake_at = end;
			sleep(WAIT_Q, (long)&semas);
			KP(curproc)->wake_at = 0;
		}
	case 3:
		if (!s)
			return EBADARG;
		if (s->owner != curproc->pid)
			return EACCES;
		s->owner = 0;
		wake(WAIT_Q, (long)&semas);
		return 0;
	}
	return EBADARG;
}

static long _cdecl
sys_p_msg(int mode, long mbid, char *ptr)
{
	(void)mode;
	(void)mbid;
	(void)ptr;
	return ENOSYS;
}

static void
alarm_ring(PROC *p, long arg)
{
	(void)arg;
	KP(p)->alarm = 0;
	post_sig(KP(p), SIGALRM);
}

/* SIGALRM in ms milliseconds, 0 none, below 0 asks; the time left of the last */
static long _cdecl
sys_t_malarm(long ms)
{
	struct kp *k = ME;
	long left = 0;

	if (k->alarm && (left = k->alarm->when - now_ms()) <= 0)
		left = 1;
	if (ms < 0)
		return left;
	if (k->alarm) {
		canceltimeout(k->alarm);
		k->alarm = 0;
	}
	if (ms > 0)
		k->alarm = addtimeout_curproc(ms, alarm_ring);
	return left;
}

static long _cdecl
sys_t_alarm(long x)
{
	long r = sys_t_malarm(x < 0 ? -1 : x * 1000);

	return (r + 999) / 1000;
}

static long _cdecl
sys_s_ysconf(int which)
{
	switch (which) {
	case -1: return 4;
	case 0: return 64;
	case 1: return 126;
	case 2: return 32767;
	case 3: return NOFILE;
	case 4: return 0;
	}
	return ENOSYS;
}

static long _cdecl
sys_t_gettimeofday(struct timeval *tv, struct timezone *tz)
{
	if (tv) {
		tv->tv_sec = hc(H_TIME, 0);
		tv->tv_usec = (now_ms() % 1000) * 1000;
	}
	if (tz) {
		tz->tz_minuteswest = -(*(long *)0xfa0064) / 60;
		tz->tz_dsttime = 0;
	}
	return 0;
}

static long _cdecl
sys_s_uptime(ulong *cur, ulong load[3])
{
	if (cur)
		*cur = now_ms() / 1000;
	if (load)
		load[0] = load[1] = load[2] = 0;
	return 0;
}

static long _cdecl
sys_s_alert(const char *msg)
{
	xlog("alert: %s", msg);
	return 0;
}

static long _cdecl
sys_s_hutdown(long restart)
{
	xlog("Shutdown(%ld): the environment ends", restart);
	hc(H_EXIT, 1, 0L);
	return 0;
}

static long _cdecl
sys_s_version(void)
{
	return 0x4000;
}

static long _cdecl
sys_s_ync(void)
{
	return 0;
}

static long _cdecl
sys_p_sigreturn(void)
{
	return 0;
}

struct mpoll {
	long	fd;
	ushort	events, revents;
};

#define	P_IN	0x0043			/* POLLIN, POLLPRI, POLLRDNORM */
#define	P_OUT	0x0104			/* POLLOUT, POLLWRNORM */

/* Fpoll, on the select waiters' polling; handles from 32 on are polled every 20 ms */
static long _cdecl
sys_f_poll(struct mpoll *fds, ulong nfds, ulong timeout)
{
	struct kp *k = ME;
	long end = now_ms() + timeout, r, w, left;
	struct file *f;
	ulong i;
	int n, far;

	for (;;) {
		n = far = 0;
		r = w = 0;
		for (i = 0; i < nfds; i++) {
			fds[i].revents = 0;
			if (fds[i].fd < 0)
				continue;
			if (!(f = fd_get(k, fds[i].fd)))
				fds[i].revents = 0x20;
			else {
				if ((fds[i].events & P_IN) && file_select(f, 0))
					fds[i].revents |= fds[i].events & P_IN;
				if ((fds[i].events & P_OUT) && file_select(f, 1))
					fds[i].revents |= fds[i].events & P_OUT;
				if (fds[i].fd >= 32)
					far = 1;
				else {
					if (fds[i].events & P_IN)
						r |= 1L << fds[i].fd;
					if (fds[i].events & P_OUT)
						w |= 1L << fds[i].fd;
				}
			}
			if (fds[i].revents)
				n++;
		}
		if (n || timeout == 0 || (timeout != ~0UL && now_ms() >= end))
			return n;
		left = timeout == ~0UL ? 0 : end - now_ms();
		if (far && (left == 0 || left > 20))
			left = 20;
		k->sel_r = &r;
		k->sel_w = &w;
		sel_wait(left);
		k->sel_r = k->sel_w = 0;
		if (curproc->sigpending & ~curproc->p_sigmask & ~(1UL << SIGCHLD))
			return EINTR;
	}
}

/* Ssystem: identity, cookies, memory peeks */
long _cdecl
sys_s_system(int mode, ulong arg1, ulong arg2)
{
	long v;

	switch (mode) {
	case -1: return 0;
	case S_OSNAME: return 0x4d694e54L;
	case S_OSXNAME: return 0x46726565L;
	case S_OSVERSION: return 0x01130000L;
	case S_OSFEATURES: return 0;
	case S_GETCOOKIE:
		if (cookie_get(arg1, &v) < 0)
			return EERROR;
		if (arg2)
			*(long *)arg2 = v;
		return arg2 ? 0 : v;
	case S_SETCOOKIE: return cookie_set(arg1, arg2);
	case S_DELCOOKIE: return 0;
	case S_GETLVAL: return *(long *)arg1;
	case S_GETWVAL: return *(unsigned short *)arg1;
	case S_GETBVAL: return *(unsigned char *)arg1;
	case S_SETLVAL: *(long *)arg1 = arg2; return 0;
	case S_SETWVAL: *(short *)arg1 = arg2; return 0;
	case S_SETBVAL: *(char *)arg1 = arg2; return 0;
	case S_SECLEVEL: return 0;
	case S_TSLICE: return 2;
	case S_FASTLOAD: return 0;
	case S_INITIALTPA: return 0;
	case S_CLOCKUTC: return 0;
	case S_FLUSHCACHE: case S_CTRLCACHE: return 0;
	case S_GETBOOTLOG:
		if (arg1 && arg2 > 20) {
			strcpy((char *)arg1, "C:\\MINT\\BOOT.LOG");
			return 0;
		}
		return EINVAL;
	case S_SETEXC: return 0;
	}
	xlog("Ssystem %d", mode);
	return ENOSYS;
}

/* from a module: trap #1's own entry serves programs */
static long _cdecl
sys_p_vfork(void)
{
	return ENOSYS;
}

/* ---- the tables ---- */

dos_vecs dos_tab = {
	.p_pterm0 = sys_pterm0,
	.p_c_conin = sys_c_conin,
	.p_c_conout = sys_c_conout,
	.p_c_rawio = sys_c_rawio,
	.p_c_rawcin = sys_c_rawcin,
	.p_c_necin = sys_c_rawcin,
	.p_c_conrs = sys_c_conrs,
	.p_c_conws = sys_c_conws,
	.p_c_conis = sys_c_conis,
	.p_c_conos = sys_c_conos,
	.p_d_setdrv = sys_d_setdrv,
	.p_d_getdrv = sys_d_getdrv,
	.p_f_setdta = sys_f_setdta,
	.p_f_getdta = sys_f_getdta,
	.p_s_version = sys_s_version,
	.p_p_termres = sys_p_termres,
	.p_d_free = sys_d_free,
	.p_d_create = sys_d_create,
	.p_d_delete = sys_d_delete,
	.p_d_setpath = sys_d_setpath,
	.p_f_create = sys_f_create,
	.p_f_open = sys_f_open,
	.p_f_close = sys_f_close,
	.p_f_read = sys_f_read,
	.p_f_write = sys_f_write,
	.p_f_delete = sys_f_delete,
	.p_f_seek = sys_f_seek,
	.p_f_attrib = sys_f_attrib,
	.p_m_xalloc = sys_m_xalloc,
	.p_f_dup = sys_f_dup,
	.p_f_force = sys_f_force,
	.p_d_getpath = sys_d_getpath,
	.p_m_alloc = sys_m_alloc,
	.p_m_free = sys_m_free,
	.p_m_shrink = sys_m_shrink,
	.p_p_exec = sys_p_exec,
	.p_p_term = sys_p_term,
	.p_f_sfirst = sys_f_sfirst,
	.p_f_snext = sys_f_snext,
	.p_f_rename = sys_f_rename,
	.p_f_datime = sys_f_datime,
	.p_s_yield = sys_s_yield,
	.p_f_pipe = sys_f_pipe,
	.p_f_cntl = sys_f_cntl,
	.p_f_instat = sys_f_instat,
	.p_f_outstat = sys_f_outstat,
	.p_f_getchar = sys_f_getchar,
	.p_f_putchar = sys_f_putchar,
	.p_p_wait = sys_p_wait,
	.p_p_nice = sys_p_nice,
	.p_p_getpid = sys_p_getpid,
	.p_p_getppid = sys_p_getppid,
	.p_p_getpgrp = sys_p_getpgrp,
	.p_p_setpgrp = sys_p_setpgrp,
	.p_p_getuid = sys_p_zero,
	.p_p_setuid = sys_p_setid,
	.p_p_kill = sys_p_kill,
	.p_p_signal = sys_p_signal,
	.p_p_vfork = sys_p_vfork,
	.p_p_getgid = sys_p_zero,
	.p_p_setgid = sys_p_setid,
	.p_p_sigblock = sys_p_sigblock,
	.p_p_sigsetmask = sys_p_sigsetmask,
	.p_p_usrval = sys_p_usrval,
	.p_p_domain = sys_p_domain,
	.p_p_sigreturn = sys_p_sigreturn,
	.p_p_fork = sys_p_vfork,
	.p_p_wait3 = sys_p_wait3,
	.p_f_select = sys_f_select,
	.p_t_alarm = sys_t_alarm,
	.p_p_pause = sys_p_pause,
	.p_s_ysconf = sys_s_ysconf,
	.p_p_sigpending = sys_p_sigpending,
	.p_d_pathconf = sys_d_pathconf,
	.p_p_msg = sys_p_msg,
	.p_p_renice = sys_p_renice,
	.p_d_opendir = sys_d_opendir,
	.p_d_readdir = sys_d_readdir,
	.p_d_rewind = sys_d_rewind,
	.p_d_closedir = sys_d_closedir,
	.p_f_xattr = sys_f_xattr,
	.p_f_link = sys_f_link,
	.p_f_symlink = sys_f_symlink,
	.p_f_readlink = sys_f_readlink,
	.p_d_cntl = sys_d_cntl,
	.p_f_chown = sys_f_chown,
	.p_f_chmod = sys_f_chmod,
	.p_p_umask = sys_p_umask,
	.p_p_semaphore = sys_p_semaphore,
	.p_d_lock = sys_d_lock,
	.p_p_sigpause = sys_p_sigpause,
	.p_p_sigaction = sys_p_sigaction,
	.p_p_geteuid = sys_p_zero,
	.p_p_getegid = sys_p_zero,
	.p_p_waitpid = sys_p_waitpid,
	.p_d_getcwd = sys_d_getcwd,
	.p_s_alert = sys_s_alert,
	.p_t_malarm = sys_t_malarm,
	.p_s_uptime = sys_s_uptime,
	.p_d_xreaddir = sys_d_xreaddir,
	.p_f_stat64 = sys_f_stat64,
	.p_s_ync = sys_s_ync,
	.p_s_hutdown = sys_s_hutdown,
	.p_d_readlabel = sys_d_readlabel,
	.p_s_system = sys_s_system,
	.p_t_gettimeofday = sys_t_gettimeofday,
	.p_f_poll = (void *)sys_f_poll,
	.p_f_fstat = sys_f_fstat,
};

long (*dos_trap[0x190])();

/* trap #1's table: ours, without what TOS keeps; the module table's
 * empty slots get thunks that log */
void
dos_init(void)
{
	long (**t)() = (long (**)())&dos_tab;
	int i;

	sock_init(&dos_tab);
	for (i = 0; i < 0x190; i++) {
		dos_trap[i] = t[i];
		if (!t[i])
			t[i] = (long (*)())(xa_thunks + 8 * (512 + i));
	}
	dos_trap[0x2a] = dos_trap[0x2b] = dos_trap[0x2c] = dos_trap[0x2d] = 0;
}
