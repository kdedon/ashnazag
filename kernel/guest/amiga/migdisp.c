#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include "dsio.h"
#include "amigaio.h"
#include "migvideo.h"
#include "rtgshare.h"
#include "inputshare.h"

extern int munmap();

static struct migframe frame;
static short lookup[4096];
static unsigned short red[256], green[256], blue[256];
static unsigned char pixels[MIG_WIDTH * MIG_HEIGHT];

static int palette(int fd)
{
    struct fbcmap cm;
    int i, count = 0, v, index, overflow = 0;
    memset(lookup, 0xff, sizeof lookup);
    for (i = 0; i < frame.width * frame.height; i++) {
        v = frame.rgb[i];
        if (lookup[v] < 0) {
            if (count == 256) { overflow = 1; break; }
            lookup[v] = count;
            red[count] = ((v >> 8) & 15) * 0x1111;
            green[count] = ((v >> 4) & 15) * 0x1111;
            blue[count++] = (v & 15) * 0x1111;
        }
    }
    if (overflow)
        for (i = 0; i < 256; i++) {
            red[i] = ((i >> 5) & 7) * 65535UL / 7;
            green[i] = ((i >> 2) & 7) * 65535UL / 7;
            blue[i] = (i & 3) * 65535UL / 3;
        }
    for (i = 0; i < frame.width * frame.height; i++) {
        v = frame.rgb[i];
        index = overflow ? ((v >> 9) << 5) | (((v >> 5) & 7) << 2) | ((v >> 2) & 3) : lookup[v];
        pixels[i] = index;
    }
    cm.cm_start = 0; cm.cm_count = overflow ? 256 : count;
    cm.cm_red = red; cm.cm_green = green; cm.cm_blue = blue;
    return ioctl(fd, FBIOPUTCMAP, &cm);
}
static void refresh(unsigned char *fb, struct fbinfo *fi)
{
    int sx = frame.width <= 384 ? 2 : 1, sy = 2;
    int x, y, ox, oy, width, height;
    unsigned char *row;
    if ((unsigned long)(frame.height * sy) > fi->fi_height) sy = 1;
    if ((unsigned long)(frame.width * sx) > fi->fi_width) sx = 1;
    width = frame.width * sx; height = frame.height * sy;
    ox = fi->fi_width > (unsigned long)width ? (fi->fi_width - width) / 2 : 0;
    oy = fi->fi_height > (unsigned long)height ? (fi->fi_height - height) / 2 : 0;
    for (y = 0; y < height && (unsigned long)(oy + y) < fi->fi_height; y++) {
        row = fb + fi->fi_offset + (oy + y) * fi->fi_rowbytes + ox;
        for (x = 0; x < width && (unsigned long)(ox + x) < fi->fi_width; x++)
            row[x] = pixels[(y / sy) * frame.width + x / sx];
    }
}
static int rtg_blank(int fd, unsigned char *fb, struct fbinfo *fi)
{
    struct fbcmap cm;
    unsigned short black = 0;
    cm.cm_start = 0; cm.cm_count = 1;
    cm.cm_red = cm.cm_green = cm.cm_blue = &black;
    if (ioctl(fd, FBIOPUTCMAP, &cm) < 0) return -1;
    memset(fb + fi->fi_offset, 0, fi->fi_rowbytes * fi->fi_height);
    return 0;
}

static int rtg_refresh(int fd, unsigned char *fb, struct fbinfo *fi,
    struct mig_rtg *s)
{
    struct fbcmap cm;
    unsigned int i, y, xoff = (fi->fi_width - s->width) / 2;
    unsigned int yoff = (fi->fi_height - s->height) / 2;
    for (i = 0; i < 256; i++) {
        red[i] = s->palette[i][0];
        green[i] = s->palette[i][1];
        blue[i] = s->palette[i][2];
    }
    cm.cm_start = 0; cm.cm_count = 256;
    cm.cm_red = red; cm.cm_green = green; cm.cm_blue = blue;
    if (ioctl(fd, FBIOPUTCMAP, &cm) < 0) return -1;
    for (y = 0; y < s->height; y++)
        memcpy(fb + fi->fi_offset + (yoff + y) * fi->fi_rowbytes + xoff,
            (const unsigned char *)MIG_RTG_PIXELS + s->offset + y * s->stride,
            s->width);
    return 0;
}

