#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include "sys/mod.h"
#include "t.h"

static char *hooks[] = { "guest_gate_c_hook", "guest_linea_hook",
    "guest_ftrap_hook", "guest_fpriv_hook" };
static char *targets[] = { "gc_gate_c", "gc_linea", "gc_ftrap", "gc_fpriv" };

static int
status(name, st)
char *name;
struct modstatus *st;
{
    int id = 1;

    while (modstat(id, st, 1) == 0) {
        if (strcmp(st->ms_name, name) == 0)
            return st->ms_id;
        id = st->ms_id + 1;
    }
    return -1;
}

static int
hookvalues(st)
struct modstatus *st;
{
    unsigned long p, target, info, value;
    int i, fd, ok = 1;

    fd = open("/dev/kmem", O_RDONLY);
    if (fd < 0)
        return 0;
    for (i = 0; i < 4; i++) {
        p = target = 0;
        if (getksym(hooks[i], &p, &info) < 0 || lseek(fd, (off_t)p, 0) == -1 ||
            read(fd, (char *)&value, 4) != 4) {
            ok = 0;
            continue;
        }
        if (!st)
            ok &= value == 0;
        else
            ok &= getksym(targets[i], &target, &info) == 0 && value == target &&
                target >= (unsigned long)st->ms_base &&
                target - (unsigned long)st->ms_base < st->ms_size;
    }
    close(fd);
    return ok;
}

int
main()
{
    struct modstatus st, core;
    struct mod_mreg reg;
    struct mod_execreg er;
    unsigned long p = 0, info;
    int id, aid = -1, xid, pid, waitst, round, busy = 0;
    long end;

    t_init("guestdlm", 60);
    if (getksym(hooks[0], &p, &info) < 0 || access("/tests/aux/mod.d/guestcore", 0) < 0) {
        t_skip("all", "no modular guest gates");
        return t_done();
    }
    if (status("guestcore", &st) > 0) {
        t_check("initial_unloaded", 0, "guestcore already loaded");
        return t_done();
    }
    modpath("/tests/aux/mod.d");
    for (round = 0; round < 2; round++) {
        id = modload("guestcore");
        if (!t_check("load", id > 0 && modstat(id, &st, 0) == 0, "round %d: %s", round, T_ERR))
            return t_done();
        t_check("hooks_loaded", hookvalues(&st), "round %d", round);
        t_check("unload", moduload(id) == 0, "round %d: %s", round, T_ERR);
        t_check("hooks_removed", hookvalues((struct modstatus *)0), "round %d", round);
        t_check("native_after_unload", getpid() > 0, "round %d", round);
    }
    if (access("/aux/bin/sleep", 0) < 0) {
        t_skip("busy", "no A/UX sleep");
        return t_done();
    }
    strcpy(reg.md_modname, "auxexec");
    reg.md_typedata = (caddr_t)&er;
    er.er_magic = 0x150;
    er.er_flags = EXF_FIRST;
    if (!t_check("register", modadm(MOD_TY_EXEC, MOD_C_MREG, &reg) == 0, "%s", T_ERR))
        return t_done();
    pid = fork();
    if (pid == 0) {
        execl("/aux/bin/sleep", "sleep", "30", (char *)0);
        _exit(127);
    }
    if (!t_check("fork", pid > 0, "%s", T_ERR))
        return t_done();
    end = t_now_ms() + 10000;
    while (t_now_ms() < end) {
        if ((aid = status("auxcore", &st)) > 0 && st.ms_refcnt > 0) {
            busy = 1;
            break;
        }
        poll((struct pollfd *)0, 0L, 10);
    }
    id = status("guestcore", &core);
    t_check("guest_attached", busy && id > 0, "guest did not attach");
    if (busy && id > 0) {
        t_check("profile_busy", moduload(aid) < 0 && errno == EBUSY, "%s", T_ERR);
        t_check("core_busy", moduload(id) < 0 && errno == EBUSY, "%s", T_ERR);
        t_check("busy_hooks_live", hookvalues(&core), "hooks lost during refused unload");
    }
    kill(pid, SIGKILL);
    waitpid(pid, &waitst, 0);
    xid = status("auxexec", &st);
    aid = status("auxcore", &st);
    if (xid > 0)
        t_check("exec_unload", moduload(xid) == 0, "%s", T_ERR);
    if (aid > 0)
        t_check("profile_unload", moduload(aid) == 0, "%s", T_ERR);
    if (id > 0)
        t_check("core_unload", moduload(id) == 0 && hookvalues((struct modstatus *)0), "%s", T_ERR);
    return t_done();
}
