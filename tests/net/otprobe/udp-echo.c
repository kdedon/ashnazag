#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifndef READY_PATH
#define READY_PATH "/tmp/otudp.ready"
#endif
#ifndef ECHO_PORT
#define ECHO_PORT 7007
#endif
#ifdef __linux__
typedef socklen_t addrlen;
#else
typedef int addrlen;
#endif
static volatile sig_atomic_t done;
static void stop(int sig) { (void)sig; done = 1; }
int main(void)
{
    struct sockaddr_in local, peer;
    struct pollfd p;
    char bytes[1024];
    int fd, ready, n, result = 1;
    addrlen length;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 1;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(ECHO_PORT);
    local.sin_addr.s_addr = htonl(0x7f000001UL);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) goto out;
    length = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &length) < 0) goto out;
    signal(SIGTERM, stop); signal(SIGALRM, stop); alarm(300);
    ready = open(READY_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (ready < 0) goto out;
    if (write(ready, (char *)&local.sin_port, 2) != 2) { close(ready); goto out; }
    close(ready);
    p.fd = fd; p.events = POLLIN;
    while (!done) {
        p.revents = 0;
        n = poll(&p, 1, 500);
        if (n < 0) { if (errno == EINTR) continue; goto out; }
        if (!n) continue;
        if (!(p.revents & POLLIN)) goto out;
        length = sizeof(peer);
        n = recvfrom(fd, bytes, sizeof(bytes), 0, (struct sockaddr *)&peer, &length);
        if (n < 0) { if (errno == EINTR) continue; goto out; }
        if (sendto(fd, bytes, n, 0, (struct sockaddr *)&peer, length) != n) goto out;
    }
    result = 0;
out:
    alarm(0); close(fd); unlink(READY_PATH);
    return result;
}
