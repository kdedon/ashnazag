#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stropts.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include "sndio.h"
#include "lisa_audio.h"
#include "lisa_wire.h"

#define RATE 22050UL

static int
transfer(fd, buffer, count, writing)
int fd, writing;
void *buffer;
unsigned int count;
{
	unsigned int done = 0;
	int n;

	while (done < count) {
		n = writing ? write(fd, (char *)buffer + done, count - done) :
			read(fd, (char *)buffer + done, count - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (!n)
				errno = EIO;
			return -1;
		}
		done += n;
	}
	return 0;
}

static int
record(fd, command, buffer, count)
int fd;
unsigned long command;
void *buffer;
unsigned int count;
{
	struct sndrec r;

	r.r_cmd = command;
	r.r_len = count;
	if (transfer(fd, &r, sizeof r, 1) < 0)
		return -1;
	return transfer(fd, buffer, count, 1);
}

static int
reply(fd, command, buffer, count)
int fd;
unsigned long command;
void *buffer;
unsigned int count;
{
	struct sndrec r;

	if (transfer(fd, &r, sizeof r, 0) < 0)
		return -1;
	if (r.r_cmd != command || r.r_len != count) {
		errno = EIO;
		return -1;
	}
	return transfer(fd, buffer, count, 0);
}

static int
play(period, ms, gain)
unsigned long period, ms, gain;
{
	unsigned char samples[1024];
	struct sndfmt format;
	struct sndstat status;
	unsigned long left, phase = 0, cycle, n, i;
	long available;
	int fd, result;

	fd = open(SNDPATH, O_RDWR);
	if (fd < 0)
		return errno;
	if (ioctl(fd, I_NREAD, &available) < 0)
		goto failed;
	memset(&format, 0, sizeof format);
	format.f_rate = RATE << 16;
	format.f_enc = SNDE_U8;
	format.f_chans = 1;
	if (record(fd, SNDR_FMT, &format, sizeof format) < 0)
		goto failed;
	left = (RATE * ms + 999) / 1000;
	cycle = RATE * period;
	while (left) {
		n = left < sizeof samples ? left : sizeof samples;
		for (i = 0; i < n; i++) {
			samples[i] = phase < cycle / 2 ? 128 + gain * 8 : 128 - gain * 8;
			phase += 1000000;
			if (phase >= cycle)
				phase -= cycle;
		}
		if (record(fd, SNDR_PCM, samples, n) < 0)
			goto failed;
		left -= n;
	}
	if (record(fd, SNDR_DRAIN, 0, 0) < 0 ||
		reply(fd, SNDR_DRAIN, 0, 0) < 0 ||
		record(fd, SNDR_STAT, 0, 0) < 0 ||
		reply(fd, SNDR_STAT, &status, sizeof status) < 0)
		goto failed;
	if (!status.ss_heard || status.ss_muted || status.ss_queued ||
		status.ss_sess < 0 || status.ss_sess != status.ss_front) {
		errno = EIO;
		goto failed;
	}
	if (close(fd) < 0)
		return errno;
	return 0;
failed:
	result = errno ? errno : EIO;
	close(fd);
	return result;
}

int
lisa_audio_stop(h)
struct lisa_host *h;
{
	pid_t child;
	int status, saved = 0;

	if (h->audio_pid) {
		if (kill(h->audio_pid, SIGKILL) < 0 && errno != ESRCH)
			saved = errno;
		do {
			child = waitpid(h->audio_pid, &status, 0);
		} while (child < 0 && errno == EINTR);
		if (child < 0 && !saved)
			saved = errno;
		close(h->audio_fd);
		h->audio_fd = -1;
		h->audio_pid = 0;
	}
	if (saved) {
		errno = saved;
		return -1;
	}
	return 0;
}

