#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/termio.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "sys/mod.h"
#include "nativehost.h"
#include "t.h"

#ifdef __AMIX__
extern int munmap();
#endif

static int hostfd = -1, sequence;

static int
host_request(char *command, char *answer)
{
    struct pollfd p;
    char request[128], line[256];
    int n, used = 0, seq;
    long deadline = t_now_ms() + 2000;

    if (hostfd < 0)
        return 0;
    sprintf(request, "@@ %d %s\n", ++sequence, command);
    if (write(hostfd, request, strlen(request)) != strlen(request))
        return 0;
    p.fd = hostfd;
    p.events = POLLIN;
    while (t_now_ms() < deadline) {
        if (poll(&p, 1, 50) <= 0)
            continue;
        n = read(hostfd, line + used, sizeof line - 1 - used);
        if (n <= 0)
            return 0;
        used += n;
        line[used] = 0;
        if (strchr(line, '\n')) {
            if (sscanf(line, "@@ok %d", &seq) == 1 && seq == sequence) {
                char *value = strchr(line + 5, ' ');
                if (value) {
                    value++;
                    value[strcspn(value, "\r\n")] = 0;
                    strcpy(answer, value);
                    return 1;
                }
            }
            used = 0;
        }
        if (used == sizeof line - 1)
            return 0;
    }
    return 0;
}

static void
input_test(struct nh_display *display)
{
    struct termio terminal;
    struct inev event;
    char answer[256];
    long deadline, dx = 0, dy = 0;
    int n, down = 0, up = 0, code = -1, bad = 0;

    hostfd = open("/dev/term/b", O_RDWR | O_NOCTTY);
    if (hostfd >= 0 && ioctl(hostfd, TCGETA, &terminal) == 0) {
        terminal.c_iflag = IGNCR;
        terminal.c_oflag = 0;
        terminal.c_lflag = ICANON;
        terminal.c_cflag |= CREAD | CLOCAL;
        ioctl(hostfd, TCSETAF, &terminal);
    }
    if (!host_request("ping", answer) || strcmp(answer, "pong")) {
        t_skip("input_injection", "display hostio unavailable");
        if (hostfd >= 0)
            close(hostfd);
        hostfd = -1;
        return;
    }
    while (nh_input(display->keyboard, &event) == 1)
        ;
    while (nh_input(display->mouse, &event) == 1)
        ;
    t_check("key_request", host_request("key a", answer), "host input request");
    deadline = t_now_ms() + 1500;
    while (t_now_ms() < deadline && !up) {
        n = nh_input(display->keyboard, &event);
        if (n < 0) {
            bad++;
            break;
        }
        if (n && event.ie_type == IE_KEY) {
            if (event.ie_value == 1 && !down) {
                down++;
                code = event.ie_code;
            } else if (event.ie_value == 0 && down && event.ie_code == code)
                up++;
            else
                bad++;
        }
        if (!n)
            poll(0, 0, 10);
    }
    t_check("key_pair", down == 1 && up == 1 && !bad,
        "down %d up %d invalid %d", down, up, bad);
    t_check("mouse_request", host_request("move 8 5 200", answer), "host input request");
    deadline = t_now_ms() + 1500;
    bad = 0;
    while (t_now_ms() < deadline && (dx != 8 || dy != 5)) {
        n = nh_input(display->mouse, &event);
        if (n < 0) {
            bad++;
            break;
        }
        if (n && event.ie_type == IE_REL) {
            if (event.ie_code == IE_RELX)
                dx += event.ie_value;
            else if (event.ie_code == IE_RELY)
                dy += event.ie_value;
        }
        if (!n)
            poll(0, 0, 10);
    }
    t_check("mouse_delta", dx == 8 && dy == 5 && !bad,
        "x %ld y %ld invalid %d", dx, dy, bad);
    close(hostfd);
    hostfd = -1;
}

