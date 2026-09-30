/*
 * t_dlm.c -- loadable kernel modules: modload, modstat, moduload,
 * modpath, modadm and getksym, with the modules in /tests/mod.d.
 *
 * dlmta exports dlmta_value (1000) and dlmta_add(); dlmtb depends on it
 * and its _load adds 234.  Loaded values are read back through getksym
 * and /dev/kmem.  Skips when the kernel has no module support.
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "sys/mod.h"
#include "sys/ksym.h"
#include "t.h"

#define	MD	"/tests/mod.d/"
#define	STT_OBJECT	1
#define	STT_FUNC	2

/* errno of a call: 0 on success */
#define	E(x)	((x) < 0 ? errno : 0)

/* a symbol's value by name, 0 if not found */
static unsigned long
sym(name)
char *name;
{
	unsigned long v = 0, info;

	if (getksym(name, &v, &info) < 0)
		return 0;
	return v;
}

/* a kernel long, -1 on error */
static long
kread(a)
unsigned long a;
{
	long v = -1;
	int fd = open("/dev/kmem", O_RDONLY);

	if (fd < 0)
		return -1;
	if (lseek(fd, (off_t)a, 0) == -1 || read(fd, (char *)&v, sizeof v) != sizeof v)
		v = -1;
	close(fd);
	return v;
}

static long
kval(name)
char *name;
{
	unsigned long a = sym(name);

	return a ? kread(a) : -1;
}

static int
nloaded()
{
	struct modstatus st;
	int n = 0, id = 1;

	while (modstat(id, &st, 1) == 0) {
		n++;
		id = st.ms_id + 1;
	}
	return n;
}

static void
test_getksym()
{
	char name[MAXSYMNMLEN + 44];
	unsigned long v, info;
	FILE *f;
	char n[64];
	unsigned long a = 0;

	f = fopen("/tests/ksyms", "r");
	while (f && fscanf(f, "%63s %lx", n, &a) == 2 && strcmp(n, "freemem") != 0)
		a = 0;
	if (f)
		fclose(f);
	strcpy(name, "freemem");
	v = 0;
	t_check("getksym_name", getksym(name, &v, &info) == 0 && v == a && a != 0 &&
	    info == STT_OBJECT, "%s: value 0x%lx (nm 0x%lx) type %lu", T_ERR, v, a, info);
	v = a + 2;
	name[0] = 0;
	t_check("getksym_addr", getksym(name, &v, &info) == 0 &&
	    strcmp(name, "freemem") == 0 && info == 2, "%s: '%s' +%lu", T_ERR, name, info);
	v = 0;
	t_check("getksym_func", getksym("dlm_init", &v, &info) == 0 && info == STT_FUNC,
	    "%s: type %lu", T_ERR, info);
	v = 0;
	t_check("getksym_nomatch", E(getksym("dlm_no_such_symbol", &v, &info)) == ENOMATCH,
	    "%s", T_ERR);
	t_check("getksym_efault", E(getksym("freemem", (unsigned long *)1, &info)) == EFAULT,
	    "%s", T_ERR);
	memset(name, 'x', sizeof name);
	name[MAXSYMNMLEN - 1] = 0;
	v = 0;
	t_check("getksym_longest", E(getksym(name, &v, &info)) == ENOMATCH, "%s", T_ERR);
	name[MAXSYMNMLEN - 1] = 'x';
	name[sizeof name - 1] = 0;
	v = 0;
	t_check("getksym_toolong", E(getksym(name, &v, &info)) == ENAMETOOLONG, "%s", T_ERR);
}

