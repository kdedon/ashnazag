/*
 * kentry.c -- the module interface XaAES and its ADI modules call:
 * MiNT's struct kentry, filled from this layer.  Slots nobody serves
 * point at thunks that log their number and return ENOSYS.
 */
#include <stdarg.h>
#include "xamint.h"

#define	MINT_MAJ_VERSION 1
#define	MINT_MIN_VERSION 19
#define	MINT_PATCH_LEVEL 0

struct global global = {
	.machine = machine_unknown,
	.tosvers = 0x0206,
	.gl_lang = 0,
	.gl_kbd = 0,
	.sysdrv = 2,
	.sysdir = "c:\\mint\\",
	.mchdir = "c:\\mint\\",
};
unsigned long loops_per_sec = 1000000;
unsigned long c20ms;
struct timeval xtime;
int debug_level = 1;
long mcpu = 30;			/* the CPU, for libkern's block copy */
long _cdecl (*aes_disp)(void *);
static struct dma dma;

extern void _cdecl cpush(const void *base, long size);
extern void _cdecl cpushi(const void *base, long size);
extern void _cdecl canceltimeout(TIMEOUT *t);
extern TIMEOUT *_cdecl addtimeout_curproc(long delta, void (*func)(PROC *, long));
extern void _cdecl iwake(int que, long cond, short pid);
extern long _cdecl killgroup(int pgrp, unsigned short sig, int priv);
extern void _cdecl raise(unsigned short sig);
extern void *_cdecl proc_lookup_extension(struct proc *p, long ident);
extern void *_cdecl proc_attach_extension(struct proc *p, long ident, unsigned long flags,
	unsigned long size, struct module_callback *cb);
extern void _cdecl proc_detach_extension(struct proc *p, long ident);
extern long _cdecl proc_setuid(struct proc *p, unsigned short uid);
extern long _cdecl proc_setgid(struct proc *p, unsigned short gid);
extern MEMREGION *_cdecl addr2mem(struct proc *p, long addr);
extern long _cdecl attach_region(struct proc *p, MEMREGION *m);
extern void _cdecl detach_region(struct proc *p, MEMREGION *m);
extern long _cdecl kernel_opendir(struct dirstruct *dirh, const char *name);
extern long _cdecl kernel_readdir(struct dirstruct *dirh, char *buf, int len);
extern void _cdecl kernel_closedir(struct dirstruct *dirh);
extern struct file *_cdecl kernel_open(const char *path, int rwmode, long *err, XATTR *x);
extern long _cdecl kernel_read(struct file *f, void *buf, long n);
extern long _cdecl kernel_write(struct file *f, const void *buf, long n);
extern long _cdecl kernel_lseek(FILEPTR *f, long where, int whence);
extern void _cdecl kernel_close(struct file *f);
extern void _cdecl load_modules(const char *path, const char *ext,
	long _cdecl (*loader)(struct basepage *, const char *, short *, short *));
extern unsigned long _cdecl remaining_proc_time(void);
extern long _cdecl trap_13_emu(short fnum, ...);
extern long _cdecl trap_14_emu(short fnum, ...);
extern long _cdecl parse_cnf(const char *path, struct parser_item *, void *, unsigned long);
extern long _cdecl parse_include(const char *path, struct parsinf *, struct parser_item *);
extern void _cdecl parser_msg(struct parsinf *, const char *msg);

/* ---- debug output ---- */

static void
vlog(const char *pfx, const char *fmt, va_list a)
{
	char buf[200];

	kvsprintf(buf, sizeof buf, fmt, a);
	xlog("%s%s", pfx, buf);
}

void _cdecl
Trace(const char *fmt, ...)
{
	va_list a;

	if (debug_level < 3)
		return;
	va_start(a, fmt);
	vlog("", fmt, a);
	va_end(a);
}

void _cdecl
Debug(const char *fmt, ...)
{
	va_list a;

	if (debug_level < 2)
		return;
	va_start(a, fmt);
	vlog("", fmt, a);
	va_end(a);
}

void _cdecl
ALERT(const char *fmt, ...)
{
	va_list a;

	va_start(a, fmt);
	vlog("ALERT: ", fmt, a);
	va_end(a);
}

void _cdecl
FORCE(const char *fmt, ...)
{
	va_list a;

	va_start(a, fmt);
	vlog("", fmt, a);
	va_end(a);
}

