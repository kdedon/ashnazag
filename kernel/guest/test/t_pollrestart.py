#!/usr/bin/env python3
import os
from pathlib import Path
import resource
import subprocess
import tempfile


def block(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 0
    for end in range(opening, len(text)):
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
            if depth == 0:
                return text[start:end + 1]
    raise AssertionError("unterminated production block")


root = Path(__file__).resolve().parents[1]
systrap = (root / "mod/auxcore/auxsys.c").read_text()
signals = (root / "mod/auxcore/auxsig.c").read_text()
restart = block(systrap, "if (e == EINTR || e == ERESTART)")
restartable = block(signals, "aux_restartable(n)")
source = r'''
#include <assert.h>
#include <stdio.h>
#define EINTR 4
#define ERESTART 91
#define COMPAT_BSDSIGNALS 1
#define COMPAT_SYSCALLS 2
#define GR_PC(r) (*(r))
struct proc { int p_cursig; } proc;
struct aux_proc { int ap_compat,ap_mac; } profile;
struct { long u_sigrestart; } u;
int aux_polldiag;
long aux_pollintr,aux_pollretry,aux_polllast[4];
int
''' + restartable + r'''
static int checks;
static void check(int num,long cmd,int error,int compat,int signal,
                  long mask,int expected_restart,int diagnostic)
{
    struct proc *p=&proc;
    struct aux_proc *ap=&profile;
    long pc=0x104,*r=&pc,a[5]={cmd,0,0,0,0};
    int e=error,restarted=0;
    ap->ap_compat=compat;ap->ap_mac=1;p->p_cursig=signal;
    u.u_sigrestart=mask;aux_polldiag=diagnostic;
    (void)a;
''' + restart + r'''
    goto done;
out:
    restarted=1;
done:
    assert(restarted==expected_restart);
    assert(pc==(expected_restart ? 0x102 : 0x104));
    assert(e==(expected_restart ? error : EINTR));
    checks++;
}
int main(void)
{
    int errors[]={EINTR,ERESTART},i,j,d;
    int aliases[]={38,50};
    int all=COMPAT_BSDSIGNALS|COMPAT_SYSCALLS;
    long tickmask=1L<<5;
    for(d=0;d<2;d++)for(i=0;i<2;i++) {
        for(j=0;j<2;j++) {
            check(aliases[j],0x6e,errors[i],all,6,tickmask,0,d);
            check(aliases[j],0x69,errors[i],all,6,tickmask,1,d);
            check(aliases[j],0x6e,errors[i],COMPAT_BSDSIGNALS,6,tickmask,0,d);
        }
        check(3,0,errors[i],COMPAT_BSDSIGNALS,6,tickmask,1,d);
        check(7,0,errors[i],COMPAT_BSDSIGNALS,6,tickmask,1,d);
        check(82,0,errors[i],all,6,tickmask,0,d);
        check(3,0,errors[i],COMPAT_SYSCALLS,6,tickmask,0,d);
        check(3,0,errors[i],all,0,tickmask,0,d);
        check(3,0,errors[i],all,6,0,0,d);
        check(38,0x69,errors[i],COMPAT_BSDSIGNALS,6,tickmask,0,d);
    }
    printf("pollrestart: %d PASS\n",checks);
    return 0;
}
'''
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix="pollrestart-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(source)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu89", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-g", "-o", str(path / "test"), str(path / "test.c")], check=True)
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    subprocess.run([str(path / "test")], check=True, env=env)
