#!/usr/bin/env python3
"""Exercise the production keyboard chord admission and expiry."""
from pathlib import Path
import os
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / 'mod/uinter/uievent.c'
text = source.read_text()
start = text.index('static int forceheld, forcepending, forceenabled, forceescape;')
end = text.index('static int\nui_mods()', start)
production = text[start:end].replace('long', 'int32_t')
header = '''#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define HZ 60
#define NEV 32
#define DS_HI 0
#define GPF_STUB 0x80000000UL
#define GPF_AWATCH 0x4000
#define GPF_ALINE 1
#define UI_NTASK 2
struct guest_proc { uint32_t gp_flags; };
struct uitask { struct guest_proc *t_gp; int t_force; void *t_proc; int32_t t_forceentry; };
static struct { void *u_procp; } u;
static int entry_result, entry_calls;
static int32_t uin_forceentry=0x1000;
static int ui_forceentry(int32_t a) { assert(a==0x1000 || a==0); entry_calls++; return entry_result; }
static struct { struct uitask l_task[UI_NTASK]; struct guest_proc *l_active; } ui;
static struct uitask *ui_task(struct guest_proc *gp) {
 int i; for(i=0;i<UI_NTASK;i++) if(ui.l_task[i].t_gp==gp) return &ui.l_task[i]; return 0;
}
void ui_forcewatch(void);
static int DS_SPL(int n) { return n; }
static void DS_SPLX(int n) { (void)n; }
typedef char *caddr_t;
struct uiev { short what; long msg; };
static struct uiev ev_q[NEV];
static int ev_n,ev_first,ev_wait,in_down=-1;
static int wakes;
static void wakeup(caddr_t a) { (void)a; wakes++; }
static int32_t lbolt;
static unsigned char in_keys[16];
#define KEY(c) (in_keys[(c)>>3] & (1<<((c)&7)))
'''
tests = r'''
static void key(int c,int down) {
    if(down) in_keys[c>>3] |= 1<<(c&7);
    else in_keys[c>>3] &= ~(1<<(c&7));
    ui_forcekey();
}
int main(void) {
    int i,n=0;
    struct guest_proc a,b;
    int32_t scans;
    for(i=0;i<4;i++) {
        memset(in_keys,0,sizeof in_keys); ui_forceclear(); ui_forceenable(1); lbolt=100;
        if(i&1) key(0x38,1);
        if(i&2) key(0x3b,1);
        key(0x37,1); key(0x3a,1); key(0x35,1);
        assert(ui_forceget()==(i==0)); n++;
    }
    memset(in_keys,0,sizeof in_keys); ui_forceclear(); ui_forceenable(1); lbolt=100;
    key(0x35,1); key(0x3a,1); key(0x37,1);
    assert(ui_forceget()); ui_forceconsume(); assert(!ui_forceget()); n++;
    key(0,1); key(0,0); key(0x35,1); assert(!ui_forceget()); n++;
    key(0x35,0); key(0x35,1); assert(ui_forceget()); n++;
    ui_forceconsume(); lbolt=399; ev_wait=1; wakes=0; ui_forceretry(); assert(ui_forceget() && wakes==1); n++;
    lbolt=400; assert(!ui_forceget()); wakes=0; ui_forceretry(); assert(!ui_forceget() && !wakes); n++;
    ui_forceclear(); assert(!forceheld && !forcepending && !forceuntil); n++;
    memset(in_keys,0,sizeof in_keys); ui_forceenable(1); lbolt=0x7fffff00;
    key(0x37,1); key(0x3a,1); key(0x35,1); assert(ui_forceget()); n++;
    lbolt=(uint32_t)lbolt+300; assert(!ui_forceget()); n++;
    ui_forceclear(); memset(in_keys,0,sizeof in_keys); lbolt=100;
    key(0x37,1); key(0x3a,1); key(0x35,1); assert(!ui_forceget() && !forceescape); n++;
    ui_forceenable(1); ev_first=31; ev_n=3;
    ev_q[31].what=3; ev_q[31].msg=0x3500;
    ev_q[0].what=3; ev_q[0].msg=0x1200;
    ev_q[1].what=5; ev_q[1].msg=0x3500;
    in_down=0x35; ui_forcekey();
    assert(ui_forceget() && forceescape && in_down==-1);
    assert(ev_n==1 && ev_q[31].msg==0x1200); n++;
    ui_forceenable(0); assert(!ui_forceget() && !forceescape); n++;
    ui_forceclear(); memset(in_keys,0,sizeof in_keys); lbolt=100;
    a.gp_flags=b.gp_flags=GPF_ALINE; ui.l_task[0].t_gp=&a; ui.l_task[1].t_gp=&b;
    ui.l_active=&a; ui_forceenable(1); key(0x37,1); key(0x3a,1); key(0x35,1);
    assert((a.gp_flags&GPF_AWATCH) && !(b.gp_flags&GPF_AWATCH)); n++;
    ui.l_active=&b; ui_forcewatch();
    assert(!(a.gp_flags&GPF_AWATCH) && (b.gp_flags&GPF_AWATCH)); n++;
    lbolt=400; ui_forcewatch();
    assert(!ui_forceget() && !(b.gp_flags&GPF_AWATCH)); n++;
    key(0x35,0); lbolt=500; key(0x35,1); ui_forcestub(&b); ui_forceconsume();
    ui_forcewatch(); assert((b.gp_flags&(GPF_STUB|GPF_AWATCH))==(GPF_STUB|GPF_AWATCH)); n++;
    ui_forceend(&b); assert(!(b.gp_flags&(GPF_STUB|GPF_AWATCH))); n++;
    key(0x35,0); key(0x35,1); ui_forceclear();
    assert(!(b.gp_flags&GPF_AWATCH)); n++;
    a.gp_flags=GPF_ALINE|GPF_AWATCH; ui.l_active=&a; ui_forceenable(1);
    key(0x35,0); key(0x35,1); ui_forceconsume(); ui_forcewatch();
    assert(a.gp_flags&GPF_AWATCH); n++;
    ui_forceclear(); scans=uin_forcescan;
    ui_forcewatch(); assert(uin_forcescan==scans && !forcewatching); n++;
    memset(in_keys,0,sizeof in_keys); ui_forceenable(1); lbolt=1000;
    a.gp_flags=GPF_ALINE; ui.l_active=&a;
    key(0x37,1); key(0x3a,1); key(0x35,1);
    assert(forcewatching && (a.gp_flags&GPF_AWATCH)); n++;
    lbolt=1300; ui_forcewatch();
    assert(!forcewatching && !(a.gp_flags&GPF_AWATCH)); n++;
    scans=uin_forcescan; ui_forcewatch(); assert(uin_forcescan==scans); n++;
    ui_forceclear(); ui_forceenable(1); ui.l_active=&a;
    u.u_procp=(void *)1; ui.l_task[0].t_proc=(void *)2;
    ui_forcereserve(0x1000); assert(!ui.l_task[0].t_forceentry); n++;
    ui.l_task[0].t_proc=u.u_procp; ui_forcereserve(0x1000);
    assert(ui.l_task[0].t_forceentry==0x1000); n++;
    lbolt=1400; forceuntil=1700; forcepending=1; entry_result=0; entry_calls=0;
    ui_forcepublish(&b); assert(!entry_calls && forcepending); n++;
    ui_forcepublish(&a); assert(entry_calls==1 && !forcepending && uin_forcefailed==1); n++;
    forcepending=1;
    entry_result=1; ui_forcepublish(&a);
    assert(!forcepending && uin_forcepublish==1); n++;
    forcepending=1; entry_result=2; ui_forcepublish(&a);
    assert(!forcepending && uin_forcepublish==1); n++;
    ui.l_task[0].t_forceentry=0; forcepending=1; entry_result=1;
    ui_forcepublish(&a); assert(!forcepending && uin_forcepublish==2); n++;
    ui_forceconsume(); ui_forcewatch(); a.gp_flags=GPF_ALINE;
    ui.l_task[0].t_force=0; ui_forcepollwatch(&a,1);
    assert(ui_forcepollowned(&a) && (a.gp_flags&GPF_AWATCH) && !(a.gp_flags&GPF_STUB)); n++;
    ui_forcewatch(); assert(a.gp_flags&GPF_AWATCH); n++;
    ui_forcepollwatch(&a,0); assert(!ui_forcepollowned(&a) && !(a.gp_flags&GPF_AWATCH)); n++;
    a.gp_flags|=GPF_AWATCH; ui_forcepollwatch(&a,1); ui_forcepollwatch(&a,0);
    assert(a.gp_flags&GPF_AWATCH); n++;
    ui.l_task[0].t_proc=(void *)2; ui_forcepollwatch(&a,1);
    assert(!ui_forcepollowned(&a)); n++;
    printf("forcekeys: %d checks passed\n",n);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='forcekeys-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(header + production + tests)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu89', '-fwrapv', '-Wall', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