void _cdecl
FATAL(const char *fmt, ...)
{
	va_list a;

	va_start(a, fmt);
	vlog("FATAL: ", fmt, a);
	va_end(a);
	sys_p_term(-1);
	for (;;)
		;
}

/* ---- cookies ---- */

long
cookie_get(long tag, long *val)
{
	long *j = *(long **)0x5a0;

	for (; j && j[0]; j += 2)
		if (j[0] == tag) {
			if (val)
				*val = j[1];
			return 0;
		}
	return EERROR;
}

long
cookie_set(long tag, long val)
{
	long *j = *(long **)0x5a0, *s = j, n;

	for (; j && j[0]; j += 2)
		if (j[0] == tag) {
			j[1] = val;
			return 0;
		}
	if (!j)
		return ENOMEM;
	n = j[1];
	if ((j - s) / 2 + 1 >= n)
		return ENOMEM;
	j[2] = 0;
	j[3] = n;
	j[0] = tag;
	j[1] = val;
	return 0;
}

static long _cdecl
get_toscookie(unsigned long tag, unsigned long *val)
{
	return cookie_get(tag, (long *)val);
}

/* ---- traps and modules ---- */

static long _cdecl
register_trap2(long _cdecl (*dispatch)(void *), int mode, int flag, long extra)
{
	(void)extra;
	if (flag != 0)
		return EINVAL;
	if (mode == 0) {
		if (aes_disp)
			return EACCES;
		aes_disp = dispatch;
		xlog("AES dispatcher at %lx", (long)dispatch);
		return 0;
	}
	if (aes_disp != dispatch)
		return EINVAL;
	aes_disp = 0;
	return 0;
}

