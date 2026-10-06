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
#include <signal.h>
#include "amigaio.h"
#include <sys/time.h>
#include "dsio.h"
#include "rtgshare.h"
#include "inputshare.h"
#include "hostfswire.h"
#include "miglog.h"

extern int munmap();

int migkick = -1;
extern int mig_fs_bell;

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

/*
 * The card draws straight into the session's memory, the display while in
 * front; this keeps the colour table in step.  Returns -1 on error.
 */
static int cmap(int fd, struct mig_rtg *s, int status)
{
    static int shown = -1;
    static unsigned short pal[3][256];
    struct fbcmap cm;
    unsigned int i;
    int changed = status != shown;
    for (i = 0; i < 256; i++) {
        unsigned short r = status == MIG_RTG_VISIBLE ? s->palette[i][0] : 0;
        unsigned short g = status == MIG_RTG_VISIBLE ? s->palette[i][1] : 0;
        unsigned short b = status == MIG_RTG_VISIBLE ? s->palette[i][2] : 0;
        if (pal[0][i] != r || pal[1][i] != g || pal[2][i] != b) changed = 1;
        pal[0][i] = r; pal[1][i] = g; pal[2][i] = b;
    }
    shown = status;
    if (!changed) return 0;
    cm.cm_start = 0; cm.cm_count = 256;
    cm.cm_red = pal[0]; cm.cm_green = pal[1]; cm.cm_blue = pal[2];
    return ioctl(fd, FBIOPUTCMAP, &cm);
}


/* the card's pointer: shape, colours, place */
static struct { unsigned int on, w, h; int x, y; unsigned short rgb[4][3];
    unsigned char img[48][16]; } cur;
static unsigned char pen[4];
static unsigned short curpal[3][256];

/* the card's pointer state, if it is between updates; 1 if it changed */
static int cursor_read(void)
{
    volatile struct mig_rtg *s = (volatile struct mig_rtg *)MIG_RTG_BASE;
    static unsigned int done = 0xffffffffU;
    unsigned int seq = s->cseq;
    if (seq & 1 || seq == done) return 0;
    MIG_RTG_BARRIER();
    cur.on = s->con; cur.x = s->cx; cur.y = s->cy;
    /* guest memory: keep the sums below in range */
    if (cur.x < -4096 || cur.x > 4096 || cur.y < -4096 || cur.y > 4096) cur.on = 0;
    cur.w = s->cw > 16 ? 16 : s->cw; cur.h = s->ch > 48 ? 48 : s->ch;
    memcpy(cur.rgb, (const void *)s->crgb, sizeof cur.rgb);
    memcpy(cur.img, (const void *)s->cimg, sizeof cur.img);
    MIG_RTG_BARRIER();
    if (seq != s->cseq) return 1;
    done = seq;
    return 1;
}

/* the nearest palette entries to the pointer's colours */
static void pens(struct mig_rtg *r)
{
    unsigned int i, c, best, d;
    for (c = 1; c < 4; c++) {
        best = 0xffffffffU; pen[c] = 0;
        for (i = 0; i < 256; i++) {
            d = (unsigned int)abs((int)(r->palette[i][0] >> 8) - (cur.rgb[c][0] >> 8)) +
                abs((int)(r->palette[i][1] >> 8) - (cur.rgb[c][1] >> 8)) +
                abs((int)(r->palette[i][2] >> 8) - (cur.rgb[c][2] >> 8));
            if (d < best) { best = d; pen[c] = i; }
        }
    }
}

/* rows 0 to h-1 of w bytes, stride apart from origin, lie in [VRAM, end) without wrapping */
static int fits(unsigned long origin, unsigned long stride, unsigned long w, unsigned long h,
    unsigned long end)
{
    if (!w || !h || origin < MIG_RTG_VRAM || origin > end || w > end - origin) return 0;
    return h == 1 || (stride && h - 1 <= (end - origin - w) / stride);
}

/* 1 if the lock was free and is now ours */
static int ptake(volatile struct mig_rtg *s)
{
    char busy;
    __asm__ __volatile__("tas %1\n\tsmi %0" : "=d" (busy), "=m" (s->plock) : "m" (s->plock) : "memory");
    return !busy;
}

/*
 * Puts back what the pointer covered, keeping pixels the guest drew since,
 * then, if VISIBLE, saves and draws it where the card has it.  The card
 * does the same putting back before its own drawing under the lock.
 * Returns 0, or -1 if the card held the lock.
 */
