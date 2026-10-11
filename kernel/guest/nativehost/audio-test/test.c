#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sndio.h"
#include "nativehost_audio.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed line %d\n", __LINE__); _exit(77); } } while (0)
static int mode, interrupts;
static unsigned int header_used, body_used, phase, frames, stage, reply_used;
static struct sndrec command;
static unsigned char body[sizeof(struct sndfmt)];
extern int __real_close(int);
extern int __real_read(int, void *, unsigned int);
extern int __real_write(int, const void *, unsigned int);

int
__wrap_open(const char *path, int flags, ...)
{
    CHECK(!strcmp(path, SNDPATH));
    if (mode == 1) { errno = ENOENT; return -1; }
    return 55;
}

int
__wrap_ioctl(int fd, unsigned long request, ...)
{
    CHECK(fd == 55);
    if (mode == 2) for (;;) pause();
    if (mode == 3) { errno = ENOTTY; return -1; }
    return 0;
}

int
__wrap_close(int fd)
{
    if (fd != 55) return __real_close(fd);
    if (mode == 6) { errno = EIO; return -1; }
    return 0;
}

int
__wrap_write(int fd, const void *data, unsigned int count)
{
    const unsigned char *p = data;
    unsigned int i, n;
    struct sndfmt f;

    if (fd != 55) return __real_write(fd, data, count);
    if (!interrupts++) { errno = EINTR; return -1; }
    n = count > 1 ? count / 2 : count;
    for (i = 0; i < n; i++) {
        if (header_used < sizeof command) {
            ((unsigned char *)&command)[header_used++] = p[i];
            if (header_used < sizeof command) continue;
            CHECK(command.r_len <= 1024);
            if (command.r_cmd == SNDR_DRAIN) {
                CHECK(!command.r_len && frames == 5513 && stage == 1);
                stage = 2;
            } else if (command.r_cmd == SNDR_STAT) {
                CHECK(!command.r_len && stage == 3);
                stage = 4;
            } else if (command.r_cmd == SNDR_FMT) {
                CHECK(command.r_len == sizeof f && stage == 0);
            } else CHECK(command.r_cmd == SNDR_PCM && stage == 1);
            if (!command.r_len) header_used = 0;
            continue;
        }
        if (command.r_cmd == SNDR_PCM) {
            CHECK(p[i] == (phase < 22050 / 2 ? 160 : 96));
            phase += 440;
            if (phase >= 22050) phase -= 22050;
            frames++;
        } else body[body_used] = p[i];
        body_used++;
        if (body_used == command.r_len) {
            if (command.r_cmd == SNDR_FMT) {
                memcpy(&f, body, sizeof f);
                CHECK(f.f_rate == (22050UL << 16) && f.f_enc == SNDE_U8 && f.f_chans == 1);
                stage = 1;
            }
            body_used = header_used = 0;
        }
    }
    return n;
}

int
__wrap_read(int fd, void *data, unsigned int count)
{
    struct sndrec r;
    struct sndstat st;
    unsigned char response[sizeof r + sizeof st];
    unsigned int total, n;

    if (fd != 55) return __real_read(fd, data, count);
    CHECK(stage == 2 || stage == 4);
    memset(&r, 0, sizeof r);
    r.r_cmd = stage == 2 ? SNDR_DRAIN : SNDR_STAT;
    r.r_len = stage == 2 ? 0 : sizeof st;
    if (mode == 5) r.r_cmd = 999;
    memset(&st, 0, sizeof st);
    st.ss_heard = 5564;
    st.ss_muted = mode == 4 ? 1 : 0;
    memcpy(response, &r, sizeof r);
    memcpy(response + sizeof r, &st, sizeof st);
    total = sizeof r + r.r_len;
    n = total - reply_used;
    if (n > count) n = count;
    if (n > 3) n = 3;
    memcpy(data, response + reply_used, n);
    reply_used += n;
    if (reply_used == total) { reply_used = 0; stage++; }
    return n;
}

int
main(void)
{
    int m, expected[] = {0, ENOENT, ETIMEDOUT, ENOTTY, EIO, EIO, EIO};

    CHECK(nh_audio_tone(0, 250) == -1 && errno == EINVAL);
    CHECK(nh_audio_tone(10001, 250) == -1 && errno == EINVAL);
    CHECK(nh_audio_tone(440, 0) == -1 && errno == EINVAL);
    CHECK(nh_audio_tone(440, 2001) == -1 && errno == EINVAL);
    for (m = 0; m < 7; m++) {
        mode = m;
        if (!m) CHECK(nh_audio_tone(440, 250) == 0);
        else CHECK(nh_audio_tone(440, 250) == -1 && errno == expected[m]);
    }
    puts("audio tests passed: PCM, partial I/O, EINTR, validation, unavailable, stale file, muted, malformed, close, timeout");
    return 0;
}
