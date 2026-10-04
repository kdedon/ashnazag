/*
 * t_pipe.c -- pipes, FIFOs, dup/dup2, fcntl, poll/select.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/select.h>
#include <stropts.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

#define BIG	(1024L * 1024)

/* child writes n pattern bytes in odd-sized chunks; parent verifies */
static void
transfer(name, rfd, wfd, n)
char *name;
int rfd, wfd;
long n;
{
	char buf[5000];
	long off = 0;
	pid_t pid;
	int k, i, st, bad = 0, chunk;

	pid = fork();
	if (pid == 0) {
		close(rfd);
		chunk = 1;
		while (off < n) {
			k = chunk;
			if (k > n - off)
				k = n - off;
			for (i = 0; i < k; i++)
				buf[i] = t_pattern(off + i, 17);
			if (write(wfd, buf, k) != k)
				_exit(2);
			off += k;
			chunk = chunk * 3 % 4999 + 1;
		}
		_exit(0);
	}
	close(wfd);
	while ((k = read(rfd, buf, sizeof buf)) > 0) {
		for (i = 0; i < k && !bad; i++)
			if (buf[i] != (char)t_pattern(off + i, 17))
				bad = 1;
		off += k;
	}
	close(rfd);
	t_waitchild(pid, &st, 60);
	t_check(name, off == n && !bad && st == 0, "received %ld of %ld, %s, writer status 0x%x",
	    off, n, bad ? "corrupt" : "intact", st);
}

