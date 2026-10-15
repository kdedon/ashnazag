#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "envroot.h"

static int
nativeelf(char *path)
{
    unsigned char h[52];
    struct stat st;
    int fd, n;

    if (access(path, X_OK) < 0 || (fd = open(path, O_RDONLY)) < 0)
        return -1;
    n = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? read(fd, h, sizeof h) : -1;
    close(fd);
    if (n != sizeof h || memcmp(h, "\177ELF", 4) || h[4] != 1 ||
        h[5] != 2 || h[6] != 1 || h[16] != 0 || h[17] != 2 ||
        h[18] != 0 || h[19] != 4 || h[20] != 0 || h[21] != 0 ||
        h[22] != 0 || h[23] != 1) {
        errno = ENOEXEC;
        return -1;
    }
    return 0;
}

static int
publish(char *key, char *value)
{
    char *s = malloc(strlen(key) + strlen(value) + 2);
    if (!s)
        return -1;
    sprintf(s, "%s=%s", key, value);
    return putenv(s);
}

int
main(int argc, char **argv)
{
    char *prog = strrchr(argv[0], '/'), *family, *id = 0;
    char *executable = 0, *media = 0, root[1100], bin[1200], image[1200];
    struct stat st;
    int i = 1, lock, check = 0;

    prog = prog ? prog + 1 : argv[0];
    if (!strcmp(prog, "startlisa"))
        family = "lisa";
    else if (!strcmp(prog, "startvoyage200"))
        family = "voyage200";
    else if (!strcmp(prog, "startnext"))
        family = "next";
    else if (argc > 1 && (!strcmp(argv[1], "lisa") || !strcmp(argv[1], "voyage200") ||
        !strcmp(argv[1], "next")))
        family = argv[i++];
    else
        goto usage;
    while (i < argc && strcmp(argv[i], "--")) {
        if (!strcmp(argv[i], "-q")) {
            check = 1;
            i++;
            continue;
        }
        if (i + 1 >= argc)
            goto usage;
        if (!strcmp(argv[i], "-e"))
            id = argv[i + 1];
        else if (!strcmp(argv[i], "-x"))
            executable = argv[i + 1];
        else if (!strcmp(argv[i], "-m"))
            media = argv[i + 1];
        else
            goto usage;
        i += 2;
    }
    if (i < argc)
        i++;
    if (envroot(family, id, getenv("HOME"), root, sizeof root) < 0) {
        fprintf(stderr, "%s: invalid environment root or name\n", prog);
        return 2;
    }
    sprintf(bin, "%s/native", root);
    sprintf(image, "%s/media", root);
    if (!executable)
        executable = bin;
    if (!media)
        media = image;
    if (!strcmp(family, "next") && media == image && stat(image, &st) < 0) {
        /* no ROM: boot the user's kernel from the environment */
        sprintf(image, "%s/mach_kernel", root);
    }
    if (root[0] != '/' || executable[0] != '/' || media[0] != '/') {
        fprintf(stderr, "%s: root, executable and media paths must be absolute\n", prog);
        return 2;
    }
    if (nativeelf(executable) < 0) {
        fprintf(stderr, "%s: %s: native m68k ELF executable required: %s\n",
            prog, executable, strerror(errno));
        return 1;
    }
    if (stat(media, &st) < 0 || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) ||
        access(media, R_OK) < 0) {
        fprintf(stderr, "%s: %s: readable media file or directory required\n", prog, media);
        return 1;
    }
    if (check)
        return stat(root, &st) == 0 && S_ISDIR(st.st_mode) && access(root, W_OK) == 0 ? 0 : 1;
    lock = envlock(prog, family, root);
    if (lock < 0) {
        if (lock == -2)
            fprintf(stderr, "%s: %s: writable environment directory required\n", prog, root);
        return 1;
    }
    /* exec retains the session lock until the native process exits. */
    if (fcntl(lock, F_SETFD, 0) < 0 || chdir(root) < 0 ||
        publish("NATIVEHOST_ENVIRONMENT", family) < 0 ||
        publish("NATIVEHOST_ROOT", root) < 0 || publish("NATIVEHOST_MEDIA", media) < 0) {
        perror(prog);
        return 1;
    }
    argv[i - 1] = executable;
    execv(executable, argv + i - 1);
    perror(executable);
    return 127;
usage:
    fprintf(stderr, "usage: %s %s[-q] [-e NAME] [-x /executable] [-m /media] [-- args...]\n",
        prog, !strcmp(prog, "startlisa") || !strcmp(prog, "startvoyage200") ||
        !strcmp(prog, "startnext") ? "" : "lisa|voyage200|next ");
    return 2;
}
