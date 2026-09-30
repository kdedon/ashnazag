/*
 * t_sig.c -- signal delivery, masks, handlers, restart, jumps.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <setjmp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

static volatile int hits, lastsig, chld;
static int wpipe = -1;
static sigjmp_buf jb;

static void h_count(sig) int sig; { hits++; lastsig = sig; }
static void h_chld(sig) int sig; { chld++; }
static void h_wake(sig) int sig; { hits++; if (wpipe >= 0) write(wpipe, "w", 1); }
static void h_jump(sig) int sig; { siglongjmp(jb, sig); }
static ucontext_t uc;
static volatile int uc_back;
static void h_setctx(sig) int sig; { uc_back = 1; setcontext(&uc); }

static void
act(sig, fn, flags)
int sig;
void (*fn)();
int flags;
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = fn;
	sa.sa_flags = flags;
	sigemptyset(&sa.sa_mask);
	sigaction(sig, &sa, (struct sigaction *)0);
}

static void
test_basic()
{
	struct sigaction old;
	sigset_t set, pend;

	hits = 0;
	signal(SIGUSR1, h_count);
	kill(getpid(), SIGUSR1);
	t_check("signal_kill_self", hits == 1 && lastsig == SIGUSR1, "hits %d", hits);

	hits = 0;
	act(SIGUSR2, h_count, 0);
	kill(getpid(), SIGUSR2);
	kill(getpid(), SIGUSR2);
	t_check("sigaction_persistent", hits == 2, "hits %d", hits);

	act(SIGUSR2, h_count, SA_RESETHAND);
	kill(getpid(), SIGUSR2);
	sigaction(SIGUSR2, (struct sigaction *)0, &old);
	t_check("SA_RESETHAND", old.sa_handler == SIG_DFL, "handler not reset");

	/* blocked signal stays pending until unblocked */
	hits = 0;
	act(SIGUSR1, h_count, 0);
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &set, (sigset_t *)0);
	kill(getpid(), SIGUSR1);
	sigpending(&pend);
	t_check("sigprocmask_blocks", hits == 0 && sigismember(&pend, SIGUSR1), "hits %d", hits);
	sigprocmask(SIG_UNBLOCK, &set, (sigset_t *)0);
	t_check("sigprocmask_unblock_delivers", hits == 1, "hits %d", hits);

	signal(SIGUSR1, SIG_IGN);
	kill(getpid(), SIGUSR1);
	t_pass("sig_ign");

	t_check("kill_0_exists", kill(getpid(), 0) == 0, "%s", T_ERR);
	t_check("kill_ESRCH", kill(30000, SIGUSR1) == -1 && errno == ESRCH, "errno %d", errno);
	t_check("kill_EINVAL", kill(getpid(), 99) == -1 && errno == EINVAL, "errno %d", errno);
}

static void
test_alarm()
{
	long t0, dt;

	hits = 0;
	act(SIGALRM, h_count, 0);
	/* the alarm counts down at each whole second: alarm(2) fires after 1 to 2 s */
	t0 = t_now_ms();
	alarm(2);
	pause();
	dt = t_now_ms() - t0;
	t_check("alarm_pause", hits == 1 && lastsig == SIGALRM && dt >= 900 && dt <= 2500,
	    "hits %d after %ld ms", hits, dt);
	alarm(10);
	t_check("alarm_remaining", alarm(0) >= 9, "previous alarm not reported");
	t_rearm(60);
}

