#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "sys/mod.h"
#include "guest_native.h"
#include "nativehost.h"
#include "fixture.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
extern long ncf_private(void);
extern int modload(char *), moduload(int);
#ifdef __AMIX__
extern int munmap();
#endif
static volatile sig_atomic_t signals;
static void caught(int sig) { signals++; }

static int
wait_child(pid_t child, int *status)
{
	long deadline = t_now_ms() + 3000;
	int result;
	while (t_now_ms() < deadline) {
		result = waitpid(child, status, WNOHANG);
		if (result == child)
			return 1;
		if (result < 0)
			return 0;
		poll(0, 0, 10);
	}
	kill(child, SIGKILL);
	waitpid(child, status, 0);
	return 0;
}

int
main(int argc, char **argv)
{
	struct ncf_stats counts;
	struct mod_mreg registration;
	struct nh_clock clock;
	void *memory;
	long private_value, pid;
	int module, fd, error, i, status, unbalanced, major = 66;
	pid_t child;

	if (argc > 1 && !strcmp(argv[1], "execchild")) {
		memory = nh_memory(4096, 0);
		if (memory == (void *)-1 || getpid() <= 0)
			return 1;
		return munmap(memory, 4096) != 0;
	}
	t_init("nativecall", 30);
	if (access(MD "/nhcall", 0) < 0) {
		t_skip("all", "no nhcall module on this root");
		return t_done();
	}
	modpath(MD);
	memset(&registration, 0, sizeof registration);
	strcpy(registration.md_modname, "nhcall");
	registration.md_typedata = (caddr_t)&major;
	if (!t_check("profile_register", modadm(MOD_TY_CDEV, MOD_C_MREG,
		(caddr_t)&registration) == 0 || errno == EEXIST, "%s", T_ERR))
		return t_done();
	module = modload("nhcall");
	if (!t_check("profile_load", module >= 0, "%s", T_ERR))
		return t_done();
	fd = open("/dev/nhcall", O_RDWR);
	if (!t_check("device_open", fd >= 0, "%s", T_ERR))
		return t_done();
	pid = getpid();
	signal(SIGTERM, caught);
	error = ioctl(fd, NCF_ATTACH, 0);
	if (error < 0) {
		t_fail("attach", "%s", T_ERR);
		return t_done();
	}
	private_value = ncf_private();
	error = nh_native_begin();
	if (error)
		_exit(1);
	t_check("private_trap", private_value == NCF_VALUE, "value %lx", private_value);
	t_check("host_syscall", getpid() == pid, "native process id");
	t_check("depth_outer", nh_native_depth() == 1, "outer scope");
	for (i = 1; i < GNC_MAXDEPTH; i++)
		if (nh_native_begin())
			break;
	t_check("nest_limit", i == GNC_MAXDEPTH && nh_native_begin() == EBUSY &&
		nh_native_depth() == GNC_MAXDEPTH, "bounded nesting");
	for (i = 1; i < GNC_MAXDEPTH; i++)
		nh_native_end();
	t_check("depth_nested_exit", nh_native_depth() == 1, "outer scope retained");
	t_check("clock", nh_clock_read(&clock) == 0, "%s", T_ERR);
	memory = nh_memory(4096, 0);
	t_check("memory", memory != (void *)-1, "%s", T_ERR);
	if (memory != (void *)-1)
		t_check("memory_release", munmap(memory, 4096) == 0, "%s", T_ERR);
	t_check("signal_send", kill(getpid(), SIGTERM) == 0, "%s", T_ERR);
	poll(0, 0, 30);
	t_check("signal_native_frame", signals == 1, "signals %d", (int)signals);
	child = fork();
	if (child == 0) {
		long childpid = getpid();
		int good = nh_native_depth() == 1;
		good &= nh_native_end() == 0;
		good &= ncf_private() == NCF_VALUE;
		good &= nh_native_begin() == 0;
		good &= getpid() == childpid;
		_exit(good ? 0 : 1);
	}
	status = -1;
	t_check("fork_scope", child > 0 && wait_child(child, &status) &&
		WIFEXITED(status) && WEXITSTATUS(status) == 0, "status %x", status);
	child = fork();
	if (child == 0) {
		execl("/tests/t_nativecall", "t_nativecall", "execchild", (char *)0);
		_exit(2);
	}
	status = -1;
	t_check("exec_cleanup", child > 0 && wait_child(child, &status) &&
		WIFEXITED(status) && WEXITSTATUS(status) == 0, "status %x", status);
	child = fork();
	if (child == 0) {
		signal(SIGSEGV, SIG_DFL);
		signal(SIGBUS, SIG_DFL);
		memory = nh_memory(4096, 0);
		if (memory == (void *)-1 || munmap(memory, 4096))
			_exit(2);
		*(volatile long *)memory = 1;
		_exit(3);
	}
	status = -1;
	t_check("native_fault_termination", child > 0 && wait_child(child, &status) &&
		WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV ||
		WTERMSIG(status) == SIGBUS), "status %x", status);
	error = nh_native_end();
	private_value = ncf_private();
	unbalanced = nh_native_end();
	if (nh_native_begin())
		_exit(1);
	t_check("restored_private_trap", !error && private_value == NCF_VALUE,
		"value %lx", private_value);
	t_check("unbalanced_end", unbalanced == EINVAL, "error %d", unbalanced);
	memset(&counts, 0, sizeof counts);
	error = ioctl(fd, NCF_STATS, &counts);
	t_check("private_callbacks_guarded", error == 0 && !counts.bad_intr &&
		!counts.bad_signal && !counts.private_vur, "intr %lu signal %lu pointer %lu",
		counts.bad_intr, counts.bad_signal, counts.private_vur);
	t_check("lifetime_counts", counts.forks == 3 && counts.exits == 3 &&
		counts.private_traps == 3 && counts.private_intr >= 3,
		"fork %lu exit %lu trap %lu intr %lu", counts.forks, counts.exits,
		counts.private_traps, counts.private_intr);
	t_check("detach_active", ioctl(fd, NCF_DETACH, 0) == 0, "%s", T_ERR);
	t_check("device_close", close(fd) == 0, "%s", T_ERR);
	t_check("profile_unload", moduload(module) == 0, "%s", T_ERR);
	return t_done();
}