static int input_open(const char *name, int session, struct evinfo *info)
{
    int fd = open(name, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    if (ioctl(fd, EVIOCBIND, session) < 0 ||
        ioctl(fd, EVIOCGINFO, info) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static void input_read(int fd, struct evinfo *info, int hidden,
    struct mig_input *q, struct mig_input_state *state)
{
    struct inev events[32];
    int n, i;
    n = read(fd, (char *)events, sizeof events);
    if (n < 0) {
        if (errno != EAGAIN && errno != EINTR) mig_input_reset(q, state);
        return;
    }
    if (n % sizeof events[0]) { mig_input_reset(q, state); return; }
    if (hidden) return;
    for (i = 0; i < n / (int)sizeof events[0]; i++)
        mig_input_event(q, state, info->ei_kset, info->ei_flags,
            events[i].ie_type, events[i].ie_code, events[i].ie_value);
}

int migdisp(int dev, int ready, int life, long owner)
{
    struct fbacq acq;
    struct fbinfo fi;
    struct amigavideo video;
    struct fbnote note;
    struct pollfd p[4];
    struct evinfo keyinfo, mouseinfo;
    struct mig_input_state inputstate;
    struct mig_input *input = (struct mig_input *)MIG_INPUT_BASE;
    struct mig_rtg rtg;
    int rtgactive = 0;
    unsigned char *fb;
    int fd, hidden = 0, status, lastw = 0, lasth = 0, warned = 0;
    char success = 1;
    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("startmig: /dev/fb0"); return 1; }
    memset(&acq, 0, sizeof acq);
    acq.fa_kind = FBK_USER; acq.fa_flags = FBA_FRONT;
    strcpy(acq.fa_name, "amiga");
    if (ioctl(fd, FBIOACQUIRE, &acq) < 0 || ioctl(fd, FBIOGINFO, &fi) < 0) {
        perror("startmig: display session"); return 1;
    }
    if (fi.fi_depth != 8 || fi.fi_layout != FBL_PACKED || fi.fi_cmapsize < 256 ||
        fi.fi_rowbytes < fi.fi_width || fi.fi_offset > fi.fi_size ||
        !fi.fi_rowbytes || fi.fi_height > (fi.fi_size - fi.fi_offset) / fi.fi_rowbytes) {
        fprintf(stderr, "startmig: display requires a packed 8-bit framebuffer with 256 colors\n");
        return 1;
    }
    if (ioctl(fd, FBIOCACHE, FBC_CI) < 0) {
        perror("startmig: framebuffer cache mode"); return 1;
    }
    fb = (unsigned char *)mmap((caddr_t)0, fi.fi_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == (unsigned char *)-1) { perror("startmig: display mmap"); return 1; }
    memset(fb + fi.fi_offset, 0, fi.fi_rowbytes * fi.fi_height);
    mig_rtg_init((struct mig_rtg *)MIG_RTG_BASE, fi.fi_width, fi.fi_height);
    mig_input_init(input);
    memset(&inputstate, 0, sizeof inputstate);
    memset(&keyinfo, 0, sizeof keyinfo);
    memset(&mouseinfo, 0, sizeof mouseinfo);
    p[2].fd = input_open("/dev/kbd", fd, &keyinfo);
    p[3].fd = input_open("/dev/mouse", fd, &mouseinfo);
    if (p[2].fd < 0 || p[3].fd < 0)
        fprintf(stderr, "startmig: one or more session input devices unavailable\n");
    if (write(ready, &success, 1) != 1) return 1;
    close(ready);
    p[0].fd = life; p[1].fd = fd;
    p[0].events = p[1].events = p[2].events = p[3].events = POLLIN;
    for (;;) {
        status = poll(p, 4, hidden ? 500 : 40);
        if (status < 0) {
            if (errno == EINTR) continue;
            perror("startmig: display poll"); break;
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) break;
        if (p[1].revents & (POLLHUP | POLLERR | POLLNVAL)) break;
        if (p[1].revents & POLLIN) {
            if (read(fd, (char *)&note, sizeof note) != sizeof note) break;
            if (note.fn_type == FBN_HIDDEN) {
                hidden = 1;
                mig_input_reset(input, &inputstate);
            }
            if (note.fn_type == FBN_SHOWN) { hidden = 0; rtgactive = -1; }
            if (note.fn_type == FBN_MODE) break;
        }
        if (p[2].revents & POLLIN)
            input_read(p[2].fd, &keyinfo, hidden, input, &inputstate);
        if (p[3].revents & POLLIN)
            input_read(p[3].fd, &mouseinfo, hidden, input, &inputstate);
        if ((p[2].revents | p[3].revents) & (POLLHUP | POLLERR | POLLNVAL)) {
            mig_input_reset(input, &inputstate);
            if (p[2].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                close(p[2].fd); p[2].fd = -1;
            }
            if (p[3].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                close(p[3].fd); p[3].fd = -1;
            }
        }
        if (hidden) continue;
        status = mig_rtg_snapshot((const volatile struct mig_rtg *)MIG_RTG_BASE,
            &rtg, fi.fi_width, fi.fi_height);
        if (status < 0) continue;
        if (status == MIG_RTG_BLANK) {
            if (rtgactive != MIG_RTG_BLANK && rtg_blank(fd, fb, &fi) < 0) {
                perror("startmig: RTG blank"); break;
            }
            rtgactive = MIG_RTG_BLANK;
            continue;
        }
        if (status == MIG_RTG_VISIBLE) {
            if (rtgactive != MIG_RTG_VISIBLE || lastw != rtg.width || lasth != rtg.height) {
                memset(fb + fi.fi_offset, 0, fi.fi_rowbytes * fi.fi_height);
                lastw = rtg.width; lasth = rtg.height;
            }
            rtgactive = MIG_RTG_VISIBLE;
            if (rtg_refresh(fd, fb, &fi, &rtg) < 0) {
                perror("startmig: RTG palette"); break;
            }
            continue;
        }
        if (rtgactive) { lastw = lasth = 0; rtgactive = 0; }
        memset(&video, 0, sizeof video);
        video.av_pid = owner;
        if (ioctl(dev, AMIGAIOC_VIDEO, &video) < 0) {
            if (errno == ENXIO) continue;
            perror("startmig: AMIGAIOC_VIDEO"); break;
        }
        status = mig_render(video.av_custom, (const unsigned char *)0,
            AMIGA_CHIP_SIZE, &frame);
        if (status <= 0) {
            if (status < 0 && !warned) {
                fprintf(stderr, "startmig: unsupported display mode or invalid display memory\n");
                warned = 1;
            }
            continue;
        }
        warned = 0;
        if (palette(fd) < 0) {
            perror("startmig: display palette"); break;
        }
        if (lastw != frame.width || lasth != frame.height) {
            memset(fb + fi.fi_offset, 0, fi.fi_rowbytes * fi.fi_height);
            lastw = frame.width; lasth = frame.height;
        }
        refresh(fb, &fi);
    }
    mig_input_reset(input, &inputstate);
    if (p[2].fd >= 0) close(p[2].fd);
    if (p[3].fd >= 0) close(p[3].fd);
    munmap((caddr_t)fb, fi.fi_size);
    close(fd);
    return 0;
}
