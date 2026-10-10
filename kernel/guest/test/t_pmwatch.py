#!/usr/bin/env python3
"""Check version-scoped scheduler watching and foreign-address-space holds."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
p=Path(sys.argv[1]) if len(sys.argv)>1 else root/'mod/uinter/uipatch.c'
s=p.read_text();body=s[s.index('static int\npmwatchneeded('):s.index('void\nui_fronttick(')]
assert s.count('pmwatchneeded(gp)')==4
h=r"""
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef char *caddr_t;
struct proc {int id;};
struct guest_proc {struct proc *gp_proc;};
struct {struct guest_proc *l_gp,*l_active;} ui;
struct {struct proc *u_procp;} u;
int uin_pmidle=1,uin_pmidlestub=1,pmstate,sv=0x810,fault,reads;
#define G16(b) (((unsigned char)(b)[0]<<8)|(unsigned char)(b)[1])
int copyin(caddr_t a,caddr_t b,int n){assert((unsigned long)a==0x15a&&n==2);reads++;if(fault)return 1;b[0]=sv>>8;b[1]=sv;return 0;}
"""
t=r"""
int main(){struct proc p={1},other={2};struct guest_proc g={&p},foreign={&other};int n=0,versions[]={0x810,0x761,0x710,0x607,0,0x900},i;
ui.l_gp=ui.l_active=&g;u.u_procp=&p;
for(i=0;i<6;i++){sv=versions[i];assert(pmwatchneeded(&g)==(sv==0x810));n++;}
sv=0x810;fault=1;assert(pmwatchneeded(&g));fault=0;n++;
u.u_procp=&other;sv=0x761;i=reads;assert(pmwatchneeded(&g)&&reads==i);u.u_procp=&p;n++;
sv=0x810;uin_pmidle=0;i=reads;assert(!pmwatchneeded(&g)&&reads==i);uin_pmidle=1;n++;
assert(!pmwatchneeded(&foreign));n++;ui.l_active=&foreign;assert(!pmwatchneeded(&g));ui.l_active=&g;n++;
pmstate=1;assert(pmwatchneeded(&g));n++;pmstate=2;assert(!pmwatchneeded(&g));n++;
uin_pmidlestub=0;assert(pmwatchneeded(&g));n++;pmstate=3;assert(!pmwatchneeded(&g));n++;
printf("pmwatch: %d production version/owner/state checks PASS\n",n);return 0;}
"""
with tempfile.TemporaryDirectory() as d:
 a=Path(d);(a/'test.c').write_text(h+body+t)
 subprocess.run(['cc','-std=gnu89','-Wall','-Werror',str(a/'test.c'),'-o',str(a/'test')],check=True)
 subprocess.run([str(a/'test')],check=True)
