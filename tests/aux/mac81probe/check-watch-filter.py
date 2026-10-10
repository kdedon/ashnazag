#!/usr/bin/env python3
"""Compare filtered and unfiltered production matchers on identical frames."""
import ast,os,pathlib,subprocess,sys,tempfile
source=pathlib.Path(sys.argv[1]);fixture=pathlib.Path(sys.argv[2]);s=source.read_text()
values={}
for n in ast.parse(fixture.read_text()).body:
 if isinstance(n,ast.Assign) and isinstance(n.value,ast.Constant) and isinstance(n.value.value,str):
  for target in n.targets:
   if isinstance(target,ast.Name):values[target.id]=n.value.value
header,tests=values['header'],values['tests']
a=s.index('\nui_alwatch(gp, r)');a=s.rfind('\nint',0,a);b=s.index('{',a);j=b+1;depth=1
while depth:depth+=(s[j]=='{')-(s[j]=='}');j+=1
legacy=s[a:j].replace('ui_alwatch(gp, r)','ui_alwatch_legacy(gp, r)',1)
line='    if(!ui_trapinterest(gp,G16(b))) {uin_watchskipped++;return 0;}\n'
assert line in legacy;legacy=legacy.replace(line,'',1)
extra=r'''
    {
      char oldregs[80];unsigned char oldcode[256];unsigned long oldflags,oldpatched;
      int oldret,oldwrites,newret,k;long skipped=uin_watchskipped;
      for(k=0;k<NPATCH;k++) {
        if(!uipatches[k].p_code)continue;
        setup(&gp,r,k);oldret=ui_alwatch_legacy(&gp,r);memcpy(oldregs,r,80);
        memcpy(oldcode,memory+0x10000,256);oldflags=gp.gp_flags;oldwrites=writes;oldpatched=uinter_patched;
        setup(&gp,r,k);newret=ui_alwatch(&gp,r);
        assert(newret==oldret&&!memcmp(oldregs,r,80)&&!memcmp(oldcode,memory+0x10000,256));
        assert(gp.gp_flags==oldflags&&writes==oldwrites&&uinter_patched==oldpatched);checks++;
      }
      memset(memory,0,sizeof memory);P16(memory+0x15a,0x0810);uinter_patches=1;
      for(k=0xa000;k<=0xafff;k++) {
        memset(r,0,80);GR_PC(r)=0x10000;GR_USP(r)=0x80000;P16(memory+0x10000,k);
        gp.gp_flags=GPF_AWATCH|(GPF_DONE(NPATCH)-GPF_DONE(0));
        gp.gp_flags&=~(GPF_DONE(8)|GPF_DONE(11)|GPF_DONE(13));gp.gp_vsr=0;
        oldret=ui_alwatch_legacy(&gp,r);memcpy(oldregs,r,80);oldflags=gp.gp_flags;
        memset(r,0,80);GR_PC(r)=0x10000;GR_USP(r)=0x80000;
        gp.gp_flags=GPF_AWATCH|(GPF_DONE(NPATCH)-GPF_DONE(0));
        gp.gp_flags&=~(GPF_DONE(8)|GPF_DONE(11)|GPF_DONE(13));
        newret=ui_alwatch(&gp,r);
        assert(newret==oldret&&!memcmp(oldregs,r,80)&&gp.gp_flags==oldflags);checks++;
      }
      assert(uin_watchskipped-skipped>4000);
      gp.gp_flags=GPF_AWATCH|GPF_DONE(3)|GPF_DONE(5);
      assert(!ui_trapinterest(&gp,0xa123));checks++;
      gp.gp_flags|=GPF_STUB;assert(ui_trapinterest(&gp,0xa123));gp.gp_flags&=~GPF_STUB;checks++;
      testaux.ap_mac=APM_COFF;assert(ui_trapinterest(&gp,0xa123));testaux.ap_mac=0;checks++;
      uinter_trace=1;assert(!ui_trapinterest(&gp,0xa123));assert(ui_trapinterest(&gp,0xa9c9));uinter_trace=0;checks+=2;
      uinter_aslmtrace=1;assert(!ui_trapinterest(&gp,0xa123));assert(ui_trapinterest(&gp,0xa80e));
      assert(ui_trapinterest(&gp,0xa81f));assert(ui_trapinterest(&gp,RESUME));uinter_aslmtrace=0;checks+=4;
      uin_coffdiag=1;coffdiagowner=&gp;assert(ui_trapinterest(&gp,0xa123));coffdiagowner=0;uin_coffdiag=0;checks++;
      forcequeue=1;assert(ui_trapinterest(&gp,0xa123));forcequeue=0;checks++;
      uinter_patches=0;assert(ui_trapinterest(&gp,0xa123));uinter_patches=1;checks++;
      gp.gp_flags|=GPF_DONE(NPATCH)-GPF_DONE(0);assert(ui_trapinterest(&gp,0xa123));checks++;
      gp.gp_flags=GPF_DONE(3)|GPF_DONE(5);testaux.ap_mac=0;
      assert(ui_trapinterest(&gp,SPACETRAP));checks++;
      gp.gp_flags=0;assert(ui_trapinterest(&gp,0xafff));checks++;

      printf("filter: %ld of4096 unrelated traps rejected before full matcher\n",uin_watchskipped-skipped);
    }
'''
needle='    for(i=0;i<NPATCH;i++) {\n        if(!uipatches[i].p_code)';assert needle in tests;tests=tests.replace(needle,extra+needle,1)
s=s.replace('#include "uinter.h"','').replace('#include "sys/mman.h"','').replace('#include "sys/resource.h"','').replace('#include "vm/as.h"','')
with tempfile.TemporaryDirectory() as tmp:
 p=pathlib.Path(tmp);(p/'test.c').write_text(header+s+legacy+tests)
 subprocess.run(['cc','-std=gnu89','-w','-fno-strict-aliasing','-fsanitize=address,undefined','-fno-sanitize=alignment','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0');subprocess.run([str(p/'test')],env=env,check=True)
