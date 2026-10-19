/*
 * exec.c -- programs and modules: loading a PRG with its basepage,
 * processes made from one, Pexec, XaAES's module and its ADI modules.
 */
#include "xamint.h"

#define	INITIALTPA (128 * 1024L)

struct prghdr {
	short	magic;
	long	tlen, dlen, blen, slen, res, flags;
	short	abs;
} __attribute__((packed));

extern long mcpu;

/* a 68020/030 has no cpusha: clear both caches through the CACR */
static void
cpush_all(void)
{
	if (mcpu >= 40)
		__asm__ __volatile__("cpusha %%bc" : : : "memory");
	else
		__asm__ __volatile__("movec %%cacr,%%d0; orw #0x808,%%d0; movec %%d0,%%cacr"
		    : : : "d0", "memory");
}

void _cdecl
cpush(const void *base, long size)
{
	(void)base;
	(void)size;
	cpush_all();
}

void _cdecl
cpushi(const void *base, long size)
{
	(void)base;
	(void)size;
	cpush_all();
}

static long
readn(long fd, void *buf, long n)
{
	long r, done = 0;

	while (done < n) {
		r = hc(H_READ, 3, fd, (long)buf + done, n - done);
		if (r <= 0)
			return r < 0 ? r : done;
		done += r;
	}
	return done;
}

/* relocation: a long offset, then byte steps (1: 254 on) */
static long
reloc(long fd, char *text, long size)
{
	unsigned char buf[512];
	long off, n, i;
	char *p;

	if (readn(fd, &off, 4) != 4 || off == 0)
		return 0;
	if (off < 0 || off > size - 4)
		return ENOEXEC;
	p = text + off;
	*(long *)p += (long)text;
	for (;;) {
		n = hc(H_READ, 3, fd, (long)buf, (long)sizeof buf);
		if (n <= 0)
			return 0;
		for (i = 0; i < n; i++) {
			if (buf[i] == 0)
				return 0;
			if (buf[i] == 1) {
				p += 254;
				continue;
			}
			p += buf[i];
			if (p > text + size - 4)
				return ENOEXEC;
			*(long *)p += (long)text;
		}
	}
}

/*
 * The program at host path hp into a new TPA owned by k: stack bytes
 * past its BSS, or (0) INITIALTPA, as MiNT gives, so programs started
 * together each get room; they take more with Malloc.  Its basepage,
 * or 0 and *err.
 */
static BASEPAGE *
prg_read(const char *hp, struct kp *k, long stack, long *err)
{
	struct prghdr h;
	BASEPAGE *b;
	long fd, need, size, r;

	fd = hc(H_OPEN, 3, (long)hp, 0L, 0L);
	if (fd < 0) {
		*err = merr(fd);
		return 0;
	}
	if (readn(fd, &h, sizeof h) != sizeof h || h.magic != 0x601a) {
		hc(H_CLOSE, 1, fd);
		*err = ENOEXEC;
		return 0;
	}
	need = 256 + h.tlen + h.dlen + h.blen;
	size = need + (stack ? stack : INITIALTPA);
	if (size < need + 1024)
		size = need + 1024;
	b = kmalloc_o(size, k);
	if (!b) {
		hc(H_CLOSE, 1, fd);
		*err = ENOMEM;
		return 0;
	}
	bzero(b, 256);
	b->p_lowtpa = (long)b;
	b->p_hitpa = (long)b + size;
	b->p_tbase = (long)b + 256;
	b->p_tlen = h.tlen;
	b->p_dbase = b->p_tbase + h.tlen;
	b->p_dlen = h.dlen;
	b->p_bbase = b->p_dbase + h.dlen;
	b->p_blen = h.blen;
	b->p_dta = b->p_cmdlin;
	b->p_flags = h.flags;
	r = readn(fd, (char *)b->p_tbase, h.tlen + h.dlen);
	if (r != h.tlen + h.dlen) {
		*err = ENOEXEC;
		goto bad;
	}
	if (!h.abs) {
		if (hc(H_LSEEK, 3, fd, 28 + h.tlen + h.dlen + h.slen, 0L) < 0 ||
		    (*err = reloc(fd, (char *)b->p_tbase, h.tlen + h.dlen)) != 0)
			goto bad;
	}
	bzero((char *)b->p_bbase, h.blen);
	hc(H_CLOSE, 1, fd);
	cpush_all();
	return b;
bad:
	hc(H_CLOSE, 1, fd);
	kfree_any(b);
	return 0;
}