static long _cdecl
trap_1_emu(short fnum, ...)
{
	typedef long _cdecl (*f8)(long, long, long, long, long, long, long, long);
	long *a = (long *)(&fnum + 1);
	f8 f;

	if (fnum < 0 || fnum >= 0x190)
		return ENOSYS;
	f = ((f8 *)&dos_tab)[fnum];
	return (*f)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

static void _cdecl
changedrv(unsigned short drv, const char *function)
{
	(void)drv;
	(void)function;
}

/* a cookie holds a copy of the MiNT path */
static long _cdecl
path2cookie(struct proc *p, const char *path, char *lastname, fcookie *res)
{
	char *c;

	(void)p;
	(void)lastname;
	bzero(res, sizeof *res);
	c = kmalloc_o(strlen(path) + 1, 0);
	if (!c)
		return ENOMEM;
	strcpy(c, path);
	res->index = (long)c;
	res->dev = 0xa5;
	return 0;
}

static void _cdecl
release_cookie(fcookie *fc)
{
	if (fc->dev == 0xa5 && fc->index) {
		kfree_any((void *)fc->index);
		fc->index = 0;
	}
}

static long _cdecl
xfs_getxattr(FILESYS *fs, fcookie *fc, XATTR *x)
{
	char hp[PLEN], st[HST_SIZE];
	long r;

	(void)fs;
	if (fc->dev != 0xa5 || host_path((char *)fc->index, hp, 0) != 0)
		return EINVAL;
	if ((r = hstat(hp, st, 1)) < 0)
		return merr(r);
	st_xattr(st, x);
	return 0;
}

static long _cdecl
xfs_readlink(FILESYS *fs, fcookie *fc, char *buf, int len)
{
	char hp[PLEN];
	long r;

	(void)fs;
	if (fc->dev != 0xa5 || host_path((char *)fc->index, hp, 0) != 0)
		return EINVAL;
	r = hc(H_READLINK, 3, (long)hp, (long)buf, (long)len - 1);
	if (r < 0)
		return merr(r);
	buf[r] = 0;
	return 0;
}

static long _cdecl
add_rsvfentry(char *name, char a, char b)
{
	(void)name;
	(void)a;
	(void)b;
	return 0;
}

static long _cdecl
del_rsvfentry(char *name)
{
	(void)name;
	return 0;
}

struct kentry kentry = {
	.major = MINT_MAJ_VERSION,
	.minor = MINT_MIN_VERSION,
	.patchlevel = MINT_PATCH_LEVEL,
	.beta = 0,
	.version_major = KENTRY_MAJ_VERSION,
	.version_minor = KENTRY_MIN_VERSION,
	.dos_version = 0x00000040,
	.vec_dos = &dos_tab,
	.vec_mch = {
		.global = &global,
		.loops_per_sec = &loops_per_sec,
		.c20ms = &c20ms,
		.cpush = cpush,
		.cpushi = cpushi,
	},
	.vec_proc = {
		.nap = nap,
		.sleep = sleep,
		.wake = wake,
		.wakeselect = wakeselect,
		.ikill = ikill,
		.iwake = iwake,
		.killgroup = killgroup,
		.raise = raise,
		.addtimeout = addtimeout_curproc,
		.canceltimeout = canceltimeout,
		.addroottimeout = addroottimeout,
		.cancelroottimeout = cancelroottimeout,
		.create_process = create_process,
		.kthread_create = kthread_create,
		.kthread_exit = kthread_exit,
		.get_curproc = get_curproc,
		.pid2proc = pid2proc,
		.lookup_extension = proc_lookup_extension,
		.attach_extension = proc_attach_extension,
		.detach_extension = proc_detach_extension,
		.proc_setuid = proc_setuid,
		.proc_setgid = proc_setgid,
	},
	.vec_mem = {
		.kmalloc = _kmalloc,
		.kfree = _kfree,
		.umalloc = _umalloc,
		.ufree = _ufree,
		.addr2mem = addr2mem,
		.attach_region = attach_region,
		.detach_region = detach_region,
	},
	.vec_fs = {
		.default_perm = 0644,
		.default_dir_perm = 0755,
		._changedrv = changedrv,
		.xtime = &xtime,
		.kernel_opendir = kernel_opendir,
		.kernel_readdir = kernel_readdir,
		.kernel_closedir = kernel_closedir,
		.kernel_open = kernel_open,
		.kernel_read = kernel_read,
		.kernel_write = kernel_write,
		.kernel_lseek = kernel_lseek,
		.kernel_close = kernel_close,
		.path2cookie = path2cookie,
		.release_cookie = release_cookie,
	},
	.vec_module = {
		.load_modules = load_modules,
		.register_trap2 = register_trap2,
	},
	.vec_cnf = {
		.parse_cnf = parse_cnf,
		.parse_include = parse_include,
		.parser_msg = parser_msg,
	},
	.vec_misc = {
		.dma = &dma,
		.get_toscookie = get_toscookie,
		.add_rsvfentry = add_rsvfentry,
		.del_rsvfentry = del_rsvfentry,
		.remaining_proc_time = remaining_proc_time,
		.trap_1_emu = trap_1_emu,
		.trap_13_emu = trap_13_emu,
		.trap_14_emu = trap_14_emu,
	},
	.vec_debug = {
		.debug_level = &debug_level,
		.trace = Trace,
		.debug = Debug,
		.alert = ALERT,
		.fatal = FATAL,
		.force = FORCE,
	},
	.vec_libkern = DEFAULTS_kentry_libkern,
	.vec_xfs = {
		.getxattr = xfs_getxattr,
		.readlink = xfs_readlink,
	},
};

/* empty function slots in [from, to) to numbered thunks */
static void
fill(void *from, void *to)
{
	long *p;

	for (p = from; p < (long *)to; p++)
		if (!*p)
			*p = (long)(xa_thunks + 8 * (p - (long *)&kentry));
}

void
kentry_init(void)
{
	long *d = (long *)&dma;
	int i;

	fill(&kentry.vec_proc, &kentry.vec_mem + 1);
	fill(&kentry.vec_fs._changedrv, &kentry.vec_fs.bio);
	fill(&kentry.vec_fs.kernel_opendir, &kentry.vec_fs + 1);
	fill(&kentry.vec_sockets, &kentry.vec_cnf + 1);
	fill(&kentry.vec_misc.get_toscookie, &kentry.vec_misc + 1);
	fill(&kentry.vec_debug.trace, &kentry.vec_debug + 1);
	fill(&kentry.vec_xfs, &kentry.vec_xdd + 1);
	fill(&kentry.vec_pcibios.Find_pci_device, &kentry + 1);
	for (i = 0; i < (int)(sizeof dma / 4); i++)
		d[i] = (long)(xa_thunks + 8 * (500 + i));
}