static void
test_load()
{
	struct modstatus st;
	int a, a2, b, e;
	unsigned long v, info, addr;
	char name[MAXSYMNMLEN];

	t_check("modpath_relative", E(modpath("tests/mod.d")) == EINVAL, "%s", T_ERR);
	t_check("modpath", modpath("/tests/mod.d") == 0, "%s", T_ERR);

	a = modload(MD "dlmta");
	if (!t_check("load_abs", a > 0, "%s", T_ERR))
		return;
	t_check("load_ran", kval("dlmta_loads") == 1 && kval("dlmta_value") == 1000,
	    "dlmta_loads %ld dlmta_value %ld", kval("dlmta_loads"), kval("dlmta_value"));
	a2 = modload("dlmta");
	t_check("load_again_same_id", a2 == a, "id %d, first %d: %s", a2, a, T_ERR);
	t_check("load_ran_once", kval("dlmta_loads") == 1, "dlmta_loads %ld",
	    kval("dlmta_loads"));

	b = modload("dlmtb");
	if (!t_check("load_dependent", b > a, "id %d: %s", b, T_ERR))
		return;
	t_check("dependent_called_dep", kval("dlmta_value") == 1234 &&
	    kval("dlmtb_result") == 1234, "dlmta_value %ld dlmtb_result %ld",
	    kval("dlmta_value"), kval("dlmtb_result"));
	t_check("dependent_read_kernel", kval("dlmtb_lbolt") > 0, "dlmtb_lbolt %ld",
	    kval("dlmtb_lbolt"));

	e = E(modstat(a, &st, 0));
	t_check("modstat", e == 0 && st.ms_id == a && strcmp(st.ms_name, "dlmta") == 0 &&
	    strcmp(st.ms_path, MD "dlmta") == 0 && st.ms_rev == 1 && st.ms_refcnt == 0 &&
	    st.ms_depcnt == 1 && (st.ms_flags & MS_DEMAND) && st.ms_size > 0 &&
	    st.ms_msinfo[0].mss_type == MOD_TY_MISC &&
	    strcmp(st.ms_msinfo[0].mss_linkinfo, "t_dlm base module") == 0,
	    "%s: id %d name '%s' path '%s' rev %d refs %d deps %d flags %d type %d '%s'",
	    strerror(e), st.ms_id, st.ms_name, st.ms_path, st.ms_rev, st.ms_refcnt,
	    st.ms_depcnt, st.ms_flags, st.ms_msinfo[0].mss_type, st.ms_msinfo[0].mss_linkinfo);
	t_info("modstat", "dlmta at 0x%lx, %u bytes", (unsigned long)st.ms_base, st.ms_size);

	/* a module address maps back to its symbol */
	addr = sym("dlmta_add");
	v = addr + 4;
	name[0] = 0;
	t_check("getksym_module_addr", addr >= (unsigned long)st.ms_base &&
	    addr < (unsigned long)st.ms_base + st.ms_size &&
	    getksym(name, &v, &info) == 0 && strcmp(name, "dlmta_add") == 0 && info == 4,
	    "0x%lx '%s' +%lu: %s", addr, name, info, T_ERR);

	e = E(modstat(b, &st, 0));
	t_check("modstat_dependent", e == 0 && strcmp(st.ms_path, MD "dlmtb") == 0 &&
	    st.ms_depcnt == 0 && st.ms_msinfo[0].mss_type == MOD_TY_MISC,
	    "%s: path '%s' deps %d", strerror(e), st.ms_path, st.ms_depcnt);
	t_check("modstat_iterate", nloaded() == 2, "%d modules", nloaded());
	t_check("modstat_efault", E(modstat(a, (struct modstatus *)1, 0)) == EFAULT, "%s", T_ERR);
	t_check("modstat_noid", E(modstat(9999, &st, 0)) == EINVAL, "%s", T_ERR);

	/* unload in dependency order */
	t_check("unload_dep_busy", E(moduload(a)) == EBUSY, "%s", T_ERR);
	t_check("unload_dependent", moduload(b) == 0 && sym("dlmtb_result") == 0, "%s", T_ERR);
	t_check("dep_released", modstat(a, &st, 0) == 0 && st.ms_depcnt == 0,
	    "deps %d", st.ms_depcnt);
	t_check("unload", moduload(a) == 0 && sym("dlmta_value") == 0 && nloaded() == 0,
	    "%s", T_ERR);
	t_check("unloaded_gone", E(modstat(a, &st, 0)) == EINVAL &&
	    E(moduload(a)) == EINVAL, "%s", T_ERR);

	/* reload: fresh data, new ids; moduload(0) takes both */
	b = modload("dlmtb");
	t_check("load_pulls_dep", b > 0 && kval("dlmta_value") == 1234 &&
	    kval("dlmta_loads") == 1 && nloaded() == 2, "%s: dlmta_value %ld",
	    T_ERR, kval("dlmta_value"));
	t_check("unload_all", moduload(0) == 0 && nloaded() == 0, "%d left", nloaded());
	t_check("unload_all_none", E(moduload(0)) == EINVAL, "%s", T_ERR);
}