/* an environment copy owned by k */
static char *
env_copy(const char *env, struct kp *k)
{
	const char *e;
	char *n;
	long len;

	if (!env)
		env = "";
	for (e = env; *e; e += strlen(e) + 1)
		;
	len = e - env + 2;
	n = kmalloc_o(len, k);
	if (n) {
		memcpy(n, env, len - 1);
		n[len - 1] = 0;
	}
	return n;
}

static void
basename_noext(const char *path, char *out, int n)
{
	const char *s = path, *p;
	int i;

	for (p = path; *p; p++)
		if (*p == '\\' || *p == '/' || *p == ':')
			s = p + 1;
	for (i = 0; s[i] && s[i] != '.' && i < n - 1; i++)
		out[i] = s[i];
	out[i] = 0;
}

/* the process starts at the program's text with the basepage on its stack */
static void
proc_setstart(struct kp *k, BASEPAGE *b)
{
	CONTEXT *c = &k->p.ctxt[CURRENT];
	long *usp = (long *)((b->p_hitpa - 8) & ~3L);

	bzero(c, sizeof *c);
	usp[1] = (long)b;
	usp[0] = 0;
	c->usp = (long)usp;
	c->pc = b->p_tbase;
	c->sr = 0;
	k->base = b;
	k->p.p_mem->base = b;
	k->dta = (char *)b->p_cmdlin;
	b->p_dta = b->p_cmdlin;
}

long _cdecl
create_process(const void *filename, const void *cmdline, const void *env, struct proc **pret,
	long stack, struct create_process_opts *opts)
{
	struct kp *k, *parent = KP(curproc);
	BASEPAGE *b;
	char hp[PLEN], name[PNAMSIZ + 1];
	long err = 0;

	if (host_path(filename, hp, 1) < 0)
		return ENOENT;
	basename_noext(filename, name, sizeof name);
	if (host_prg(hp, filename)) {
		err = host_spawn(hp, cmdline, name, opts && (opts->mode & CREATE_PROCESS_OPTS_DEFDIR) ?
		    opts->defdir : 0, &k);
		if (!err && pret)
			*pret = &k->p;
		return err;
	}
	k = proc_new(parent, name, 0, 0);
	if (!k)
		return ENOMEM;
	b = prg_read(hp, k, stack, &err);
	if (!b) {
		xlog("create_process %s: %ld", (const char *)filename, err);
		k->zombie = 1;
		k->p.ppid = 0;
		fd_closeall(k);
		return err ? err : ENOEXEC;
	}
	b->p_env = env_copy(env, k);
	b->p_parent = parent->base;
	if (cmdline)
		memcpy(b->p_cmdlin, cmdline, 128);
	strncpy(k->p.fname, filename, sizeof k->p.fname - 1);
	strncpy(k->p.cmdlin, cmdline ? (const char *)cmdline + 1 : "", sizeof k->p.cmdlin - 1);
	proc_setstart(k, b);
	if (opts && (opts->mode & CREATE_PROCESS_OPTS_DEFDIR) && opts->defdir) {
		const char *d = opts->defdir;

		if (d[0] && d[1] == ':') {
			k->p.p_cwd->curdrv = (d[0] | 0x20) - 'a';
			d += 2;
		}
		dir_set(k, k->p.p_cwd->curdrv, d);
	}
	strupr(name);
	xlog("start %s pid %d tpa %lx-%lx", name, k->p.pid, b->p_lowtpa, b->p_hitpa);
	if (pret)
		*pret = &k->p;
	ready(k);
	return 0;
}

