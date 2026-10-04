#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include "sysroot.h"

int
mig_sysroot(const char *home, const char *system, char *out,
    unsigned int size, int *readonly)
{
    struct stat st;
    unsigned int n;
    if (home && home[0] == '/') {
        n = strlen(home);
        if (n + sizeof "/Amiga" > size) { errno = ENAMETOOLONG; return -1; }
        strcpy(out, home);
        strcat(out, "/Amiga");
        if (stat(out, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
            *readonly = 0;
            return 0;
        }
    }
    if (strlen(system) + 1 > size) { errno = ENAMETOOLONG; return -1; }
    if (stat(system, &st) < 0) return -1;
    if ((st.st_mode & S_IFMT) != S_IFDIR) { errno = ENOTDIR; return -1; }
    strcpy(out, system);
    *readonly = 1;
    return 0;
}
