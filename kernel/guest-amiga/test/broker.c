#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <poll.h>
#include <stdio.h>
#include "hostfswire.h"

/* the voluntary context switches of process P */
static long switches(pid_t p)
{
    char path[64], line[128];
    long n = -1;
    FILE *f;
    snprintf(path, sizeof path, "/proc/%ld/status", (long)p);
    f = fopen(path, "r");
    assert(f);
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "voluntary_ctxt_switches: %ld", &n) == 1) break;
    fclose(f);
    return n;
}

int main(void)
{
    char dir[] = "/tmp/migbroker-XXXXXX", ok, bell = 1;
    int ready[2], life[2], go[2], status, i;
    long n;
    pid_t child;
    struct pollfd p;
    struct mig_fs_mailbox *b;
    assert(sizeof(unsigned int) == 4);
    assert(sizeof *b < MIG_FS_MAP_SIZE);
    assert(mkdtemp(dir));
    b = mmap(0, MIG_FS_MAP_SIZE, PROT_READ|PROT_WRITE,
        MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    assert(b != MAP_FAILED);
    assert(pipe(ready) == 0 && pipe(life) == 0 && pipe(go) == 0);
    child = fork(); assert(child >= 0);
    if (!child) {
        close(ready[0]); close(life[1]); close(go[1]);
        _exit(mig_fs_broker_at(ready[1], life[0], go[0], dir, 1, b));
    }
    close(ready[1]); close(life[0]); close(go[0]);
    p.fd = ready[0]; p.events = POLLIN;
    assert(poll(&p, 1, 2000) == 1);
    assert(read(ready[0], &ok, 1) == 1 && ok == 1);
    assert(b->magic == MIG_FS_MAGIC && b->version == MIG_FS_VERSION);
    memset(&b->request, 0, sizeof b->request);
    b->request.op = MIG_FS_MKDIR;
    strcpy(b->request.path, "blocked");
    MIG_FS_BARRIER(); b->state = MIG_FS_REQUEST;
    assert(write(go[1], &bell, 1) == 1);
    for (i = 0; i < 200 && b->state != MIG_FS_REPLY; i++) usleep(10000);
    assert(b->state == MIG_FS_REPLY);
    MIG_FS_BARRIER();
    assert(b->request.error == 214);
    /* idle: the broker sleeps until the next ring */
    usleep(50000);
    n = switches(child);
    usleep(500000);
    assert(switches(child) - n <= 1);
    close(life[1]); close(ready[0]); close(go[1]);
    for (i = 0; i < 200; i++) {
        if (waitpid(child, &status, WNOHANG) == child) break;
        usleep(10000);
    }
    assert(i < 200 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(rmdir(dir) == 0);
    munmap(b, MIG_FS_MAP_SIZE);
    puts("Filesystem broker publication, read-only dispatch, idle sleep and parent-exit cleanup pass");
    return 0;
}