static void
test_errors()
{
	struct mod_mreg reg;
	int id, e;

	id = modload(MD "dlmta");
	e = E(modload(MD "nosuch"));
	t_check("load_enoent", e == ENOENT, "%s", strerror(e));
	e = E(modload("/tests/ksyms"));
	t_check("load_not_elf", e == EINVAL, "%s", strerror(e));
	e = E(modload(MD "abcdefghijklmno"));
	t_check("load_name_too_long", e == ENAMETOOLONG, "%s", strerror(e));
	e = E(modload((char *)1));
	t_check("load_efault", e == EFAULT, "%s", strerror(e));
	e = E(modload(MD "dlmtu"));
	t_check("load_undefined", e == ERELOC, "errno %d", e);
	e = E(modload(MD "dlmtv"));
	t_check("load_badver", e == EBADVER, "errno %d", e);
	e = E(modload(MD "dlmtl"));
	t_check("load_fails", e == ENODEV, "errno %d", e);
	e = E(modload(MD "dlmtd"));
	t_check("load_missing_dep", e == EINVAL, "errno %d", e);
	e = E(modload(MD "dlmtw"));
	t_check("load_wrapper_wrap", e == ERELOC, "errno %d", e);
	t_check("failures_unwound", nloaded() == 1, "%d loaded", nloaded());
	t_check("ids_consumed", modload(MD "dlmtb") > id + 8, "%s", T_ERR);

	t_check("unload_bad_id", E(moduload(-1)) == EINVAL && E(moduload(9999)) == EINVAL,
	    "%s", T_ERR);
	memset(&reg, 0, sizeof reg);
	strcpy(reg.md_modname, "dlmta");
	t_check("modadm_misc", modadm(MOD_TY_MISC, MOD_C_MREG, (caddr_t)&reg) == 0, "%s", T_ERR);
	t_check("modadm_badtype", E(modadm(99, MOD_C_MREG, (caddr_t)&reg)) == EINVAL, "%s", T_ERR);
	moduload(0);
	t_check("errors_clean", nloaded() == 0, "%d loaded", nloaded());
}

/* a non-root child: EPERM for all but getksym */
static void
test_perm()
{
	struct modstatus st;
	struct mod_mreg reg;
	unsigned long v, info;
	int pid, s, r;

	pid = fork();
	if (pid == 0) {
		r = 0;
		setgid(1);
		if (setuid(1) < 0)
			_exit(100);
		strcpy(reg.md_modname, "dlmta");
		r |= E(modload(MD "dlmta")) != EPERM;
		r |= (E(moduload(0)) != EPERM) << 1;
		r |= (E(modpath("/tmp")) != EPERM) << 2;
		r |= (E(modstat(1, &st, 1)) != EPERM) << 3;
		r |= (E(modadm(MOD_TY_MISC, MOD_C_MREG, (caddr_t)&reg)) != EPERM) << 4;
		v = 0;
		r |= (getksym("freemem", &v, &info) != 0 || v == 0) << 5;
		_exit(r);
	}
	r = t_waitchild(pid, &s, 20);
	t_check("perm", r == pid && WIFEXITED(s) && WEXITSTATUS(s) == 0,
	    "status 0x%x (bits: load unload path stat adm getksym)", s);
}

/* modadmin exit status */
static int
admin(a1, a2)
char *a1, *a2;
{
	int pid, s;

	pid = fork();
	if (pid == 0) {
		close(1);
		close(2);
		open("/dev/null", O_WRONLY);
		dup(1);
		execl("/tests/modadmin", "modadmin", a1, a2, (char *)0);
		_exit(127);
	}
	if (t_waitchild(pid, &s, 20) != pid || !WIFEXITED(s))
		return -1;
	return WEXITSTATUS(s);
}

static void
test_modadmin()
{
	t_check("modadmin_load", admin("-l", MD "dlmta") == 0 && nloaded() == 1, "");
	t_check("modadmin_list", admin("-s", (char *)0) == 0, "");
	t_check("modadmin_bad", admin("-l", MD "nosuch") == 1, "");
	t_check("modadmin_unload", admin("-U", "dlmta") == 0 && nloaded() == 0, "");
}

int
main()
{
	t_init("dlm", 60);
	if (t_kmem("dlm_inited") != 1) {
		t_skip("all", "kernel has no loadable-module support");
		return t_done();
	}
	moduload(0);
	test_getksym();
	test_load();
	test_errors();
	test_perm();
	test_modadmin();
	moduload(0);
	modpath((char *)0);
	return t_done();
}
