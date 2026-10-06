#include <sys/types.h>
#include <sys/times.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include "miglog.h"

/* the target compiler's header wants the native compiler's &... */
#if defined(__STDC__) && __STDC__ == 0
#undef va_start
#define va_start(ap, last) (void)(ap = (va_list)((char *)&(last) + sizeof(last)))
#endif

int miglog_fd = -1;
static clock_t start;
static long hz = 100;

void
miglog_start(int fd)
{
    struct tms t;
    miglog_fd = fd;
    start = times(&t);
    hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
}

long
miglog_ms(void)
{
    struct tms t;
    return (long)(times(&t) - start) * 1000 / hz;
}

/* "seconds message" on the log; with echo, "startmig: message" on stderr too */
void
miglog(int echo, const char *fmt, ...)
{
    char line[1400];
    va_list ap;
    long ms = miglog_ms();
    int n;
    n = sprintf(line, "%4ld.%02ld ", ms / 1000, ms % 1000 / 10);
    va_start(ap, fmt);
    vsprintf(line + n, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    if (miglog_fd >= 0) write(miglog_fd, line, strlen(line));
    if (echo) {
        fputs("startmig: ", stderr);
        fputs(line + n, stderr);
    }
}
