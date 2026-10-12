import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[3]
source = (root / 'kernel/guest/mod/guestcore/gcpu.c').read_text()
start = source.index('static int\nguest_fsig1(')
end = source.index('\n/*', start)
macro_start = source.index('#define\tVIPL_PASS')
macro_end = source.index('\n\n', macro_start)
prelude = r'''
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include "guest.h"
typedef unsigned long k_sigset_t;
#define SIG_DFL 0
#define SIGKILL 9
#define SIGINT 2
#define SIGQUIT 3
#define SIGTERM 15
#define SIGHUP 1
#define SIGILL 4
#define SIGTRAP 5
#define SIGEMT 7
#define SIGFPE 8
#define SIGBUS 10
#define SIGSEGV 11
#define SIGUSR1 16
#define NSIG 32
#define SR_IPL 0x700
#define sigmask(n) (1UL << ((n) - 1))
struct proc { k_sigset_t p_hold, p_sig; };
static struct { struct proc *u_procp; int u_signal[NSIG]; } u;
static int splhi_(void) { return 0; }
static void splx_(int s) { (void)s; }
static int __amix_fsig(struct proc *p) { return (p->p_sig & ~p->p_hold) != 0; }
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(1); } } while (0)
'''
# guest.h declares this import; the harness supplies it.
prelude = prelude.replace('static int __amix_fsig(', 'int __amix_fsig(')
test = r'''
int main(void) {
    struct proc p = {0, 0};
    struct guest_proc gp = {0};
    struct guest_profile profile = {0};
    int faults[] = { SIGILL, SIGTRAP, SIGEMT, SIGFPE, SIGBUS, SIGSEGV };
    unsigned int i;
    gp.gp_prof = &profile;
    gp.gp_flags = GPF_QUIET | GPF_NCALL;
    u.u_procp = &p;
    for (i = 0; i < sizeof faults / sizeof faults[0]; i++) {
        p.p_sig = sigmask(faults[i]);
        CHECK(guest_fsig1(&p, &gp) == 1);
        CHECK(p.p_hold == 0);
    }
    p.p_sig = sigmask(SIGUSR1);
    CHECK(guest_fsig1(&p, &gp) == 0);
    p.p_sig = p.p_hold = sigmask(SIGTERM);
    CHECK(guest_fsig1(&p, &gp) == 0);
    CHECK(p.p_hold == sigmask(SIGTERM));
    p.p_hold = 0;
    CHECK(guest_fsig1(&p, &gp) == 1);
    gp.gp_flags = GPF_QUIET;
    p.p_sig = sigmask(SIGSEGV);
    CHECK(guest_fsig1(&p, &gp) == 0);
    puts("PASS native fault delivery, asynchronous hold, signal mask preservation");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nativecall-signals.') as directory:
    directory = pathlib.Path(directory)
    unit = directory / 'signals.c'
    unit.write_text(prelude + source[macro_start:macro_end] + '\n' + source[start:end] + test)
    executable = directory / 'signals'
    subprocess.run(['nice', '-n', '19', 'cc', '-std=gnu89', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'kernel/guest/include'), str(unit), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