static void
test_sigchld()
{
	pid_t pid;
	int st, i;
	sigset_t set, old, none;

	chld = 0;
	act(SIGCHLD, h_chld, 0);
	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	sigprocmask(SIG_BLOCK, &set, &old);
	pid = fork();
	if (pid == 0)
		_exit(3);
	sigemptyset(&none);
	alarm(5);
	act(SIGALRM, h_count, 0);
	while (chld == 0)
		sigsuspend(&none);
	alarm(0);
	sigprocmask(SIG_SETMASK, &old, (sigset_t *)0);
	waitpid(pid, &st, 0);
	t_check("sigchld_sigsuspend", chld == 1 && WEXITSTATUS(st) == 3, "chld %d status 0x%x", chld, st);

	/* SIGCHLD ignored: children are reaped by the system */
	signal(SIGCHLD, SIG_IGN);
	for (i = 0; i < 3; i++)
		if (fork() == 0)
			_exit(0);
	poll((struct pollfd *)0, 0, 300);
	errno = 0;
	t_check("sigchld_ignore_noZombie", wait(&st) == -1 && errno == ECHILD, "wait: errno %d", errno);
	signal(SIGCHLD, SIG_DFL);
	t_rearm(60);
}

static void
test_eintr_restart()
{
	int p[2], k;
	char c;
	long t0;

	pipe(p);
	hits = 0;
	act(SIGALRM, h_count, 0);
	alarm(1);
	t0 = t_now_ms();
	k = read(p[0], &c, 1);
	t_check("read_EINTR", k == -1 && errno == EINTR && hits == 1, "read %d errno %d after %ld ms",
	    k, errno, t_now_ms() - t0);

	/* SA_RESTART: handler feeds the pipe; the restarted read returns it */
	hits = 0;
	wpipe = p[1];
	act(SIGALRM, h_wake, SA_RESTART);
	alarm(1);
	k = read(p[0], &c, 1);
	t_check("read_SA_RESTART", k == 1 && c == 'w' && hits == 1, "read %d errno %d hits %d", k, errno, hits);
	wpipe = -1;

	/* wait() interrupted */
	hits = 0;
	act(SIGALRM, h_count, 0);
	if (fork() == 0) {
		poll((struct pollfd *)0, 0, 3000);
		_exit(0);
	}
	alarm(1);
	k = wait((int *)0);
	t_check("wait_EINTR", k == -1 && errno == EINTR, "wait %d errno %d", k, errno);
	wait((int *)0);
	close(p[0]);
	close(p[1]);
	t_rearm(60);
}

static void
test_other_process()
{
	pid_t pid;
	int st, p[2];
	char c;

	pid = fork();
	if (pid == 0) {
		for (;;)
			pause();
	}
	poll((struct pollfd *)0, 0, 100);
	kill(pid, SIGTERM);
	t_waitchild(pid, &st, 10);
	t_check("kill_child_SIGTERM", WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "status 0x%x", st);

	/* handler in the child, then exit with count */
	pipe(p);
	pid = fork();
	if (pid == 0) {
		hits = 0;
		act(SIGUSR1, h_count, 0);
		write(p[1], "r", 1);
		while (hits < 5)
			pause();
		_exit(hits);
	}
	read(p[0], &c, 1);
	{
		int i;
		for (i = 0; i < 5; i++) {
			kill(pid, SIGUSR1);
			poll((struct pollfd *)0, 0, 50);
		}
	}
	t_waitchild(pid, &st, 10);
	t_check("kill_child_handler", WIFEXITED(st) && WEXITSTATUS(st) == 5, "status 0x%x", st);

	/* stop / continue */
	pid = fork();
	if (pid == 0) {
		for (;;)
			pause();
	}
	kill(pid, SIGSTOP);
	st = 0;
	waitpid(pid, &st, WUNTRACED);
	t_check("sigstop_wuntraced", WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "status 0x%x", st);
	kill(pid, SIGCONT);
	kill(pid, SIGKILL);
	t_waitchild(pid, &st, 10);
	t_check("sigkill", WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "status 0x%x", st);

	/* process group */
	pid = fork();
	if (pid == 0) {
		setpgid(0, 0);
		if (fork() == 0) {
			for (;;)
				pause();
		}
		for (;;)
			pause();
	}
	poll((struct pollfd *)0, 0, 200);
	t_check("kill_pgrp", kill(-pid, SIGTERM) == 0, "%s", T_ERR);
	t_waitchild(pid, &st, 10);
	t_check("kill_pgrp_leader", WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "status 0x%x", st);
	close(p[0]);
	close(p[1]);
}