static void
memory_file_clock(void)
{
    struct nh_clock before, after;
    unsigned char *memory;
    char buffer[16];
    int fd, result;

    memset(&after, 0, sizeof after);
    memory = nh_memory(8192, 0);
    t_check("memory_map", memory != (void *)-1, "%s", T_ERR);
    if (memory != (void *)-1) {
        t_check("memory_zero", memory[0] == 0 && memory[8191] == 0, "initial contents");
        memory[0] = 17;
        memory[8191] = 23;
        t_check("memory_write", memory[0] == 17 && memory[8191] == 23, "mapped contents");
        t_check("memory_release", munmap((void *)memory, 8192) == 0, "%s", T_ERR);
    }
    t_check("memory_empty", nh_memory(0, 0) == (void *)-1 && errno == EINVAL, "%s", T_ERR);
    fd = open("/tmp/nativehost", O_CREAT | O_TRUNC | O_RDWR, 0600);
    t_check("file_open", fd >= 0, "%s", T_ERR);
    if (fd >= 0) {
        t_check("file_write", nh_file_transfer(fd, 4, "abcdef", 6, 1) == 6, "%s", T_ERR);
        memset(buffer, 0xff, sizeof buffer);
        t_check("file_sparse_read", nh_file_transfer(fd, 0, buffer, sizeof buffer, 0) == 10 &&
            !memcmp(buffer, "\0\0\0\0abcdef", 10), "file content and EOF");
        t_check("file_eof", nh_file_transfer(fd, 10, buffer, 1, 0) == 0, "EOF");
        t_check("file_offset", nh_file_transfer(fd, -1, buffer, 1, 0) < 0 && errno == EINVAL,
            "%s", T_ERR);
        t_check("file_close", close(fd) == 0, "%s", T_ERR);
        t_check("file_remove", unlink("/tmp/nativehost") == 0, "%s", T_ERR);
    }
    if (t_check("clock_sample", nh_clock_read(&before) == 0, "%s", T_ERR)) {
        poll(0, 0, 200);
        result = nh_clock_read(&after);
        t_check("clock_progress", result == 0 &&
            after.frequency == before.frequency && after.frequency > 0 &&
            after.ticks - before.ticks > 0,
            "frequency %ld ticks %lu", before.frequency, after.ticks - before.ticks);
    }
}

int
main(void)
{
    struct nh_display display;
    struct fbstate state;
    struct inev event;
    int id, result;
    unsigned char *pixels;
    unsigned long count, i;

    memset(&state, 0, sizeof state);
    t_init("nativehost", 30);
    memory_file_clock();
    id = modload(access("/tests/dsuser", R_OK) == 0 ?
        "/tests/dsuser" : "/etc/conf/mod.d/dsuser");
    if (!t_check("display_module", id >= 0, "%s", T_ERR))
        return t_done();
    nh_display_init(&display);
    result = nh_display_open(&display, "/dev/fb0", "nativehost", 1);
    if (t_check("display_open", result == 0, "%s", T_ERR)) {
        result = ioctl(display.framebuffer, FBIOGSTATE, &state);
        t_check("display_owner", result == 0 &&
            state.st_session > 0 && state.st_front == state.st_session,
            "session %ld front %ld", state.st_session, state.st_front);
        pixels = (unsigned char *)display.mapping + display.info.fi_offset;
        count = display.info.fi_rowbytes * display.info.fi_height;
        for (i = 0; i < count; i++)
            pixels[i] = (unsigned char)(i * 13 + 7);
        result = 1;
        for (i = 0; i < count; i++)
            if (pixels[i] != (unsigned char)(i * 13 + 7)) {
                result = 0;
                break;
            }
        t_check("display_readback", result, "framebuffer bytes %lu", count);
        t_check("display_busy", nh_display_open(&display, "/dev/fb0", "nativehost", 1) < 0 &&
            errno == EBUSY, "%s", T_ERR);
        t_check("input_poll", nh_input(display.keyboard, &event) >= 0, "%s", T_ERR);
        input_test(&display);
    }
    t_check("display_close", nh_display_close(&display) == 0 &&
        display.mapping == (void *)-1 && display.framebuffer == -1 &&
        display.keyboard == -1 && display.mouse == -1, "%s", T_ERR);
    t_check("display_close_again", nh_display_close(&display) == 0, "%s", T_ERR);
    t_check("display_unload", moduload(id) == 0, "%s", T_ERR);
    return t_done();
}