static int pointer(unsigned char *fb, struct fbinfo *fi, struct mig_rtg *r, int visible)
{
    volatile struct mig_rtg *s = (volatile struct mig_rtg *)MIG_RTG_BASE;
    unsigned long origin, end = MIG_RTG_VRAM + fi->fi_size;
    unsigned char *p;
    int x, y, x0, y0, x1, y1;
    unsigned int c;
    /* the card rings after its drawing when it sees pwant */
    if (!ptake(s)) {
        s->pwant = 1;
        if (!ptake(s)) return -1;
    }
    s->pwant = 0;
    /* guest memory: only a rectangle inside the session's memory */
    if (s->pshown && s->px0 >= 0 && s->py0 >= 0 && s->px1 - s->px0 <= 16 &&
        s->py1 - s->py0 <= 48 && s->px0 < s->px1 && s->py0 < s->py1 &&
        fits(s->porigin, s->pstride, s->px1, s->py1, end)) {
        for (y = s->py0; y < s->py1; y++) {
            p = fb + (s->porigin - MIG_RTG_VRAM) + y * s->pstride;
            for (x = s->px0; x < s->px1; x++)
                if (p[x] == s->pdrawn[y - s->py0][x - s->px0])
                    p[x] = s->psave[y - s->py0][x - s->px0];
        }
    }
    s->pshown = 0;
    origin = r->vram + r->offset;
    x0 = cur.x < 0 ? 0 : cur.x; y0 = cur.y < 0 ? 0 : cur.y;
    x1 = cur.x + (int)cur.w; y1 = cur.y + (int)cur.h;
    if (x1 > (int)r->width) x1 = r->width;
    if (y1 > (int)r->height) y1 = r->height;
    if (visible && cur.on && x0 < x1 && y0 < y1 && fits(origin, r->stride, x1, y1, end)) {
        for (y = y0; y < y1; y++) {
            p = fb + (origin - MIG_RTG_VRAM) + y * r->stride;
            for (x = x0; x < x1; x++) {
                s->psave[y - y0][x - x0] = p[x];
                c = cur.img[y - cur.y][x - cur.x] & 3;
                p[x] = s->pdrawn[y - y0][x - x0] = c ? pen[c] : p[x];
            }
        }
        s->porigin = origin; s->pstride = r->stride;
        s->px0 = x0; s->py0 = y0; s->px1 = x1; s->py1 = y1;
        s->pshown = 1;
    }
    MIG_RTG_BARRIER();
    s->plock = 0;
    return 0;
}


/*
 * Fallback when the Workbench mode is narrower than the display's rows:
 * the card's memory is RAM and the shown screen's changed rows are copied,
 * centred, with the pointer drawn over them from the copy.
 */
static unsigned char *shadow;
static int cshown, cx0, cy0, cx1, cy1;

/* rows of N bytes, N a multiple of 4 on 4-byte boundaries, are equal */
static int same(const unsigned char *a, const unsigned char *b, unsigned int n)
{
    const unsigned long *x = (const unsigned long *)a, *y = (const unsigned long *)b;
    if ((n | (unsigned long)a | (unsigned long)b) & 3) return !memcmp(a, b, n);
    for (n >>= 2; n; n--)
        if (*x++ != *y++) return 0;
    return 1;
}

/* the screen's rows from SRC, stride apart, changed or all, to the display */
static int copyrows(unsigned char *fb, struct fbinfo *fi, struct mig_rtg *s, int full,
    const unsigned char *src0)
{
    unsigned int y, xoff = (fi->fi_width - s->width) / 2, yoff = (fi->fi_height - s->height) / 2;
    const unsigned char *src;
    int rows = 0;
    for (y = 0; y < s->height; y++) {
        src = src0 + y * s->stride;
        if (!full && same(shadow + y * s->width, src, s->width)) continue;
        memcpy(shadow + y * s->width, src, s->width);
        memcpy(fb + fi->fi_offset + (yoff + y) * fi->fi_rowbytes + xoff,
            shadow + y * s->width, s->width);
        rows++;
    }
    return rows;
}

