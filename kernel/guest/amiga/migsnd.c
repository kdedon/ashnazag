/*
 * migsnd.c -- the sound helper: the AHI driver's ring to the host's
 * sound service, fed a lead ahead of the clock.  It sleeps on the
 * guest's sound doorbell until play starts; while playing it wakes the
 * guest's mixing task (PORTS) whenever the ring runs low.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include "amigaio.h"
#include "sndshare.h"
#include "sndout.h"
#include "miglog.h"

#define LEAD 60		/* ms fed ahead of the clock */
#define WANT 120	/* ms the guest keeps mixed ahead */
#define TICK 10		/* ms between feeds */

int
mig_snd_helper(int ready, int life, int dev)
{
    struct mig_snd *s = (struct mig_snd *)MIG_SND_BASE;
    unsigned char *ring = (unsigned char *)MIG_SND_BASE + MIG_SND_HEADER_SIZE;
    struct sndout so;
    struct pollfd p;
    pid_t guest = getppid();
    unsigned int gen = 0, fs = 0, at, len;
    long n, k;
    int playing = 0, bell = 1;
    char success = 1;

    memset((char *)s, 0, sizeof *s);
    s->magic = MIG_SND_MAGIC;
    s->version = MIG_SND_VERSION;
    MIG_SND_BARRIER();
    s->ready = 1;
    so_init(&so);
    if (write(ready, &success, 1) != 1)
        return 1;
    close(ready);
    p.fd = life;
    p.events = POLLIN;
    for (;;) {
        n = poll(&p, 1, playing ? TICK : 0);
        if (n < 0 && errno != EINTR)
            return 1;
        if (n > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            return 0;
        if (!s->play) {
            if (playing) {
                so_flush(&so);
                miglog(1, "sound: stopped: %u frames, %u passes, %u kicks",
                    s->frames, s->passes, s->kicks);
            }
            playing = 0;
            /* before the guest starts: ESRCH */
            if (bell && ioctl(dev, AMIGAIOC_SNDWAIT, guest) < 0 && errno != EINTR) {
                if (errno != ESRCH)
                    bell = 0;
                else
                    poll(&p, 1, 100);
            } else if (!bell)
                poll(&p, 1, 100);
            continue;
        }
        if (!playing || s->gen != gen) {
            gen = s->gen;
            if (s->rate < 4000 || s->rate > 65535 || (s->chans != 1 && s->chans != 2)) {
                s->play = 0;
                continue;
            }
            so_start(&so, (unsigned long)s->rate << 16, SNDE_S16, (int)s->chans);
            fs = 2 * s->chans;
            s->want = (WANT * s->rate / 1000) * fs;
            playing = 1;
            miglog(1, "sound: %u Hz, %u channels", s->rate, s->chans);
        }
        MIG_SND_BARRIER();
        len = s->head - s->tail;
        if (len > MIG_SND_RING || len % fs) {
            s->tail = s->head;
            len = 0;
        }
        n = so_due(&so, LEAD * (long)s->rate / 1000);
        if (n > (long)(len / fs))
            n = len / fs;
        for (; n > 0; n -= k) {
            at = s->tail % MIG_SND_RING;
            k = (MIG_SND_RING - at) / fs;
            if (k > n)
                k = n;
            so_put(&so, (char *)ring + at, k);
            MIG_SND_BARRIER();
            s->tail += k * fs;
            s->frames += k;
        }
        if (s->head - s->tail < s->want) {
            s->doorbell = 1;
            MIG_SND_BARRIER();
            if (ioctl(dev, AMIGAIOC_KICK, guest) == 0)
                s->kicks++;
        }
    }
}
