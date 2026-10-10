#!/usr/bin/env python3
"""Compile actual layer teardown and owner forgetting with fatal cleanup fixtures."""
from pathlib import Path
import ast, subprocess, sys
route, layer, directory = map(Path, sys.argv[1:])
directory.mkdir(parents=True, exist_ok=True)
def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace+1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
production = '\n'.join(line for line in route.read_text().splitlines() if not line.startswith('#include'))
host = ast.parse((Path(__file__).parent/'route-host.py').read_text())
values = {node.targets[0].id: ast.literal_eval(node.value) for node in host.body if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name) and node.targets[0].id in ['prelude', 'tests']}
memory_helpers = values['prelude'][values['prelude'].index('static unsigned char memory'):values['prelude'].index('static void psignal')]
memory_helpers += 'void dlm_cacheflush(void){++flushes;}\n'
detach = function(layer.read_text(), 'static void\nuidetach(gp)')
prelude = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define SIGKILL 9
#define SIGSEGV 11
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define LS_EMPTY 0
#define LS_INUSE 2
#define UI_NTASK 3
#define APM_UIP 1
#define GPF_ALINE 1
#define GPF_PRIV 2
#define GPF_SPIN 4
#define GPF_VPEND 8
#define GPF_FAULT 16
#define GPF_AWATCH 32
#define AUXP(g) (&(g)->aux)
typedef char *caddr_t;
struct proc {int p_hold, killed;void *p_as;};
struct guest_proc {struct proc *gp_proc; int gp_flags; struct {int ap_mac;} aux;};
struct uitask {struct proc *t_proc; struct guest_proc *t_gp; int t_held,t_hold;};
static struct proc owner, peer;
static struct guest_proc guest, joined;
static struct {struct proc *u_procp;} u;
static struct {struct uitask l_task[UI_NTASK]; struct proc *l_proc,*l_uiproc,*l_romproc;
struct guest_proc *l_gp,*l_active; int l_state,l_uip,l_shmid,l_romaddr;} ui;
static int error,drops,left,stops,screens,resets,prams,aslmclears,ramdrops,gfdclears,wakes;
#define C(x) do {assert(x);++checks;} while(0)
static void psignal(struct proc *p,int signal){if(signal==SIGKILL)p->killed++;else {assert(signal==SIGSEGV);++signals;}}
static void coffdrop(struct guest_proc *gp,int main){assert(gp);assert(main==(gp==ui.l_gp));++drops;}
static struct uitask *ui_task(struct guest_proc *gp){int i;for(i=0;i<UI_NTASK;i++)if(ui.l_task[i].t_gp==gp)return &ui.l_task[i];return 0;}
static void uileave(struct uitask *t){++left;t->t_proc=0;t->t_gp=0;}
static void wakeup(caddr_t p){assert(p==(caddr_t)&ui.l_active);++wakes;}
static void uitickstop(void){++stops;}
static void ui_unscreen(int n){assert(!n);++screens;}
static void ui_inreset(void){++resets;}
static void pramout(void){++prams;}
static void ui_aslmclear(struct guest_proc *gp){assert(gp==&guest);++aslmclears;}
static void ui_rmram(struct proc *p){assert(p==&owner);++ramdrops;}
static void ui_gfdclear(void){++gfdclears;}
'''
prelude = prelude.replace('static void psignal', memory_helpers+'static void psignal', 1)
tests = r'''
static void reset(void){
memset(&ui,0,sizeof ui);memset(&owner,0,sizeof owner);memset(&peer,0,sizeof peer);
memset(&guest,0,sizeof guest);memset(&joined,0,sizeof joined);
guest.gp_proc=&owner;guest.gp_flags=63;guest.aux.ap_mac=7;joined.gp_proc=&peer;
ui.l_gp=ui.l_active=&guest;ui.l_proc=ui.l_uiproc=ui.l_romproc=&owner;ui.l_state=LS_INUSE;
ui.l_task[0].t_proc=&owner;ui.l_task[0].t_gp=&guest;ui.l_task[0].t_held=1;ui.l_task[0].t_hold=123;
ui.l_task[1].t_proc=&peer;ui.l_task[1].t_gp=&joined;ui.l_shmid=7;ui.l_romaddr=123;
u.u_procp=&owner;routeowner=0;routepc=routetail=routehelper=routelo=routehi=0;
memset(memory,0,sizeof memory);reads=writes=readfail=writefail=deny=signals=flushes=partial=0;
memcpy(memory+0x2000,routenative,28);memcpy(memory+0x4000,routestub,38);
P32(memory+0x400c,0x4026);P16(memory+0x4026,0x4ef9);P32(memory+0x4028,0x4100);
P32(memory+0x402c,0x4f544144);P16(memory+0x4030,1);P16(memory+0x15a,0x0810);
{char request[20];P32(request,0x2000);P32(request+4,0x4000);P32(request+8,0x4026);
P32(request+12,0x4000);P32(request+16,0x1000);assert(!ui_otroute(&guest,request));}
error=drops=left=stops=screens=resets=prams=aslmclears=ramdrops=gfdclears=wakes=0;
}
static void retired(void){
C(!routeowner && !routepc && !routetail && !routehelper && !routelo && !routehi);
C(!ui.l_gp && !ui.l_active && !ui.l_proc && !ui.l_romproc && !ui.l_uiproc);
C(ui.l_state==LS_EMPTY && ui.l_shmid==-1 && !ui.l_romaddr);
C(!ui.l_task[0].t_gp && !ui.l_task[1].t_gp);
C(stops==1 && screens==1 && resets==1 && wakes==1);
C(aslmclears==1 && ramdrops==1 && gfdclears==1 && prams==1 && drops==1);
C(peer.killed==1 && !guest.gp_flags && !guest.aux.ap_mac);
}
int main(void){
int failure;
reset();uidetach(&guest);C(!owner.killed);retired();
for(failure=1;failure<=5;failure++){
reset();
if(failure==1)memory[0x2008]^=1;
if(failure==2)memory[0x4000]^=1;
if(failure==3)readfail=reads+1;
if(failure==4)deny=1;
if(failure==5)writefail=1<<(writes+1);
{int oldwrites=writes;uidetach(&guest);if(failure<5)C(writes==oldwrites);}
C(owner.killed==1 && signals==1);retired();
/* The allocator can reuse gp without inheriting stale route ownership. */
memset(&guest,0,sizeof guest);guest.gp_proc=&owner;C(!ui_otrouteclear(&guest));C(!routeowner);
}
reset();ui_otrouteforget(&joined);C(routeowner==&guest && routepc==0x2000);
reset();uidetach(&joined);C(routeowner==&guest && ui.l_gp==&guest);C(!owner.killed && !peer.killed && !ramdrops);
printf("%d production teardown checks PASS\n",checks);return 0;
}
'''
source = directory/'admission-teardown.c'
source.write_text(prelude+production+'\n'+detach+tests)
exe = directory/'admission-teardown'
subprocess.run(['nice','-n19','cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-sign-compare','-fsanitize=address,undefined',str(source),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True,env={'ASAN_OPTIONS':'detect_leaks=0'})