static void copypointer(unsigned char *fb, struct fbinfo *fi, struct mig_rtg *s)
{
    unsigned int xoff = (fi->fi_width - s->width) / 2, yoff = (fi->fi_height - s->height) / 2;
    int x, y;
    unsigned int c;
    unsigned char *row;
    if (cshown)
        for (y = cy0; y < cy1; y++)
            memcpy(fb + fi->fi_offset + (yoff + y) * fi->fi_rowbytes + xoff + cx0,
                shadow + y * s->width + cx0, cx1 - cx0);
    cshown = 0;
    if (!cur.on || !cur.w || !cur.h) return;
    cx0 = cur.x < 0 ? 0 : cur.x; cy0 = cur.y < 0 ? 0 : cur.y;
    cx1 = cur.x + (int)cur.w; cy1 = cur.y + (int)cur.h;
    if (cx1 > (int)s->width) cx1 = s->width;
    if (cy1 > (int)s->height) cy1 = s->height;
    if (cx0 >= cx1 || cy0 >= cy1) return;
    for (y = cy0; y < cy1; y++) {
        row = fb + fi->fi_offset + (yoff + y) * fi->fi_rowbytes + xoff;
        for (x = cx0; x < cx1; x++)
            if ((c = cur.img[y - cur.y][x - cur.x] & 3) != 0) row[x] = pen[c];
    }
    cshown = 1;
}

/*
 * The screen at card address ORIGIN, as the guest maps it: a copy read
 * through /proc into a buffer, or 0.
 */
static const unsigned char *guestrows(int proc, unsigned long origin, unsigned long n)
{
    static unsigned char *buf;
    static unsigned long size;
    if (n > size) {
        free(buf);
        if (!(buf = (unsigned char *)malloc(n))) { size = 0; return 0; }
        size = n;
    }
    if (lseek(proc, (off_t)origin, SEEK_SET) != (off_t)origin || read(proc, (char *)buf, n) != (int)n)
        return 0;
    return buf;
}

/* asks the guest to move the display's part of card memory; 0 once moved */
static int movecard(pid_t guest, int ack, int ram)
{
    volatile struct mig_rtg *live = (volatile struct mig_rtg *)MIG_RTG_BASE;
    char c;
    int n;
    live->ram = ram;
    if (kill(guest, SIGUSR1) < 0) return -1;
    while ((n = read(ack, &c, 1)) < 0 && errno == EINTR)
        ;
    return n == 1 && live->inram == (unsigned int)ram ? 0 : -1;
}

/*
 * Wakes when the guest rings: the card changed the palette, the mode or
 * the pointer, drew into copied memory or put the pointer back, or a SYS:
 * request moved the startup status on.  Copying, it also looks every 40 ms
 * until a second passes without change, for pixels P96 wrote itself.
 */