static long
waitchild(struct kp *k)
{
	struct kp *me = KP(curproc);
	long code;

	while (!k->zombie)
		sleep(WAIT_Q, (long)curproc);
	code = k->exitcode;
	(void)me;
	proc_reap(k);
	return code;
}

/* a process for basepage b, which the caller loaded or made */
static struct kp *
proc_frombase(BASEPAGE *b, const char *name)
{
	struct kp *k = proc_new(KP(curproc), name, 0, 0);

	if (!k)
		return 0;
	mem_give(b, k);
	if (b->p_env)
		mem_give(b->p_env, k);
	proc_setstart(k, b);
	return k;
}

/*
 * Pexec 200: the caller becomes the program, keeping its pid and
 * handles.  A Pvfork child leaves its parent's memory; anyone else's
 * old memory goes.  No return on success: the kernel stack starts over.
 */
static long
exec_over(const char *path, const char *cmd, const char *env)
{
	struct kp *k = KP(curproc);
	BASEPAGE *b, *pb = k->base ? k->base->p_parent : 0;
	char hp[PLEN], name[PNAMSIZ + 1], tail[128];
	long err = 0, i;
	char *e;

	if (host_path(path, hp, 1) < 0)
		return EFILNF;
	basename_noext(path, name, sizeof name);
	b = prg_read(hp, 0, 0, &err);
	if (!b)
		return err ? err : ENOEXEC;
	e = env_copy(env ? env : k->base ? k->base->p_env : 0, 0);
	if (cmd)
		memcpy(tail, cmd, 128);
	else
		tail[0] = tail[1] = 0;
	strncpy(k->p.fname, path, sizeof k->p.fname - 1);
	if (k->vfparent)
		vfork_release(k);
	else {
		fork_exit(k);
		mem_freeproc(k);
	}
	mem_give(b, k);
	mem_give(e, k);
	b->p_env = e;
	b->p_parent = pb;
	memcpy(b->p_cmdlin, tail, 128);
	for (i = MIN_HANDLE; i < NOFILE; i++)
		if (k->p.p_fd->ofiles[i] && (k->p.p_fd->ofileflags[i] & FD_CLOEXEC)) {
			file_close(k->p.p_fd->ofiles[i]);
			k->p.p_fd->ofiles[i] = 0;
		}
	proc_sigreset(k);
	strncpy(k->p.name, name, PNAMSIZ);
	proc_setstart(k, b);
	strupr(name);
	xlog("exec %s pid %d tpa %lx-%lx", name, k->p.pid, b->p_lowtpa, b->p_hitpa);
	xa_spl7();
	xa_rexec(k->p.sysstack);
	return 0;
}

