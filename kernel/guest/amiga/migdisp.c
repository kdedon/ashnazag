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
#include "amigaio.h"
#include <sys/time.h>
#include "dsio.h"
#include "rtgshare.h"
#include "inputshare.h"
#include "hostfswire.h"
#include "miglog.h"

extern int munmap(), gettimeofday();

int migkick = -1;
static unsigned short red[256], green[256], blue[256];
static unsigned char *shadow;
static int pensok;   /* the pointer's pens match the palette and its colours */

/* 5x7 glyphs, a row per byte, for the startup status */
static const char glyphs[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:/-_?";
static const unsigned char font[][7] = {
    {0,0,0,0,0,0,0},
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
    {28,18,17,17,17,18,28}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
    {7,2,2,2,2,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,17,25,21,19,17,17}, {14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,17,10,4,4,4}, {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {31,2,4,2,1,17,14}, {2,6,10,18,31,2,2}, {31,16,30,1,1,17,14},
    {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,2,12},
    {0,0,0,0,0,12,12}, {0,12,12,0,12,12,0}, {0,1,2,4,8,16,0},
    {0,0,0,31,0,0,0}, {0,0,0,0,0,0,31}, {14,17,1,2,4,0,4}
};

/* one line of text, twice size, colour 1 on 0 */
static void text(unsigned char *fb, struct fbinfo *fi, unsigned int y, const char *s)
{
    unsigned int n = strlen(s), x0, i, row, col, c;
    const char *g;
    unsigned char *p;
    if (n > fi->fi_width / 12) n = fi->fi_width / 12;
    x0 = (fi->fi_width - n * 12) / 2;
    for (row = 0; row < 16; row++)
        memset(fb + fi->fi_offset + (y + row) * fi->fi_rowbytes, 0, fi->fi_width);
    for (i = 0; i < n; i++) {
        c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        g = strchr(glyphs, c);
        c = g && c ? g - glyphs : sizeof glyphs - 2;
        for (row = 0; row < 14; row++) {
            p = fb + fi->fi_offset + (y + row) * fi->fi_rowbytes + x0 + i * 12;
            for (col = 0; col < 10; col++)
                p[col] = font[c][row / 2] >> (4 - col / 2) & 1;
        }
    }
}

/* the startup status while no RTG screen shows */
static void drawstatus(unsigned char *fb, struct fbinfo *fi)
{
    volatile struct mig_fs_status *st = (struct mig_fs_status *)MIG_FS_STATUS;
    char line[160], path[64];
    unsigned int i, y = fi->fi_height / 2 - 24;
    long ms = miglog_ms();
    if (fi->fi_height < 64) return;
    text(fb, fi, y, "STARTING THE AMIGA ENVIRONMENT");
    if (!st->requests)
        sprintf(line, "%ld S  KICKSTART", ms / 1000);
    else {
        for (i = 0; i < sizeof path - 1 && st->path[i]; i++)
            path[i] = st->path[i];
        path[i] = 0;
        sprintf(line, "%ld S  SYS:%s", ms / 1000, path);
    }
    text(fb, fi, y + 32, line);
}

static int rtg_blank(int fd, unsigned char *fb, struct fbinfo *fi)
{
    struct fbcmap cm;
    static unsigned short grey[2] = { 0, 0xaaaa };
    cm.cm_start = 0; cm.cm_count = 2;
    cm.cm_red = cm.cm_green = cm.cm_blue = grey;
    if (ioctl(fd, FBIOPUTCMAP, &cm) < 0) return -1;
    memset(fb + fi->fi_offset, 0, fi->fi_rowbytes * fi->fi_height);
    return 0;
}

/* rows of N bytes, N a multiple of 4 on 4-byte boundaries, are equal */
static int same(const unsigned char *a, const unsigned char *b, unsigned int n)
{
    const unsigned long *x = (const unsigned long *)a, *y = (const unsigned long *)b;
    if ((n | (unsigned long)a | (unsigned long)b) & 3) return !memcmp(a, b, n);
    for (n >>= 2; n; n--)
        if (*x++ != *y++) return 0;
    return 1;
}

/*
 * Copies the rows that changed since the last refresh, or all with full:
 * the framebuffer is uncached, so writes cost far more than compares.
 * Returns the number of rows copied, or -1.
 */
static int rtg_refresh(int fd, unsigned char *fb, struct fbinfo *fi,
    struct mig_rtg *s, int full)
{
    struct fbcmap cm;
    unsigned int i, y, xoff = (fi->fi_width - s->width) / 2;
    unsigned int yoff = (fi->fi_height - s->height) / 2;
    int changed = full, rows = 0;
    const unsigned char *src;
    for (i = 0; i < 256; i++) {
        if (red[i] != s->palette[i][0] || green[i] != s->palette[i][1] ||
            blue[i] != s->palette[i][2]) changed = 1, pensok = 0;
        red[i] = s->palette[i][0];
        green[i] = s->palette[i][1];
        blue[i] = s->palette[i][2];
    }
    cm.cm_start = 0; cm.cm_count = 256;
    cm.cm_red = red; cm.cm_green = green; cm.cm_blue = blue;
    if (changed && ioctl(fd, FBIOPUTCMAP, &cm) < 0) return -1;
    for (y = 0; y < s->height; y++) {
        src = (const unsigned char *)MIG_RTG_PIXELS + s->offset + y * s->stride;
        if (!full && same(shadow + y * s->width, src, s->width)) continue;
        memcpy(shadow + y * s->width, src, s->width);
        memcpy(fb + fi->fi_offset + (yoff + y) * fi->fi_rowbytes + xoff,
            shadow + y * s->width, s->width);
        rows++;
    }
    return rows + changed;
}

/* the host-drawn pointer: the card's state, where it is drawn, its pens */
static struct { unsigned int on, w, h; int x, y; unsigned short rgb[4][3];
    unsigned char img[48][16]; } cur, drawn;
static int curshown, curx0, cury0, curx1, cury1;
static unsigned char pen[4];

/* the card's pointer state, if it is between updates; 1 if it changed */
static int cursor_read(void)
{
    volatile struct mig_rtg *s = (volatile struct mig_rtg *)MIG_RTG_BASE;
    static unsigned int done = 0xffffffffU;
    unsigned int seq = s->cseq, old = cur.on;
    int ox = cur.x, oy = cur.y;
    if (seq & 1 || seq == done || !s->cursor) return 0;
    MIG_RTG_BARRIER();
    cur.on = s->con; cur.x = s->cx; cur.y = s->cy;
    /* guest memory: keep the sums below in range */
    if (cur.x < -4096 || cur.x > 4096 || cur.y < -4096 || cur.y > 4096) cur.on = 0;
    cur.w = s->cw > 16 ? 16 : s->cw; cur.h = s->ch > 48 ? 48 : s->ch;
    memcpy(cur.rgb, (const void *)s->crgb, sizeof cur.rgb);
    memcpy(cur.img, (const void *)s->cimg, sizeof cur.img);
    MIG_RTG_BARRIER();
    if (seq != s->cseq) { cur.on = 0; return 1; }
    done = seq;
    return cur.on != old || cur.x != ox || cur.y != oy ||
        memcmp(&cur, &drawn, sizeof cur) != 0;
}

/* screen rows of the old pointer from the shadow, then the new one on top */
static void cursor_draw(unsigned char *fb, struct fbinfo *fi, struct mig_rtg *s)
{
    unsigned int xoff = (fi->fi_width - s->width) / 2, yoff = (fi->fi_height - s->height) / 2;
    unsigned int i, best, d, c;
    int x, y;
    unsigned char *row;
    if (curshown)
        for (y = cury0; y < cury1; y++)
            memcpy(fb + fi->fi_offset + (yoff + y) * fi->fi_rowbytes + xoff + curx0,
                shadow + y * s->width + curx0, curx1 - curx0);
    curshown = 0;
    if (memcmp(cur.rgb, drawn.rgb, sizeof cur.rgb)) pensok = 0;
    drawn = cur;
    if (!cur.on || !cur.w || !cur.h) return;
    if (!pensok && !curx1) miglog(0, "pointer drawn by the display");
    /* nearest palette entries to the sprite colours */
    for (c = 1; c < 4 && !pensok; c++) {
        best = 0xffffffffUL; pen[c] = 0;
        for (i = 0; i < 256; i++) {
            d = (unsigned int)abs((int)(red[i] >> 8) - (cur.rgb[c][0] >> 8)) +
                abs((int)(green[i] >> 8) - (cur.rgb[c][1] >> 8)) +
                abs((int)(blue[i] >> 8) - (cur.rgb[c][2] >> 8));
            if (d < best) { best = d; pen[c] = i; }
        }
    }
    pensok = 1;
    curx0 = cur.x < 0 ? 0 : cur.x; cury0 = cur.y < 0 ? 0 : cur.y;
    curx1 = cur.x + (int)cur.w; cury1 = cur.y + (int)cur.h;
    if (curx1 > (int)s->width) curx1 = s->width;
    if (cury1 > (int)s->height) cury1 = s->height;
    if (curx0 >= curx1 || cury0 >= cury1) return;
    for (y = cury0; y < cury1; y++) {
        row = fb + fi->fi_offset + (yoff + y) * fi->fi_rowbytes + xoff;
        for (x = curx0; x < curx1; x++)
            if ((c = cur.img[y - cur.y][x - cur.x] & 3) != 0) row[x] = pen[c];
    }
    curshown = 1;
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

static long now(void)
{
    struct timeval tv;
    gettimeofday(&tv, (void *)0);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

int migdisp(int ready, int life)
{
    struct fbacq acq;
    struct fbinfo fi;
    struct fbnote note;
    struct pollfd p[4];
    struct evinfo keyinfo, mouseinfo;
    struct mig_input_state inputstate;
    struct mig_input *input = (struct mig_input *)MIG_INPUT_BASE;
    struct mig_rtg rtg;
    int rtgactive = 0;
    unsigned char *fb;
    int fd, hidden = 0, status, lastw = 0, lasth = 0, idle = 0, shown = 0, soon = 0, fresh = 0;
    pid_t guest = getppid();
    long tin = 0, waited = 0, longest = 0, changes = 0, lastfull = 0, t;
    long wwait = 0, wlong = 0, wn = 0, wlog = 0, ms;
    int rows, moved, kick;
    long nextstatus = 0;
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
    shadow = (unsigned char *)malloc(fi.fi_width * fi.fi_height);
    if (!shadow) { fprintf(stderr, "startmig: display: out of memory\n"); return 1; }
    miglog(0, "display session %ux%u", fi.fi_width, fi.fi_height);
    mig_rtg_init((struct mig_rtg *)MIG_RTG_BASE, fi.fi_width, fi.fi_height);
    ((struct mig_rtg *)MIG_RTG_BASE)->cursor = 1;
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
        /*
         * Compares the screen every 40 ms, 120 ms while it stays unchanged;
         * the pointer a tick after input, when the guest has moved it.
         * Timeouts are whole ticks: a shorter one would spin.
         */
        t = lastfull + (idle > 25 ? 120 : 40) - now();
        status = poll(p, 4, hidden ? 500 : soon || t < 20 ? 20 : (int)t);
        ms = now();
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
                if (migkick >= 0) {
                    input->doorbell = 1;
                    ioctl(migkick, AMIGAIOC_KICK, guest);
                }
            }
            if (note.fn_type == FBN_SHOWN) { hidden = 0; rtgactive = -1; }
            if (note.fn_type == FBN_MODE) break;
        }
        soon = (p[2].revents | p[3].revents) & POLLIN && !hidden;
        if ((p[2].revents | p[3].revents) & POLLIN) idle = 0;
        kick = input->head == input->tail;
        if (p[2].revents & POLLIN)
            input_read(p[2].fd, &keyinfo, hidden, input, &inputstate);
        if (p[3].revents & POLLIN)
            input_read(p[3].fd, &mouseinfo, hidden, input, &inputstate);
        /* the guest takes the events now rather than at its next poll */
        /* only when the queue was empty: otherwise the guest is already on it */
        if (soon && migkick >= 0 && kick) {
            input->doorbell = 1;
            ioctl(migkick, AMIGAIOC_KICK, guest);
        }
        fresh = soon && !tin;
        if (fresh) tin = ms;
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
        /* until the RTG card shows a screen, the session stays black */
        if (status != MIG_RTG_VISIBLE) {
            if (rtgactive != MIG_RTG_BLANK) {
                if (rtg_blank(fd, fb, &fi) < 0) {
                    perror("startmig: RTG blank"); break;
                }
                nextstatus = 0;
            }
            rtgactive = MIG_RTG_BLANK;
            if (!shown && miglog_ms() >= nextstatus) {
                drawstatus(fb, &fi);
                nextstatus = miglog_ms() + 1000;
            }
            continue;
        }
        if (!shown) {
            miglog(0, "RTG screen %ux%u: Picasso96 bound", rtg.width, rtg.height);
            shown = 1;
        }
        status = rtgactive != MIG_RTG_VISIBLE || lastw != rtg.width || lasth != rtg.height;
        if (status) {
            memset(fb + fi.fi_offset, 0, fi.fi_rowbytes * fi.fi_height);
            curshown = 0;
            lastw = rtg.width; lasth = rtg.height;
        }
        rtgactive = MIG_RTG_VISIBLE;
        rows = 0;
        if (status || ms - lastfull >= (idle > 25 ? 120 : 40) - 20) {
            rows = rtg_refresh(fd, fb, &fi, &rtg, status);
            if (rows < 0) {
                perror("startmig: RTG palette"); break;
            }
            lastfull = ms;
            idle = rows ? 0 : idle + 1;
        }
        moved = cursor_read();
        if (moved || rows)
            cursor_draw(fb, &fi, &rtg);
        /* input to the first screen change in a later refresh */
        if ((moved || rows) && tin && !fresh) {
            tin = ms - tin;
            if (tin < 1000) {
                waited += tin; changes++;
                if (tin > longest) longest = tin;
                wwait += tin; wn++;
                if (tin > wlong) wlong = tin;
            }
            tin = 0;
        }
        if (wn && ms - wlog >= 2000) {
            miglog(0, "input to screen: %ld changes, %ld ms average, %ld ms longest",
                wn, wwait / wn, wlong);
            wwait = wlong = wn = 0;
            wlog = ms;
        }
    }
    if (changes)
        miglog(1, "input to screen: %ld changes, %ld ms average, %ld ms longest",
            changes, waited / changes, longest);
    mig_input_reset(input, &inputstate);
    if (p[2].fd >= 0) close(p[2].fd);
    if (p[3].fd >= 0) close(p[3].fd);
    munmap((caddr_t)fb, fi.fi_size);
    close(fd);
    return 0;
}
