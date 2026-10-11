#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int nativeenv_execprobe(const char *path, char *const args[])
{
    char cwd[1200], meta[1200];
    struct flock fl;
    int child, status, fd;
    char *root = getenv("NATIVEHOST_ROOT");

    if (!root || !getcwd(cwd, sizeof cwd) || strcmp(cwd, root) ||
        !getenv("NATIVEHOST_MEDIA") || !getenv("NATIVEHOST_ENVIRONMENT") ||
        strcmp(path, args[0]) || !args[1] || strcmp(args[1], "argument") || args[2])
        _exit(90);
    child = fork();
    if (child < 0)
        _exit(91);
    if (!child) {
        sprintf(meta, "%s/.env", root);
        fd = open(meta, O_RDWR);
        memset(&fl, 0, sizeof fl);
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        _exit(fd >= 0 && fcntl(fd, F_SETLK, &fl) < 0 &&
            (errno == EACCES || errno == EAGAIN) ? 0 : 92);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
        _exit(93);
    puts("native exec: arguments, working directory, environment and lock verified");
    _exit(37);
}