static void watch(int fd, int go, int ack, unsigned char *fb, struct fbinfo *fi, pid_t guest)
{
    struct mig_rtg rtg;
    struct amigawait aw;
    struct fbcmap cm;
    static unsigned short grey[2] = { 0, 0xaaaa };
    volatile struct mig_rtg *live = (volatile struct mig_rtg *)MIG_RTG_BASE;
    struct pollfd p;
    /* card memory as the host set it up, before the guest runs */
    unsigned long end = live->vram + live->memory_size, origin;
    const unsigned char *src;
    char path[32];
    int other, away = 0, proc = -1;
    int status, bound = 0, warned = 0, bad = 0, last = -1;
    int moved, changed, laststatus = -1, copying = 0, full, rows, idle = 0;
    unsigned int lastw = 0, lasth = 0;
    unsigned int lastoff = 0, laststride = 0;
    long next = 0;
    memset(&aw, 0, sizeof aw);
    aw.aw_pid = guest;
    cm.cm_start = 0; cm.cm_count = 2;
    cm.cm_red = cm.cm_green = cm.cm_blue = grey;
    ioctl(fd, FBIOPUTCMAP, &cm);
    drawstatus(fb, fi);
    /* until the guest has entered: its end closes go */
    p.fd = go; p.events = POLLIN;
    while (poll(&p, 1, -1) < 0 && errno == EINTR)
        ;
    close(go);
    if (ack >= 0) {
        sprintf(path, "/proc/%05ld", (long)guest);
        if ((proc = open(path, O_RDONLY)) < 0)
            miglog(0, "%s: %s; screens other than the display-sized one stay hidden", path, strerror(errno));
    }
    for (;;) {
        if (copying && idle <= 25)
            poll((struct pollfd *)0, 0, 40);
        else if (ioctl(mig_fs_bell, AMIGAIOC_WAITN, &aw) < 0 && errno != EINTR) {
            if (errno != ESRCH)
                miglog(0, "display doorbell: %s", strerror(errno));
            return;
        }
        live->wakes++;
        if (getppid() == 1) return;
        status = mig_rtg_snapshot((const volatile struct mig_rtg *)MIG_RTG_BASE,
            &rtg, fi->fi_width, fi->fi_height);
        /* mid-update or invalid: the card rings when it completes an update */
        if (status < 0) {
            if (bad++ == 20)
                miglog(0, "RTG state invalid: %ux%u rows %u at %u of %u", rtg.width,
                    rtg.height, rtg.stride, rtg.offset, rtg.memory_size);
            continue;
        }
        bad = 0;
        if (status != last) {
            miglog(0, "RTG state %d", status);
            last = status;
        }
        if (!bound && status == MIG_RTG_NATIVE) {
            if (miglog_ms() >= next) {
                drawstatus(fb, fi);
                next = miglog_ms() + 250;
            }
            continue;
        }
        if (!bound) miglog(0, "RTG screen %ux%u: Picasso96 bound", rtg.width, rtg.height);
        bound = 1;
        /*
         * Direct, a screen outside the display's rows: the guest moves the
         * display's part of card memory to RAM and the screen is copied;
         * the display-sized screen back in front moves it back.
         */
        other = status == MIG_RTG_VISIBLE && !rtg.copy && (rtg.offset || rtg.stride != rtg.vstride);
        if (status == MIG_RTG_VISIBLE && proc >= 0 && other != away) {
            /* the pointer out of the screen being moved first */
            if (other && pointer(fb, fi, &rtg, 0) < 0) {
                curpal[0][0] ^= 1;
                continue;
            }
            if (movecard(guest, ack, other) < 0) {
                miglog(0, "card memory not moved");
                close(proc);
                proc = -1;
            } else {
                away = other;
                live->track = other;
                copying = 0;
                laststatus = -1;
                miglog(0, "RTG screen %ux%u %s", rtg.width, rtg.height,
                    other ? "copied to the display" : "is the display again");
            }
        }
        if (other && !away && !warned) {
            miglog(0, "RTG screen at %u, rows %u apart: outside the display", rtg.offset, rtg.stride);
            warned = 1;
        }
        if (cmap(fd, &rtg, status) < 0) {
            perror("startmig: RTG palette");
            return;
        }
        if (rtg.copy || away) {
            if (status != MIG_RTG_VISIBLE) {
                copying = 0;
                continue;
            }
            live->drawn = 0;
            MIG_RTG_BARRIER();
            origin = rtg.vram + rtg.offset;
            if (!fits(origin, rtg.stride, rtg.width, rtg.height,
                rtg.copy ? MIG_RTG_VRAM + MIG_RTG_EXTRA : end))
                continue;
            src = rtg.copy ? (const unsigned char *)origin :
                guestrows(proc, origin, (rtg.height - 1) * rtg.stride + rtg.width);
            if (!src)
                continue;
            full = !copying || rtg.width != lastw || rtg.height != lasth;
            if (!live->copies)
                miglog(0, "RTG screen %ux%u copied to the display", rtg.width, rtg.height);
            live->copies++;
            if (full) {
                if (!shadow && !(shadow = (unsigned char *)malloc(fi->fi_width * fi->fi_height)))
                    return;
                memset(fb + fi->fi_offset, 0, fi->fi_rowbytes * fi->fi_height);
                cshown = 0;
                lastw = rtg.width; lasth = rtg.height;
            }
            copying = 1;
            moved = cursor_read();
            if (moved || memcmp(curpal, rtg.palette, sizeof curpal)) {
                memcpy(curpal, rtg.palette, sizeof curpal);
                pens(&rtg);
            }
            rows = copyrows(fb, fi, &rtg, full, src);
            idle = rows ? 0 : idle + 1;
            if (moved || rows || full)
                copypointer(fb, fi, &rtg);
            continue;
        }
        /* the pointer again after a move, a new shape or palette, or the card's drawing */
        moved = cursor_read();
        changed = moved || memcmp(curpal, rtg.palette, sizeof curpal) ||
            rtg.offset != lastoff || rtg.stride != laststride || status != laststatus;
        if (changed || (status == MIG_RTG_VISIBLE && cur.on && !((volatile struct mig_rtg *)MIG_RTG_BASE)->pshown)) {
            if (changed) {
                memcpy(curpal, rtg.palette, sizeof curpal);
                pens(&rtg);
            }
            /* the card is putting it back for its drawing: again at its ring */
            if (pointer(fb, fi, &rtg, status == MIG_RTG_VISIBLE) < 0) {
                curpal[0][0] ^= 1;
                continue;
            }
            lastoff = rtg.offset; laststride = rtg.stride; laststatus = status;
        }
    }
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

/*
 * The display session, opened before the helpers fork so that the guest
 * maps the same one as its card's memory.  Returns the fd, or -1.
 */
int migdisp_open(struct fbinfo *fi)
{
    struct fbacq acq;
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("startmig: /dev/fb0"); return -1; }
    memset(&acq, 0, sizeof acq);
    acq.fa_kind = FBK_USER; acq.fa_flags = FBA_FRONT;
    strcpy(acq.fa_name, "amiga");
    if (ioctl(fd, FBIOACQUIRE, &acq) < 0 || ioctl(fd, FBIOGINFO, fi) < 0) {
        perror("startmig: display session"); close(fd); return -1;
    }
    if (fi->fi_depth != 8 || fi->fi_layout != FBL_PACKED || fi->fi_cmapsize < 256 ||
        fi->fi_rowbytes < fi->fi_width || fi->fi_offset > fi->fi_size ||
        !fi->fi_rowbytes || fi->fi_size > MIG_RTG_VRAM_MAX ||
        fi->fi_height > (fi->fi_size - fi->fi_offset) / fi->fi_rowbytes) {
        fprintf(stderr, "startmig: display requires a packed 8-bit framebuffer with 256 colors\n");
        close(fd); return -1;
    }
    /* write-through keeps reads cached for the card's blits */
    if (ioctl(fd, FBIOCACHE, FBC_WT) < 0 && ioctl(fd, FBIOCACHE, FBC_CI) < 0) {
        perror("startmig: framebuffer cache mode"); close(fd); return -1;
    }
    return fd;
}