long _cdecl
sys_p_exec(short mode, const void *p1, const void *p2, const void *p3)
{
	struct kp *k;
	BASEPAGE *b;
	char hp[PLEN], name[PNAMSIZ + 1];
	long err = 0;

	switch (mode) {
	case 200:
		return exec_over(p1, p2, p3);
	case 0: case 100: case 3:
		if (host_path(p1, hp, 1) < 0)
			return EFILNF;
		basename_noext(p1, name, sizeof name);
		if (mode != 3 && host_prg(hp, p1)) {
			if ((err = host_spawn(hp, p2, name, 0, &k)) != 0)
				return err;
			xlog("Pexec %d %s: Unix process %ld, pid %d", mode, (const char *)p1,
			    k->hostpid, k->p.pid);
			return mode == 100 ? k->p.pid : waitchild(k);
		}
		k = proc_new(KP(curproc), name, 0, 0);
		if (!k)
			return ENOMEM;
		b = prg_read(hp, k, 0, &err);
		if (!b) {
			k->zombie = 1;
			k->p.ppid = 0;
			return err;
		}
		b->p_env = env_copy(p3 ? p3 : KP(curproc)->base ? KP(curproc)->base->p_env : 0, k);
		b->p_parent = KP(curproc)->base;
		if (p2)
			memcpy(b->p_cmdlin, p2, 128);
		if (mode == 3) {
			mem_give(b, KP(curproc));
			mem_give(b->p_env, KP(curproc));
			k->zombie = 1;
			k->p.ppid = 0;
			return (long)b;
		}
		strncpy(k->p.fname, p1, sizeof k->p.fname - 1);
		proc_setstart(k, b);
		ready(k);
		xlog("Pexec %d %s: pid %d", mode, (const char *)p1, k->p.pid);
		if (mode == 100)
			return k->p.pid;
		return waitchild(k);
	case 4: case 6: case 104: case 106:
		b = (BASEPAGE *)p2;
		if (!b)
			return EINVAL;
		k = proc_frombase(b, mode >= 100 && p1 ? (const char *)p1 : "child");
		if (!k)
			return ENOMEM;
		ready(k);
		if (mode >= 100)
			return k->p.pid;
		return waitchild(k);
	case 5: case 7:
		b = kmalloc_o(mem_largest(), KP(curproc));
		if (!b)
			return ENOMEM;
		bzero(b, 256);
		b->p_lowtpa = (long)b;
		b->p_hitpa = (long)b + mem_largest();
		b->p_tbase = (long)b + 256;
		b->p_env = env_copy(p3 ? p3 : KP(curproc)->base ? KP(curproc)->base->p_env : 0, KP(curproc));
		b->p_parent = KP(curproc)->base;
		if (p2)
			memcpy(b->p_cmdlin, p2, 128);
		return (long)b;
	}
	xlog("Pexec mode %d unserved", mode);
	return EINVAL;
}

/* ---- modules ---- */

static BASEPAGE *
mod_read(const char *mpath, long *err)
{
	char hp[PLEN];

	if (host_path(mpath, hp, 1) < 0) {
		*err = ENOENT;
		return 0;
	}
	return prg_read(hp, 0, 1024, err);
}

void _cdecl
load_modules(const char *path, const char *ext,
	long _cdecl (*loader)(struct basepage *, const char *, short *, short *))
{
	char buf[PLEN], name[PLEN];
	struct dirstruct d;
	BASEPAGE *b;
	long err, r;
	int el = strlen(ext), nl;
	short c1 = 0, c2 = 0;

	if (!path)
		path = global.sysdir;
	if (kernel_opendir(&d, path) < 0)
		return;
	while (kernel_readdir(&d, buf, sizeof buf) == 0) {
		nl = strlen(buf + 4);
		if (nl <= el || stricmp(buf + 4 + nl - el, ext))
			continue;
		strcpy(name, path);
		strcat(name, buf + 4);
		b = mod_read(name, &err);
		xlog("module %s: %lx (%ld)", name, (long)b, b ? 0L : err);
		if (!b)
			continue;
		r = (*loader)(b, buf + 4, &c1, &c2);
		if (r)
			kfree_any(b);
	}
	kernel_closedir(&d);
}

/* XaAES: load the module and run init_km in this process */
long
run_xaaes(const char *path)
{
	static struct kernel_module km;
	long err = 0, r;
	BASEPAGE *b = mod_read(path, &err);
	long _cdecl (*init)(struct kentry *, struct kernel_module *);
	char *s;

	if (!b) {
		xlog("%s: %ld", path, err);
		return err;
	}
	bzero(&km, sizeof km);
	km.b = b;
	km.caller = curproc;
	strcpy(km.path, path);
	s = strrchr(km.path, '\\');
	if (s) {
		strcpy(km.name, s + 1);
		s[1] = 0;
	}
	init = (long _cdecl (*)(struct kentry *, struct kernel_module *))b->p_tbase;
	xlog("init_km at %lx", (long)init);
	r = (*init)(&kentry, &km);
	xlog("init_km: %ld", r);
	return r;
}
