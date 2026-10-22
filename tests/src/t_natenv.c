/*
 * t_natenv: startlisa and startvoyage200 (one launcher): own roots,
 * one session per root, the payload exec'd with the session lock.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "t.h"

#define FIXTURE "/tmp/natlaunch"

static char *families[] = { "lisa", "voyage200" };
static char *directories[] = { "Lisa", "Voyage200" };
static char *launcher = "/tests/startnative", *payload = "/tests/natpayload";

static void
check(char *name, int valid)
{
    t_check(name, valid, "unexpected result");
}

static int
run(int family, char *name, char *executable, char *media)
{
    char root[256];
    char *args[16];
    int child, status, n = 0;

    sprintf(root, "%s/%s%s%s", FIXTURE, directories[family], name ? "/" : "", name ? name : "");
    args[n++] = launcher;
    args[n++] = families[family];
    if (name) {
        args[n++] = "-e";
        args[n++] = name;
    }
    args[n++] = "-x";
    args[n++] = executable;
    args[n++] = "-m";
    args[n++] = media;
    args[n++] = "--";
    args[n++] = families[family];
    args[n++] = root;
    args[n++] = media;
    args[n++] = executable;
    args[n] = 0;
    fflush(stdout);
    child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        alarm(8);
        execv(launcher, args);
        _exit(126);
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int
sessionlock(char *root)
{
    char path[512];
    struct flock lock;
    int fd;

    sprintf(path, "%s/.env", root);
    fd = open(path, O_RDWR | O_CREAT, 0600);
    memset(&lock, 0, sizeof lock);
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fd >= 0 && fcntl(fd, F_SETLK, &lock) == 0)
        return fd;
    if (fd >= 0)
        close(fd);
    return -1;
}

int
main()
{
    char roots[2][256], media[256], broken[256], path[512], label[80];
    unsigned char header[52];
    int i, fd, lock, result;

    t_init("natenv", 60);
    putenv("HOME=" FIXTURE);
    if (!t_check("fixture", mkdir(FIXTURE, 0700) == 0, "%s: %s", FIXTURE, T_ERR))
        return t_done();
    for (i = 0; i < 2; i++) {
        sprintf(roots[i], "%s/%s", FIXTURE, directories[i]);
        if (mkdir(roots[i], 0700) < 0)
            return 1;
    }
    sprintf(media, "%s/media", FIXTURE);
    fd = open(media, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || write(fd, "fixture", 7) != 7)
        return 1;
    close(fd);
    for (i = 0; i < 2; i++) {
        sprintf(label, "%s.exec", families[i]);
        check(label, run(i, 0, payload, media) == 37);
        sprintf(path, "%s/payload.ok", roots[i]);
        fd = open(path, O_RDONLY);
        result = fd >= 0 && read(fd, header, sizeof header) == (long)strlen(families[i]) &&
            !memcmp(header, families[i], strlen(families[i]));
        if (fd >= 0)
            close(fd);
        sprintf(label, "%s.own_root", families[i]);
        check(label, result);
        lock = sessionlock(roots[i]);
        sprintf(label, "%s.release_after_exit", families[i]);
        check(label, lock >= 0);
        sprintf(label, "%s.competing_launch", families[i]);
        check(label, lock >= 0 && run(i, 0, payload, media) == 1);
        sprintf(label, "%s.family_isolation", families[i]);
        check(label, lock >= 0 && run((i + 1) % 2, 0, payload, media) == 37);
        if (lock >= 0)
            close(lock);
    }
    sprintf(path, "%s/separate", roots[0]);
    check("named_root", mkdir(path, 0700) == 0 && run(0, "separate", payload, media) == 37);
    check("list_default", system("/tests/startnative lisa -l | grep -s '^office (default)$' > /dev/null") == 0);
    check("set_default", system("/tests/startnative lisa -d separate && "
        "/tests/startnative lisa -l | grep -s '^separate (default)$' > /dev/null") == 0);
    unlink(FIXTURE "/.default/lisa");
    rmdir(FIXTURE "/.default");
    check("missing_media", run(0, 0, payload, "/tmp/natlaunch/absent") == 1);
    check("not_set_up", system("/tests/startnative lisa -e absent -x /tests/natpayload 2>&1 | "
        "grep -s 'no lisa environment for this user' > /dev/null") == 0);
    memset(header, 0, sizeof header);
    memcpy(header, "\177ELF\001\002\001", 7);
    header[17] = 2;
    header[19] = 4;
    header[23] = 1;
    sprintf(broken, "%s/broken", FIXTURE);
    fd = open(broken, O_WRONLY | O_CREAT | O_EXCL, 0700);
    result = fd >= 0 && write(fd, header, sizeof header) == sizeof header;
    if (fd >= 0)
        close(fd);
    check("exec_failure", result && run(0, 0, broken, media) == 127);
    lock = sessionlock(roots[0]);
    check("release_after_exec_failure", lock >= 0);
    if (lock >= 0)
        close(lock);
    check("retry_after_failure", run(0, 0, payload, media) == 37);
    /* no host program in the folder: the installed one */
    check("installed_host", system("/tests/startnative voyage200 -m " FIXTURE "/media -q 2>&1 | "
        "grep -s /usr/lib/voyage200/native > /dev/null || "
        "/tests/startnative voyage200 -m " FIXTURE "/media -q") == 0);
    sprintf(path, "%s/separate/payload.ok", roots[0]);
    unlink(path);
    sprintf(path, "%s/separate/.env", roots[0]);
    unlink(path);
    sprintf(path, "%s/separate", roots[0]);
    rmdir(path);
    for (i = 0; i < 2; i++) {
        sprintf(path, "%s/payload.ok", roots[i]);
        unlink(path);
        sprintf(path, "%s/.env", roots[i]);
        unlink(path);
        rmdir(roots[i]);
    }
    unlink(media);
    unlink(broken);
    rmdir(FIXTURE);
    return t_done();
}