int migdisp(int ready, int life, int go, int ack, int fd, struct fbinfo *fip)
{
    struct fbinfo fi = *fip;
    struct fbnote note;
    struct pollfd p[4];
    struct evinfo keyinfo, mouseinfo;
    struct mig_input_state inputstate;
    struct mig_input *input = (struct mig_input *)MIG_INPUT_BASE;
    unsigned char *fb;
    int hidden = 0, status, kick;
    pid_t guest = getppid(), watcher;
    char success = 1;
    fb = (unsigned char *)mmap((caddr_t)0, fi.fi_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == (unsigned char *)-1) { perror("startmig: display mmap"); return 1; }
    memset(fb + fi.fi_offset, 0, fi.fi_rowbytes * fi.fi_height);
    miglog(0, "display session %ux%u", fi.fi_width, fi.fi_height);
    mig_input_init(input);
    memset(&inputstate, 0, sizeof inputstate);
    memset(&keyinfo, 0, sizeof keyinfo);
    memset(&mouseinfo, 0, sizeof mouseinfo);
    p[2].fd = input_open("/dev/kbd", fd, &keyinfo);
    p[3].fd = input_open("/dev/mouse", fd, &mouseinfo);
    if (p[2].fd < 0 || p[3].fd < 0)
        fprintf(stderr, "startmig: one or more session input devices unavailable\n");
    watcher = fork();
    if (watcher == 0) {
        close(ready); close(life);
        if (p[2].fd >= 0) close(p[2].fd);
        if (p[3].fd >= 0) close(p[3].fd);
        watch(fd, go, ack, fb, &fi, guest);
        _exit(0);
    }
    if (watcher < 0) { perror("startmig: display fork"); return 1; }
    close(go);
    if (ack >= 0) close(ack);
    if (write(ready, &success, 1) != 1) return 1;
    close(ready);
    p[0].fd = life; p[1].fd = fd;
    p[0].events = p[1].events = p[2].events = p[3].events = POLLIN;
    for (;;) {
        status = poll(p, 4, -1);
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
            if (note.fn_type == FBN_SHOWN) hidden = 0;
            if (note.fn_type == FBN_MODE) break;
        }
        kick = input->head == input->tail;
        if (p[2].revents & POLLIN)
            input_read(p[2].fd, &keyinfo, hidden, input, &inputstate);
        if (p[3].revents & POLLIN)
            input_read(p[3].fd, &mouseinfo, hidden, input, &inputstate);
        /* the guest takes the events now rather than at its next poll */
        /* only when the queue was empty: otherwise the guest is already on it */
        if ((p[2].revents | p[3].revents) & POLLIN && !hidden && migkick >= 0 && kick) {
            input->doorbell = 1;
            ioctl(migkick, AMIGAIOC_KICK, guest);
        }
        if ((p[2].revents | p[3].revents) & (POLLHUP | POLLERR | POLLNVAL)) {
            mig_input_reset(input, &inputstate);
            if (p[2].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                close(p[2].fd); p[2].fd = -1;
            }
            if (p[3].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                close(p[3].fd); p[3].fd = -1;
            }
        }
    }
    kill(watcher, SIGTERM);
    mig_input_reset(input, &inputstate);
    if (p[2].fd >= 0) close(p[2].fd);
    if (p[3].fd >= 0) close(p[3].fd);
    munmap((caddr_t)fb, fi.fi_size);
    close(fd);
    return 0;
}
