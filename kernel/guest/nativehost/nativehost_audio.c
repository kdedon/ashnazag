#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stropts.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include "sndio.h"
#include "nativehost_audio.h"

#define RATE 22050U
#define BLOCK 1024

static int
transfer(int fd, void *buffer, unsigned int count, int writing)
{
    unsigned int done = 0;
    int n;

    while (done < count) {
        if (writing)
            n = write(fd, (char *)buffer + done, count - done);
        else
            n = read(fd, (char *)buffer + done, count - done);
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
record(int fd, unsigned long command, void *buffer, unsigned int count)
{
    struct sndrec r;

    memset(&r, 0, sizeof r);
    r.r_cmd = command;
    r.r_len = count;
    if (transfer(fd, &r, sizeof r, 1) < 0)
        return -1;
    return transfer(fd, buffer, count, 1);
}

static int
reply(int fd, unsigned long command, void *buffer, unsigned int count)
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
play(unsigned int hz, unsigned int ms)
{
    unsigned char samples[BLOCK];
    struct sndfmt format;
    struct sndstat status;
    unsigned int left, n, i, phase = 0;
    int fd, saved = 0;
    long available;

    fd = open(SNDPATH, O_RDWR);
    if (fd < 0)
        return errno;
    /* Reject a stale regular file beneath the mounted service. */
    if (ioctl(fd, I_NREAD, &available) < 0)
        goto failed;
    memset(&format, 0, sizeof format);
    format.f_rate = (unsigned long)RATE << 16;
    format.f_enc = SNDE_U8;
    format.f_chans = 1;
    if (record(fd, SNDR_FMT, &format, sizeof format) < 0)
        goto failed;
    left = (RATE * ms + 999) / 1000;
    while (left) {
        n = left < BLOCK ? left : BLOCK;
        for (i = 0; i < n; i++) {
            samples[i] = phase < RATE / 2 ? 160 : 96;
            phase += hz;
            if (phase >= RATE)
                phase -= RATE;
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
    saved = errno;
    close(fd);
    return saved ? saved : EIO;
}

int
nh_audio_tone(unsigned int hz, unsigned int ms)
{
    struct tms accounting;
    struct pollfd p;
    clock_t start, now;
    unsigned long budget, elapsed;
    long ticks;
    int pipefd[2], result = EIO, n, status, saved;
    pid_t child, reaped;

    if (hz < 20 || hz > 10000 || !ms || ms > 2000) {
        errno = EINVAL;
        return -1;
    }
    ticks = sysconf(_SC_CLK_TCK);
    start = times(&accounting);
    if (ticks <= 0 || ticks > 1000000 || start == (clock_t)-1) {
        errno = EIO;
        return -1;
    }
    budget = ((unsigned long)ticks * (ms + 2000) + 999) / 1000;
    if (pipe(pipefd) < 0)
        return -1;
    child = fork();
    if (child < 0) {
        saved = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        errno = saved;
        return -1;
    }
    if (!child) {
        close(pipefd[0]);
        signal(SIGPIPE, SIG_IGN);
        result = play(hz, ms);
        transfer(pipefd[1], &result, sizeof result, 1);
        close(pipefd[1]);
        _exit(0);
    }
    close(pipefd[1]);
    p.fd = pipefd[0];
    p.events = POLLIN;
    /* The worker bounds open, ioctl, stream I/O and close together. */
    for (;;) {
        now = times(&accounting);
        if (now == (clock_t)-1) {
            result = EIO;
            break;
        }
        elapsed = (unsigned long)now - (unsigned long)start;
        if (elapsed >= budget) {
            result = ETIMEDOUT;
            break;
        }
        p.revents = 0;
        n = poll(&p, 1, 20);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            result = errno;
            break;
        }
        if (p.revents & POLLIN) {
            n = read(pipefd[0], &result, sizeof result);
            if (n != sizeof result)
                result = EIO;
            break;
        }
        if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            result = EIO;
            break;
        }
    }
    close(pipefd[0]);
    /* Reaping releases every worker descriptor, including a stalled channel. */
    kill(child, SIGKILL);
    do {
        reaped = waitpid(child, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    if (reaped < 0 && !result)
        result = errno;
    if (result) {
        errno = result;
        return -1;
    }
    return 0;
}
