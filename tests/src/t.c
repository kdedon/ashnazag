/*
 * t.c -- result reporting, watchdog, helpers.
 */
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

char *t_area = "?";
int t_nfail;
static int t_wdsecs;
static pid_t t_mainpid;

static void
t_out(kind, name, fmt, ap)
char *kind, *name, *fmt;
va_list ap;
{
	char buf[512];
	int n, fd;

	sprintf(buf, "%s %s.%s", kind, t_area, name);
	n = strlen(buf);
	if (fmt != 0) {
		strcpy(buf + n, ": ");
		n += 2;
		vsprintf(buf + n, fmt, ap);
		n = strlen(buf);
	}
	buf[n++] = '\n';
	write(1, buf, n);
	if (kind[0] == 'P' || kind[0] == 'F' || kind[0] == 'S') {
		fd = open(COUNTFILE, O_WRONLY | O_APPEND | O_CREAT, 0666);
		if (fd >= 0) {
			write(fd, kind, 1);
			close(fd);
		}
	}
}

static void
t_watchdog(sig)
int sig;
{
	char buf[128];

	if (getpid() != t_mainpid)
		_exit(99);
	sprintf(buf, "FAIL %s.watchdog: no progress in %d s\n", t_area, t_wdsecs);
	write(1, buf, strlen(buf));
	{
		int fd = open(COUNTFILE, O_WRONLY | O_APPEND | O_CREAT, 0666);
		if (fd >= 0) {
			write(fd, "F", 1);
			close(fd);
		}
	}
	_exit(1);
}

void
t_init(char *area, int secs)
{
	struct rlimit rl;

	/* children killed on purpose must not fill the small root with core files */
	rl.rlim_cur = rl.rlim_max = 0;
	setrlimit(RLIMIT_CORE, &rl);
	t_area = area;
	t_mainpid = getpid();
	signal(SIGALRM, t_watchdog);
	t_rearm(secs);
}

void
t_rearm(int secs)
{
	t_wdsecs = secs;
	signal(SIGALRM, t_watchdog);
	alarm(secs);
}

void
t_pass(char *name)
{
	t_out("PASS", name, (char *)0, (va_list)0);
}

void
t_fail(char *name, char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	t_out("FAIL", name, fmt, ap);
	va_end(ap);
	t_nfail++;
}

void
t_skip(char *name, char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	t_out("SKIP", name, fmt, ap);
	va_end(ap);
}

void
t_info(char *name, char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	t_out("INFO", name, fmt, ap);
	va_end(ap);
}

int
t_check(char *name, int ok, char *fmt, ...)
{
	va_list ap;

	if (ok) {
		t_pass(name);
		return 1;
	}
	va_start(ap, fmt);
	t_out("FAIL", name, fmt, ap);
	va_end(ap);
	t_nfail++;
	return 0;
}

int
t_done(void)
{
	alarm(0);
	return t_nfail ? 1 : 0;
}

static int t_timedout;

static void
t_alrm(sig)
int sig;
{
	t_timedout = 1;
}

/*
 * waitpid with a timeout; kills the child with SIGKILL on expiry.
 * Returns the pid, or -1 (errno ETIME on timeout).  Restores the watchdog.
 */
int
t_waitchild(pid_t pid, int *status, int secs)
{
	int r, saved = t_wdsecs;

	t_timedout = 0;
	signal(SIGALRM, t_alrm);
	alarm(secs);
	for (;;) {
		r = waitpid(pid, status, 0);
		if (r >= 0 || errno != EINTR)
			break;
		if (t_timedout) {
			kill(pid, SIGKILL);
			waitpid(pid, status, 0);
			errno = ETIME;
			r = -1;
			break;
		}
	}
	t_rearm(saved);
	return r;
}

int
t_pattern(long off, int seed)
{
	return (int)((off * 131 + (off >> 9) * 7 + seed) & 0xff);
}

int
t_fill(int fd, int n, int seed)
{
	char buf[1024];
	long off = 0;
	int k, i, w;

	while (off < n) {
		k = n - off > (long)sizeof buf ? sizeof buf : n - off;
		for (i = 0; i < k; i++)
			buf[i] = t_pattern(off + i, seed);
		w = write(fd, buf, k);
		if (w != k)
			return off + (w > 0 ? w : 0);
		off += k;
	}
	return off;
}

/* /tests/ksyms lines: "name hexaddr" */
long
t_kmem(char *sym)
{
	FILE *f;
	char name[64], *plus;
	unsigned long addr;
	long val, off = 0;
	int fd, found = 0, len;

	/* "sym+off": a field inside a kernel structure */
	plus = strchr(sym, '+');
	len = plus ? plus - sym : strlen(sym);
	if (plus)
		off = strtol(plus + 1, (char **)0, 0);
	f = fopen("/tests/ksyms", "r");
	if (f == 0)
		return -1;
	while (fscanf(f, "%63s %lx", name, &addr) == 2)
		if (strncmp(name, sym, len) == 0 && name[len] == 0) {
			found = 1;
			break;
		}
	fclose(f);
	if (!found)
		return -1;
	fd = open("/dev/kmem", O_RDONLY);
	if (fd < 0)
		return -1;
	if (lseek(fd, (off_t)(addr + off), 0) == -1 || read(fd, (char *)&val, sizeof val) != sizeof val)
		val = -1;
	close(fd);
	return val;
}

long
t_now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, (void *)0);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}