int
lisa_audio_start(h, period, ms, gain)
struct lisa_host *h;
unsigned long period, ms, gain;
{
	struct nh_clock clock;
	sigset_t mask;
	int fds[2], result, saved, fd;
	long limit;
	pid_t child;

	if (!h || period < 100 || period > 50000 || !ms || ms > 2000 || gain > 7) {
		errno = EINVAL;
		return -1;
	}
	if (nh_clock_read(&clock) < 0 || lisa_audio_stop(h) < 0)
		return -1;
	if (pipe(fds) < 0)
		return -1;
	child = fork();
	if (child < 0) {
		saved = errno;
		close(fds[0]);
		close(fds[1]);
		errno = saved;
		return -1;
	}
	if (!child) {
		close(fds[0]);
		signal(SIGALRM, SIG_DFL);
		sigemptyset(&mask);
		sigaddset(&mask, SIGALRM);
		sigprocmask(SIG_UNBLOCK, &mask, 0);
		alarm((ms + 2999) / 1000);
		limit = sysconf(_SC_OPEN_MAX);
		if (limit < 0)
			_exit(1);
		for (fd = 3; fd < limit; fd++)
			if (fd != fds[1])
				close(fd);
		signal(SIGPIPE, SIG_IGN);
		result = play(period, ms, gain);
		transfer(fds[1], &result, sizeof result, 1);
		close(fds[1]);
		_exit(0);
	}
	close(fds[1]);
	h->audio_pid = child;
	h->audio_fd = fds[0];
	h->audio_started = clock.ticks;
	h->audio_budget = ((unsigned long)clock.frequency * (ms + 2000) + 999) / 1000;
	return 0;
}

int
lisa_host_audio_poll(h)
struct lisa_host *h;
{
	struct pollfd p;
	struct nh_clock clock;
	int n, result = EIO;

	if (!h)
		return -1;
	if (nh_clock_read(&clock) < 0) {
		result = errno;
		lisa_audio_stop(h);
		errno = result;
		return -1;
	}
	if (!h->audio_pid)
		return 0;
	if (clock.ticks - h->audio_started >= h->audio_budget) {
		lisa_audio_stop(h);
		errno = ETIMEDOUT;
		return -1;
	}
	p.fd = h->audio_fd;
	p.events = POLLIN;
	p.revents = 0;
	n = poll(&p, 1, 0);
	if (n < 0) {
		result = errno;
		lisa_audio_stop(h);
		errno = result;
		return -1;
	}
	if (!n)
		return 0;
	if (p.revents & POLLIN) {
		n = read(h->audio_fd, &result, sizeof result);
		if (n != sizeof result)
			result = EIO;
	}
	if (lisa_audio_stop(h) < 0 && !result)
		result = errno;
	if (result) {
		errno = result;
		return -1;
	}
	return 1;
}

int
lisa_host_tone(h, hz, ms)
struct lisa_host *h;
unsigned int hz, ms;
{
	if (hz < 20 || hz > 10000) {
		errno = EINVAL;
		return -1;
	}
	return lisa_audio_start(h, 1000000UL / hz, ms, 4);
}

int
lisa_host_audio(h, surface)
struct lisa_host *h;
unsigned char *surface;
{
	unsigned char request[36], *p;
	unsigned long gen, period, ms, gain, op;
	int n, result;

	p = surface + LISA_AUDIO;
	memcpy(request, p, sizeof request);
	lisa_barrier();
	gen = lisa_get32(request + 12);
	if (lisa_get32(request) != 0x4c415544 || lisa_get32(request + 4) != 2) {
		errno = EINVAL;
		return -1;
	}
	if (gen && gen != h->audio_generation) {
		if (gen != lisa_get32(p + 12))
			return 0;
		period = lisa_get32(request + 16);
		ms = lisa_get32(request + 20);
		gain = lisa_get32(request + 8);
		op = lisa_get32(request + 24);
		h->audio_generation = gen;
		n = lisa_audio_stop(h);
		if (!n && op == 1)
			n = lisa_audio_start(h, period, ms, gain);
		else if (!n && op != 2) {
			errno = EINVAL;
			n = -1;
		}
		if (n < 0 || op == 2) {
			result = n < 0 ? -errno : 0;
			lisa_put32(p + 28, result);
			lisa_barrier();
			lisa_put32(p + 32, gen);
			return n;
		}
	}
	if (!h->audio_pid)
		return 0;
	n = lisa_host_audio_poll(h);
	if (n) {
		result = n < 0 ? -errno : 0;
		lisa_put32(p + 28, result);
		lisa_barrier();
		lisa_put32(p + 32, h->audio_generation);
	}
	return n;
}
