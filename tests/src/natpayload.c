#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int
main(int argc, char **argv)
{
    char cwd[1200], path[1220], *value, *memory;
    struct flock lock;
    int i, child, status, fd;

    alarm(5);
    if (argc != 5 || strcmp(argv[0], argv[4]))
        return 80;
    value = getenv("NATIVEHOST_ENVIRONMENT");
    if (!value || strcmp(value, argv[1]))
        return 81;
    value = getenv("NATIVEHOST_ROOT");
    if (!value || strcmp(value, argv[2]) || !getcwd(cwd, sizeof cwd) || strcmp(cwd, argv[2]))
        return 82;
    value = getenv("NATIVEHOST_MEDIA");
    if (!value || strcmp(value, argv[3]))
        return 83;
    memory = malloc(2048);
    if (!memory)
        return 84;
    for (i = 0; i < 2048; i++)
        memory[i] = (char)(i ^ 0x5a);
    for (i = 0; i < 2048; i++)
        if (memory[i] != (char)(i ^ 0x5a))
            return 85;
    free(memory);
    child = fork();
    if (child < 0)
        return 86;
    if (!child) {
        sprintf(path, "%s/.env", cwd);
        fd = open(path, O_RDWR);
        memset(&lock, 0, sizeof lock);
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        _exit(fd >= 0 && fcntl(fd, F_SETLK, &lock) < 0 &&
            (errno == EACCES || errno == EAGAIN) ? 0 : 87);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
        return 88;
    fd = open("payload.ok", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, argv[1], strlen(argv[1])) != (long)strlen(argv[1]))
        return 89;
    close(fd);
    return 37;
}