static int
div0(n)
int n;
{
	volatile int z = n - n;
	return n / z;
}

static void
test_faults()
{
	pid_t pid;
	int st;

	pid = fork();
	if (pid == 0)
		_exit(div0(5));
	t_waitchild(pid, &st, 10);
	t_check("SIGFPE_div0", WIFSIGNALED(st) && WTERMSIG(st) == SIGFPE, "status 0x%x", st);

	pid = fork();
	if (pid == 0) {
		__asm__ volatile (".word 0x4afc");	/* ILLEGAL */
		_exit(0);
	}
	t_waitchild(pid, &st, 10);
	t_check("SIGILL", WIFSIGNALED(st) && WTERMSIG(st) == SIGILL, "status 0x%x", st);

	pid = fork();
	if (pid == 0) {
		__asm__ volatile ("trap &7");
		_exit(0);
	}
	t_waitchild(pid, &st, 10);
	t_info("trap7", "status 0x%x", st);
}

static void
test_jumps()
{
	volatile int r;
	sigset_t cur;
	char *m;
	int fd;

	/*
	 * AMIX libc's sigsetjmp always clears UC_SIGMASK, so siglongjmp keeps
	 * the handler's mask; the kernel side is checked with setcontext below.
	 */
	act(SIGUSR1, h_jump, 0);
	r = sigsetjmp(jb, 1);
	if (r == 0) {
		kill(getpid(), SIGUSR1);
		t_fail("siglongjmp_handler", "handler returned");
	} else {
		sigprocmask(SIG_BLOCK, (sigset_t *)0, &cur);
		t_check("siglongjmp_handler", r == SIGUSR1, "value %d", r);
		t_info("siglongjmp_mask", "SIGUSR1 %s after siglongjmp, sigsetjmp saved UC_SIGMASK %s",
		    sigismember(&cur, SIGUSR1) ? "blocked" : "unblocked",
		    ((ucontext_t *)jb)->uc_flags & UC_SIGMASK ? "set" : "clear");
		sigprocmask(SIG_UNBLOCK, &cur, (sigset_t *)0);
	}

	/* setcontext from a handler restores the mask saved by getcontext */
	uc_back = 0;
	act(SIGUSR1, h_setctx, 0);
	getcontext(&uc);
	if (!uc_back) {
		kill(getpid(), SIGUSR1);
		t_fail("setcontext_handler", "handler returned");
	} else {
		sigprocmask(SIG_BLOCK, (sigset_t *)0, &cur);
		t_check("setcontext_handler", !sigismember(&cur, SIGUSR1), "SIGUSR1 still blocked");
		sigprocmask(SIG_UNBLOCK, &cur, (sigset_t *)0);
	}

	/* recover from a fault on a read-only page */
	fd = open("/dev/zero", O_RDWR);
	m = mmap((caddr_t)0, 4096, PROT_READ, MAP_PRIVATE, fd, (off_t)0);
	close(fd);
	act(SIGSEGV, h_jump, 0);
	act(SIGBUS, h_jump, 0);
	r = sigsetjmp(jb, 1);
	if (r == 0) {
		*(volatile char *)m = 1;
		t_fail("siglongjmp_fault", "store to read-only page did not fault");
	} else {
		t_pass("siglongjmp_fault");
		r = sigsetjmp(jb, 1);
		if (r == 0) {
			*(volatile char *)(m + 8) = 1;
			t_fail("fault_twice", "second store did not fault");
		} else
			t_pass("fault_twice");
	}
	signal(SIGSEGV, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
	munmap(m, 4096);
}

int
main()
{
	t_init("sig", 60);
	test_basic();
	test_alarm();
	test_sigchld();
	test_eintr_restart();
	test_other_process();
	test_faults();
	test_jumps();
	return t_done();
}
