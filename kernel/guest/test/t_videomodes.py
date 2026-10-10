from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'kernel/guest/mod/uinter/uivideo.c').read_text()
s=s[s.index('static int\nuv_status('):s.index('/* UI_VIDEO_CONTROL')]
header=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef char *caddr_t;
#define EFAULT 14
#define UV_MODE 0x80
#define G16(b) ((uint16_t)(uint8_t)(b)[0]<<8|(uint8_t)(b)[1])
#define G32(b) ((uint32_t)G16(b)<<16|G16((b)+2))
#define P16(b,n) ((b)[0]=(uint8_t)((uint16_t)(n)>>8),(b)[1]=(uint8_t)(n))
#define P32(b,n) (P16(b,(uint32_t)(n)>>16),P16((b)+2,n))
static struct { uint32_t v_base; int v_depth,v_gray,v_width,v_height; unsigned char v_decl[2048]; } uv;
static unsigned char mem[64],vpmem[64];
static int readfault,writefault,writes,lastsize;
static int copyin(char *p,char *b,int n) {
 if(readfault) return 1;
 assert(p==(char *)mem);memcpy(b,p,n);return 0;
}
static int copyout(char *b,char *p,int n) {
 if(writefault)return 1;
 if(p==(char *)8192) { assert(n==42);p=(char *)vpmem; }
 else assert(p>=(char *)mem && p+n<=(char *)mem+sizeof mem);
 memcpy(p,b,n);writes++;lastsize=n;return 0;
}
static int uv_pageinfo(char *p) {(void)p;return 67;}
static int uv_entries(char *p,int put) {(void)p;(void)put;return 68;}
'''
tests=r'''
int main(void) {
 int depths[]={1,2,4,8,16,24,32},i,j,n=0;
 uv.v_base=0xe0000e00U;
 for(i=0;i<7;i++) {
  uv.v_depth=depths[i];memset(mem,0xa5,sizeof mem);
  assert(!uv_status(10,(char *)mem) && lastsize==16);
  assert(G16(mem)==0x80 && G32(mem+2)==0x80 && !G16(mem+6));
  assert(G32(mem+8)==0xe0000e00U && !G32(mem+12) && mem[16]==0xa5);n+=3;
  memset(mem,0xa5,sizeof mem);
  assert(!uv_status(12,(char *)mem) && lastsize==12);
  assert(G16(mem)==8 && !mem[2] && !mem[3]);
  assert(G32(mem+4)==(i?0x43U:0x53U) && !G32(mem+8) && mem[12]==0xa5);n+=3;
 }
 memset(mem,0xa5,sizeof mem);P32(mem,0x80);
 assert(!uv_status(13,(char *)mem) && lastsize==20);
 assert(G32(mem)==0x80 && !G32(mem+4) && G32(mem+8)==0x6465636cU);
 assert(!G32(mem+12) && G32(mem+16)==15 && mem[20]==0xa5);n+=3;
 P32(mem,0x81);i=writes;
 assert(uv_status(13,(char *)mem)==-50 && writes==i && G32(mem)==0x81);n++;
 readfault=1;assert(uv_status(13,(char *)mem)==EFAULT && writes==i);n++;
 readfault=0;writefault=1;P32(mem,0x80);
 assert(uv_status(10,(char *)mem)==EFAULT);
 assert(uv_status(12,(char *)mem)==EFAULT);
 assert(uv_status(13,(char *)mem)==EFAULT && writes==i);n+=3;
 writefault=0;uv.v_depth=8;
 uv.v_width=800;uv.v_height=600;
 memset(mem,0xa5,sizeof mem);
 assert(!uv_status(16,(char *)mem) && lastsize==16);
 assert(G16(mem)==128 && G32(mem+2)==128 && G32(mem+8)==uv.v_base && !G32(mem+12) && mem[16]==0xa5);n+=2;
 for(j=0;j<42;j++) uv.v_decl[0x9c+j]=j;
 P32(uv.v_decl+0x90,0x04000000);
 for(i=0;i<2;i++) {
  memset(mem,0xa5,sizeof mem);memset(vpmem,0xa5,sizeof vpmem);P32(mem,i?128:0);P16(mem+4,128);P32(mem+6,8192);
  assert(!uv_status(18,(char *)mem) && lastsize==22);
  assert(G32(mem)==(i?128:0) && G16(mem+4)==128 && G32(mem+6)==8192);
  assert(G32(mem+10)==1 && !G32(mem+14) && !G32(mem+18) && mem[22]==0xa5);
  assert(!memcmp(vpmem,uv.v_decl+0x9c,42) && vpmem[42]==0xa5);n+=4;
 }
 for(i=0;i<3;i++) {
  memset(mem,0xa5,sizeof mem);P32(mem,i==0?0xfffffffeU:i==1?0:128);
  assert(!uv_status(17,(char *)mem) && lastsize==30 && mem[30]==0xa5);
  assert(G32(mem+4)==(i==2?0xfffffffdU:128));
  assert(G32(mem+8)==(i==2?0:800) && G32(mem+12)==(i==2?0:600));
  assert(!G32(mem+16) && G16(mem+20)==(i==2?0:128) && !G32(mem+22) && !G32(mem+26));n+=4;
 }
 for(i=0;i<3;i++) {
  memset(mem,0xa5,sizeof mem);P32(mem,i==0?129:128);P16(mem+4,i==1?129:128);P32(mem+6,i==2?0:8192);j=writes;
  assert(uv_status(18,(char *)mem)==-50 && writes==j);n++;
 }
 P32(mem,129);j=writes;assert(uv_status(17,(char *)mem)==-50 && writes==j);n++;
 readfault=1;assert(uv_status(17,(char *)mem)==EFAULT && uv_status(18,(char *)mem)==EFAULT);n++;readfault=0;
 P32(mem,128);P16(mem+4,128);P32(mem+6,8192);writefault=1;
 assert(uv_status(16,(char *)mem)==EFAULT && uv_status(17,(char *)mem)==EFAULT && uv_status(18,(char *)mem)==EFAULT);n++;writefault=0;
 memset(mem,0xa5,sizeof mem);P32(mem,0xffffffffU);
 assert(!uv_status(28,(char *)mem) && lastsize==4 && G32(mem)==1 && mem[4]==0xa5);n++;
 for(i=0;i<7;i++) {
  uv.v_depth=depths[i];memset(mem,0xa5,sizeof mem);P32(mem,1);
  assert(!uv_status(28,(char *)mem) && lastsize==16 && G32(mem)==1);
  assert(G16(mem+4)==8 && !mem[6] && !mem[7] && G32(mem+8)==(i?0x43U:0x53U));
  assert(!G32(mem+12) && mem[16]==0xa5);n+=3;
 }
 for(i=0;i<3;i++) {P32(mem,i?i+1:0);j=writes;assert(uv_status(28,(char *)mem)==-50 && writes==j);n++;}
 readfault=1;assert(uv_status(28,(char *)mem)==EFAULT);readfault=0;n++;
 P32(mem,1);writefault=1;assert(uv_status(28,(char *)mem)==EFAULT);P32(mem,0xffffffffU);
 assert(uv_status(28,(char *)mem)==EFAULT);writefault=0;n+=2;
 uv.v_depth=8;
 assert(uv_status(2,(char *)mem)==67 && uv_status(3,(char *)mem)==68);
 assert(uv_status(31,(char *)mem)==-18);n+=2;
 printf("video mode ABI: %d checks passed\n",n);return 0;
}'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(header+s+tests)
 subprocess.run(['cc','-std=gnu89','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
