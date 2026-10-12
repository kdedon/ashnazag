/*
 * t_quake.c -- software Quake on the display service: "timedemo demo1"
 * from the volume on SCSI disk 5 (tests/quake/mkvol.sh), screen dumps
 * while it plays, the frame rate as INFO.  Skips without the volume.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/termio.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

#define DEV	"/dev/dsk/c5d0s0"
#define MNT	"/quake"
#define LOG	"/tmp/quake.log"
#define SECS	6600

static int hfd = -1;
static int hseq;
static char hrep[128];

/* a host request (tests/display/hostio.py); the answer, 0 if none in 20 s */
static char *
host(char *req)
{
	char buf[160], line[256], *s;
	struct pollfd p;
	int n, len = 0, seq;
	long t0;

	if (hfd < 0)
		return 0;
	sprintf(buf, "@@ %d %s\n", ++hseq, req);
	write(hfd, buf, strlen(buf));
	t0 = t_now_ms();
	p.fd = hfd;
	p.events = POLLIN;
	while (t_now_ms() - t0 < 20000) {
		if (poll(&p, 1, 500) <= 0 || (n = read(hfd, line + len, sizeof line - 1 - len)) <= 0)
			continue;
		len += n;
		line[len] = 0;
		if ((s = strstr(line, "@@ok ")) != 0 && strchr(s, '\n') &&
		    sscanf(s + 5, "%d", &seq) == 1 && seq == hseq) {
			s = strchr(s + 5, ' ');
			strncpy(hrep, s ? s + 1 : "", sizeof hrep - 1);
			hrep[strcspn(hrep, "\r\n")] = 0;
			return hrep;
		}
		if (strchr(line, '\n') && strstr(line, "@@err "))
			return 0;
		if (strchr(line, '\n'))
			len = 0;
	}
	return 0;
}

static void
hostopen()
{
	struct termio t;

	if ((hfd = open("/dev/term/b", O_RDWR | O_NOCTTY)) < 0)
		return;
	if (ioctl(hfd, TCGETA, &t) == 0) {
		t.c_iflag = IGNCR;
		t.c_oflag = 0;
		t.c_lflag = ICANON;
		t.c_cflag |= CREAD | CLOCAL;
		ioctl(hfd, TCSETAF, &t);
	}
	if (host("ping") == 0 || strcmp(hrep, "pong") != 0) {
		t_info("host", "no answer on /dev/term/b");
		close(hfd);
		hfd = -1;
	}
}

/* the last 16 KB of the log, 0 terminated */
static char *
readlog()
{
	static char buf[16384];
	int fd, n;

	buf[0] = 0;
	if ((fd = open(LOG, O_RDONLY)) < 0)
		return buf;
	if ((n = lseek(fd, 0L, SEEK_END)) > (int)sizeof buf - 1)
		lseek(fd, (long)(n - (sizeof buf - 1)), SEEK_SET);
	else
		lseek(fd, 0L, SEEK_SET);
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	return buf;
}

/* a dump of the screen and how many of its pixels are not black */
static void
shot(char *name)
{
	char req[64];
	long lit;

	sprintf(req, "shot %s", name);
	if (host(req) == 0)
		return;
	sprintf(req, "lit %s", name);
	if (host(req) == 0)
		return;
	lit = atol(hrep);
	t_check(name, lit > 20000, "%ld pixels lit", lit);
}

int
main()
{
	char *log, *s;
	long t0;
	int status, frames = 0, nshot = 0, playing = 0;
	float secs = 0, fps = 0;
	pid_t pid;

	t_init("quake", SECS + 300);
	if (access(DEV, 0) != 0 || system("/sbin/mount -F ufs -r " DEV " " MNT) != 0) {
		t_skip("quake", "no Quake volume");
		return t_done();
	}
	if (access(MNT "/quake", X_OK) != 0 || access(MNT "/id1/pak0.pak", R_OK) != 0) {
		t_skip("quake", "no quake or pak0.pak on the volume");
		system("/sbin/umount " MNT);
		return t_done();
	}
	hostopen();
	unlink(LOG);
	if ((pid = fork()) == 0) {
		int fd = open(LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		close(0);
		open("/dev/null", O_RDONLY);
		dup2(fd, 1);
		dup2(fd, 2);
		execl(MNT "/quake", "quake", "-basedir", MNT, "-nomouse", "-tdquit",
		    "+timedemo", "demo1", (char *)0);
		_exit(127);
	}
	t0 = t_now_ms();
	/* dumps 10 s into the demo, then every 10 minutes */
	for (;;) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			break;
		if (t_now_ms() - t0 > SECS * 1000L) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			t_fail("timedemo", "no result in %d s", SECS);
			break;
		}
		sleep(5);
		if (!playing && strstr(readlog(), "Playing demo"))
			playing = t_now_ms();
		if (playing && nshot < 4 && t_now_ms() - playing >= 10000 + nshot * 600000L) {
			char name[32];

			sprintf(name, "quake_demo1_%d", ++nshot);
			shot(name);
		}
	}
	log = readlog();
	if ((s = strstr(log, " frames ")) != 0) {
		while (s > log && s[-1] >= '0' && s[-1] <= '9')
			s--;
		sscanf(s, "%d frames %f seconds %f fps", &frames, &secs, &fps);
	}
	if (t_check("timedemo", frames > 900 && fps > 0, "no result; log ends: %s",
	    strlen(log) > 200 ? log + strlen(log) - 200 : log))
		t_info("fps", "timedemo demo1: %d frames %.1f s %.2f fps", frames, secs, fps);
	system("/sbin/umount " MNT);
	return t_done();
}
