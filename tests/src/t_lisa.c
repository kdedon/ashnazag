/*
 * t_lisa: the Lisa adapter's hardware, screen and input code
 * (lh_* helpers, each exits 0 when its checks pass) and its lplatform
 * module: registered, loaded on open, unloaded.
 */
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "sys/mod.h"
#include <sys/wait.h>
#include "lisa_runtime.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"

static int
modid(name)
	char *name;
{
	struct modstatus st;
	int id = 1;

	while (modstat(id, &st, 1) == 0) {
		if (strcmp(st.ms_name, name) == 0)
			return st.ms_id;
		id = st.ms_id + 1;
	}
	return -1;
}

static void
helper(name)
	char *name;
{
	char path[64];
	int pid, st;

	sprintf(path, "/tests/lh_%s", name);
	if ((pid = fork()) == 0) {
		execl(path, path, (char *)0);
		_exit(127);
	}
	t_check(name, pid > 0 && t_waitchild(pid, &st, 30) == pid && st == 0, "status 0x%x", st);
}

int
main()
{
	struct mod_mreg reg;
	int mj = LP_MAJOR, id, fd;

	t_init("lisa", 180);
	helper("hardware");
	helper("render");
	helper("publish");
	if (access(MD "/lplatform", 0) < 0 || access("/dev/lplatform", 0) < 0) {
		t_skip("module", "no lplatform module on this root");
		return t_done();
	}
	modpath(MD);
	memset(&reg, 0, sizeof reg);
	strcpy(reg.md_modname, "lplatform");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "%s", T_ERR))
		return t_done();
	fd = open("/dev/lplatform", O_RDWR);
	t_check("open_autoload", fd >= 0, "%s", T_ERR);
	if (fd >= 0)
		close(fd);
	id = modid("lplatform");
	t_check("loaded", id > 0, "not in modstat");
	if (id > 0)
		t_check("unload", moduload(id) == 0, "%s", T_ERR);
	return t_done();
}
