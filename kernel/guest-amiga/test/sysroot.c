#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sysroot.h"

int main(void)
{
    char tmp[] = "/tmp/migroot-XXXXXX", system[256], user[256], out[256];
    int readonly;
    assert(mkdtemp(tmp));
    sprintf(system, "%s/system", tmp);
    sprintf(user, "%s/Amiga", tmp);
    assert(mkdir(system, 0700) == 0);
    assert(mig_sysroot(tmp, system, out, sizeof out, &readonly) == 0);
    assert(readonly && !strcmp(out, system));
    assert(mkdir(user, 0700) == 0);
    assert(mig_sysroot(tmp, system, out, sizeof out, &readonly) == 0);
    assert(!readonly && !strcmp(out, user));
    assert(mig_sysroot(0, system, out, sizeof out, &readonly) == 0 && readonly);
    assert(mig_sysroot(tmp, system, out, 4, &readonly) < 0);
    assert(rmdir(user) == 0 && rmdir(system) == 0);
    assert(mig_sysroot(tmp, system, out, sizeof out, &readonly) < 0);
    assert(rmdir(tmp) == 0);
    puts("System directory selection: user, read-only fallback, missing tree and bounds pass");
    return 0;
}
