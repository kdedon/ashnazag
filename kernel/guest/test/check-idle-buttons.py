#!/usr/bin/env python3
import pathlib,sys,tempfile,subprocess
s=pathlib.Path(sys.argv[1]).read_text();a=s.index('long uin_evbuttoncount');z=s.index('/* append; a full queue',a);f=s[a:z]
p=r'''
#include <assert.h>
#include <string.h>
long pc=123,lbolt=42;int in_flags=7,in_btn,in_btnw,ev_n=2,uin_evdiag;
struct {int l_evmask;}ui;struct {char*u_ar0;}u;
#define GR_PC(r) pc
'''
t=r'''
int main(void){int i;ui.l_evmask=0xffef;ui_evbutton(1,2,1);assert(!uin_evbuttoncount);uin_evdiag=1;
for(i=1;i<=4;i++){ui_evbutton(i,2,4);assert(uin_evbuttons[i-1][0]==i&&uin_evbuttons[i-1][1]==2&&uin_evbuttons[i-1][2]==7&&uin_evbuttons[i-1][3]==0xffef&&uin_evbuttons[i-1][4]==2&&uin_evbuttons[i-1][5]==42&&uin_evbuttons[i-1][6]==((i==1||i==4)?0:123)&&uin_evbuttons[i-1][7]==4);}
for(i=0;i<10;i++)ui_evbutton(1,2,0);assert(uin_evbuttoncount==8);uin_evbuttoncount=-1;ui_evbutton(1,2,0);assert(uin_evbuttoncount==-1);return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 c=pathlib.Path(d)/'t.c';c.write_text(p+f+t);o=pathlib.Path(d)/'t';subprocess.run(['cc','-std=gnu89','-fsanitize=undefined',str(c),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
print('idle-buttons: 7 production record cases PASS')
