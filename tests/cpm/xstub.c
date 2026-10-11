/* xstub.c -- the system around the BDOS, for the host-side tests: one process, no locks */
static int nolocks;
int *b3_nlocks = &nolocks;
long b3_xdos() { return 0; }
long b3_hlock() { return 0; }
void b3_conflush() { }