static void
test_pipe()
{
	int p[2], k, n;
	char buf[64];
	pid_t pid;

	t_check("pipe", pipe(p) == 0, "%s", T_ERR);
	write(p[1], "hello", 5);
	k = read(p[0], buf, sizeof buf);
	t_check("pipe_small", k == 5 && memcmp(buf, "hello", 5) == 0, "read %d", k);
	/* SVR4 pipes are STREAMS and full duplex */
	k = write(p[0], "back", 4);
	n = read(p[1], buf, sizeof buf);
	t_check("pipe_duplex", k == 4 && n == 4 && memcmp(buf, "back", 4) == 0, "write %d read %d", k, n);
	write(p[1], "abc", 3);
	k = ioctl(p[0], I_NREAD, &n);
	t_check("pipe_I_NREAD", k == 1 && n == 3, "ioctl %d, bytes %d", k, n);
	read(p[0], buf, 3);

	fcntl(p[1], F_SETFL, O_NONBLOCK);
	n = 0;
	while ((k = write(p[1], buf, 64)) == 64 && n < 1000000)
		n += 64;
	t_check("pipe_full_EAGAIN", k == -1 && errno == EAGAIN, "write %d errno %d after %d bytes", k, errno, n);
	t_info("pipe_capacity", "%d bytes", n);
	fcntl(p[1], F_SETFL, 0);
	pid = fork();
	if (pid == 0) {
		_exit(write(p[1], buf, 64) == 64 ? 0 : 1);
	}
	{
		int st, total = 0;
		poll((struct pollfd *)0, 0, 300);
		fcntl(p[0], F_SETFL, O_NONBLOCK);
		while ((k = read(p[0], buf, sizeof buf)) > 0)
			total += k;
		fcntl(p[0], F_SETFL, 0);
		t_waitchild(pid, &st, 10);
		while (total < n + 64 && (k = read(p[0], buf, sizeof buf)) > 0)
			total += k;
		t_check("pipe_blocked_writer", st == 0 && total == n + 64, "status 0x%x total %d want %d",
		    st, total, n + 64);
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	k = read(p[0], buf, 1);
	t_check("pipe_empty_EAGAIN", k == -1 && errno == EAGAIN, "read %d errno %d", k, errno);
	fcntl(p[0], F_SETFL, O_NDELAY);
	k = read(p[0], buf, 1);
	t_check("pipe_empty_NDELAY_0", k == 0, "read %d errno %d", k, errno);
	close(p[1]);
	fcntl(p[0], F_SETFL, 0);
	t_check("pipe_eof", read(p[0], buf, 1) == 0, "read after writer close");
	close(p[0]);

	pipe(p);
	transfer("pipe_1MB", p[0], p[1], BIG);
	pipe(p);
	transfer("pipe_1MB_reverse", p[1], p[0], BIG);

	/* SIGPIPE / EPIPE */
	pipe(p);
	close(p[0]);
	pid = fork();
	if (pid == 0) {
		write(p[1], "x", 1);
		_exit(0);
	}
	{
		int st;
		t_waitchild(pid, &st, 10);
		t_check("sigpipe", WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE, "status 0x%x", st);
	}
	signal(SIGPIPE, SIG_IGN);
	k = write(p[1], "x", 1);
	t_check("epipe", k == -1 && errno == EPIPE, "write %d errno %d", k, errno);
	signal(SIGPIPE, SIG_DFL);
	close(p[1]);
}

static void
test_fifo()
{
	int fd, wfd;
	pid_t pid;

	unlink("/tmp/fifo");
	if (mkfifo("/tmp/fifo", 0666) == -1) {
		t_fail("mkfifo", "%s", T_ERR);
		return;
	}
	{
		struct stat sb;
		stat("/tmp/fifo", &sb);
		t_check("mkfifo", S_ISFIFO(sb.st_mode), "mode 0%o", (int)sb.st_mode);
	}
	fd = open("/tmp/fifo", O_WRONLY | O_NDELAY);
	t_check("fifo_wr_noreader_ENXIO", fd == -1 && errno == ENXIO, "open %d errno %d", fd, errno);
	if (fd >= 0)
		close(fd);
	fd = open("/tmp/fifo", O_RDONLY | O_NDELAY);
	t_check("fifo_rd_nowriter", fd >= 0, "%s", T_ERR);
	if (fd >= 0)
		close(fd);

	/* blocking open rendezvous, then 256 KB */
	pid = fork();
	if (pid == 0) {
		wfd = open("/tmp/fifo", O_WRONLY);
		_exit(t_fill(wfd, 256 * 1024, 4) == 256 * 1024 ? 0 : 1);
	}
	fd = open("/tmp/fifo", O_RDONLY);
	{
		char buf[3000];
		long off = 0;
		int k, i, bad = 0, st;
		while ((k = read(fd, buf, sizeof buf)) > 0) {
			for (i = 0; i < k; i++)
				if (buf[i] != (char)t_pattern(off + i, 4))
					bad++;
			off += k;
		}
		close(fd);
		t_waitchild(pid, &st, 30);
		t_check("fifo_256KB", off == 256 * 1024 && bad == 0 && st == 0,
		    "got %ld bytes, %d bad, writer 0x%x", off, bad, st);
	}
	unlink("/tmp/fifo");
}

static void
test_dup_fcntl()
{
	int fd, fd2, k, st;
	pid_t pid;
	char arg[16];

	fd = open("/dev/null", O_RDWR);
	close(10);
	fd2 = dup(fd);
	t_check("dup_lowest", fd2 == fd + 1 || fd2 < fd, "dup %d of %d", fd2, fd);
	close(fd2);
	t_check("dup2", dup2(fd, 10) == 10 && fcntl(10, F_GETFD) != -1, "%s", T_ERR);
	t_check("dup2_same", dup2(10, 10) == 10, "%s", T_ERR);
	k = fcntl(fd, F_DUPFD, 15);
	t_check("F_DUPFD", k == 15, "got %d", k);
	t_check("F_GETFL", (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR, "flags 0x%x", fcntl(fd, F_GETFL));
	fcntl(fd, F_SETFL, O_APPEND);
	t_check("F_SETFL", (fcntl(fd, F_GETFL) & O_APPEND) != 0, "flags 0x%x", fcntl(fd, F_GETFL));

	/* close-on-exec */
	fcntl(10, F_SETFD, 1);
	fcntl(15, F_SETFD, 0);
	pid = fork();
	if (pid == 0) {
		execl("/tests/xh_d0", "xh", "fdclosed", "10", (char *)0);
		_exit(126);
	}
	t_waitchild(pid, &st, 20);
	t_check("close_on_exec", st == 0, "fd 10 still open after exec (status 0x%x)", st);
	pid = fork();
	if (pid == 0) {
		sprintf(arg, "%d", 15);
		execl("/tests/xh_d0", "xh", "fdopen", arg, (char *)0);
		_exit(126);
	}
	t_waitchild(pid, &st, 20);
	t_check("inherit_on_exec", st == 0, "fd 15 closed after exec (status 0x%x)", st);
	close(10);
	close(15);
	close(fd);
	t_check("EBADF", fcntl(fd, F_GETFD) == -1 && errno == EBADF, "errno %d", errno);

	/* record locks */
	fd = open("/tmp/lockf", O_RDWR | O_CREAT | O_TRUNC, 0644);
	t_fill(fd, 1000, 1);
	{
		struct flock fl;
		int p[2];
		char c;
		fl.l_type = F_WRLCK;
		fl.l_whence = 0;
		fl.l_start = 100;
		fl.l_len = 50;
		t_check("F_SETLK", fcntl(fd, F_SETLK, &fl) == 0, "%s", T_ERR);
		pipe(p);
		pid = fork();
		if (pid == 0) {
			struct flock f2;
			int r = 0;
			f2.l_type = F_WRLCK;
			f2.l_whence = 0;
			f2.l_start = 120;
			f2.l_len = 10;
			if (fcntl(fd, F_SETLK, &f2) != -1 || (errno != EAGAIN && errno != EACCES))
				r |= 1;
			f2.l_start = 200;
			if (fcntl(fd, F_SETLK, &f2) == -1)
				r |= 2;
			f2.l_start = 120;
			f2.l_type = F_WRLCK;
			if (fcntl(fd, F_GETLK, &f2) == -1 || f2.l_type == F_UNLCK || f2.l_pid != getppid())
				r |= 4;
			write(p[1], "x", 1);
			f2.l_type = F_WRLCK;
			f2.l_start = 120;
			if (fcntl(fd, F_SETLKW, &f2) == -1)	/* waits for the parent's unlock */
				r |= 8;
			_exit(r);
		}
		read(p[0], &c, 1);
		poll((struct pollfd *)0, 0, 200);
		fl.l_type = F_UNLCK;
		fcntl(fd, F_SETLK, &fl);
		t_waitchild(pid, &st, 20);
		t_check("record_locks", st == 0, "child status 0x%x (1 conflict, 2 free range, 4 GETLK, 8 SETLKW)", st);
		close(p[0]);
		close(p[1]);
	}
	close(fd);
	unlink("/tmp/lockf");
}

static void
test_poll_select()
{
	int p[2], k;
	struct pollfd pf[2];
	long t0, dt;
	fd_set rs;
	struct timeval tv;

	pipe(p);
	pf[0].fd = p[0];
	pf[0].events = POLLIN;
	pf[0].revents = 0;
	k = poll(pf, 1, 0);
	t_check("poll_empty_0", k == 0, "poll %d revents 0x%x", k, pf[0].revents);
	t0 = t_now_ms();
	k = poll(pf, 1, 500);
	dt = t_now_ms() - t0;
	t_check("poll_timeout_500ms", k == 0 && dt >= 400 && dt <= 1500, "poll %d after %ld ms", k, dt);
	write(p[1], "x", 1);
	k = poll(pf, 1, 1000);
	t_check("poll_readable", k == 1 && (pf[0].revents & POLLIN), "poll %d revents 0x%x", k, pf[0].revents);
	pf[1].fd = p[1];
	pf[1].events = POLLOUT;
	k = poll(&pf[1], 1, 0);
	t_check("poll_writable", k == 1 && (pf[1].revents & POLLOUT), "poll %d revents 0x%x", k, pf[1].revents);

	FD_ZERO(&rs);
	FD_SET(p[0], &rs);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	k = select(p[0] + 1, &rs, (fd_set *)0, (fd_set *)0, &tv);
	t_check("select_readable", k == 1 && FD_ISSET(p[0], &rs), "select %d", k);
	read(p[0], (char *)&k, 1);
	FD_ZERO(&rs);
	FD_SET(p[0], &rs);
	tv.tv_sec = 0;
	tv.tv_usec = 300000;
	t0 = t_now_ms();
	k = select(p[0] + 1, &rs, (fd_set *)0, (fd_set *)0, &tv);
	dt = t_now_ms() - t0;
	t_check("select_timeout_300ms", k == 0 && dt >= 200 && dt <= 1300, "select %d after %ld ms", k, dt);

	/* wakeup from another process */
	if (fork() == 0) {
		poll((struct pollfd *)0, 0, 300);
		write(p[1], "y", 1);
		_exit(0);
	}
	t0 = t_now_ms();
	pf[0].revents = 0;
	k = poll(pf, 1, 5000);
	dt = t_now_ms() - t0;
	t_check("poll_wakeup", k == 1 && (pf[0].revents & POLLIN) && dt < 3000, "poll %d after %ld ms", k, dt);
	wait((int *)0);
	close(p[1]);
	read(p[0], (char *)&k, 1);
	pf[0].revents = 0;
	k = poll(pf, 1, 1000);
	t_check("poll_hangup", k == 1 && (pf[0].revents & POLLHUP), "poll %d revents 0x%x", k, pf[0].revents);
	close(p[0]);
	pf[0].fd = 99;
	k = poll(pf, 1, 0);
	t_check("poll_POLLNVAL", k == 1 && (pf[0].revents & POLLNVAL), "poll %d revents 0x%x", k, pf[0].revents);
}

int
main()
{
	t_init("pipe", 80);
	test_pipe();
	test_fifo();
	test_dup_fcntl();
	test_poll_select();
	return t_done();
}
