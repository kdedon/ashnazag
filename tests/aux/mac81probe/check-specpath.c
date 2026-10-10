#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
typedef char *caddr_t;
#define CINFO 108
#define FSIZE 96
#define MPATH 13
#define MOPEN 3
#define FNFERR (-43)
#define PARAMERR (-50)
#define WDCBS 0x372
#define GR_A(r,n) (((long *)(r))[8+(n)])
#define GR_D(r,n) (((long *)(r))[(n)])
#define GR_USP(r) (((long *)(r))[16])
#define GR_PC(r) (((long *)(r))[17])
#define GR_SR(r) (((long *)(r))[18])
static unsigned char mem[65536];
static long trashdir,trashpar;
static short trashvref;
static int pending, checks, failout, outcalls, auxcalls, nativevolume;
static unsigned short catcall[6];
static unsigned long G32(const unsigned char *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static unsigned G16(const unsigned char *p) {return p[0]<<8|p[1];}
static void P16(unsigned char *p,long n) {p[0]=n>>8;p[1]=n;}
static void P32(unsigned char *p,long n) {p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static int copyin(caddr_t a,caddr_t b,int n) {uintptr_t v=(uintptr_t)a;if(v+n>sizeof mem)return 1;memcpy(b,mem+v,n);return 0;}
static int copyout(caddr_t a,caddr_t b,int n) {uintptr_t v=(uintptr_t)b;if(++outcalls==failout)return 1;if(v+n>sizeof mem)return 1;memcpy(mem+v,a,n);return 0;}
static int ui_auxcall(char *r,int a,long b,unsigned short *code,int mark,int ext) {auxcalls++;GR_USP(r)=4000;pending=mark;return 1;}
static long extvcb(long a,int sel) {short v=G16(mem+a+0x16);return v==-7||v==-9||v==0||v==-32767?600:0;}
/* PRODUCTION */
struct entry {long id,parent; const char *name; int directory;};
static struct entry entries[]={{2,1,"Unix",1},{10,2,"Docs",1},{11,10,"Sub",1},{20,10,"file",0},{21,11,"deep",0}};
static void need(int ok,const char *label) {checks++;if(!ok){fprintf(stderr,"FAIL %s\n",label);exit(1);}}
static void answer(void) {
 unsigned char *c=mem+4096,*nm=mem+4096+PATHPB;struct uispecpath state, *s=&state;int i,err=-43;long dir=G32(c+0x30);short idx=G16(c+0x1c);
 memcpy(s,mem+4096+PATHPB+256,sizeof *s);
 if(s->stage==P_VOLUME){err=nm[0]&&memcmp(nm+1,"Unix:",5)?-35:0;P16(c+0x16,nativevolume?-8:-7);}
 else if(s->stage==P_WORKING){err=0;P16(c+0x20,-7);P32(c+0x30,s->vref==-32000||s->vref==-32767?10:2);}
 else if(s->stage==P_DEFAULT){err=0;P16(c+0x16,-7);P16(c+0x20,-7);P32(c+0x30,10);}
 else {
   if(nm[0]==6&&!memcmp(nm+1,"denied",6))err=-54;
   else for(i=0;i<5;i++) if(idx==-1?entries[i].id==dir:entries[i].parent==dir&&nm[0]==strlen(entries[i].name)&&!memcmp(nm+1,entries[i].name,nm[0])) {
    err=0;c[0x1e]=entries[i].directory?16:0;P32(c+0x30,entries[i].id);P32(c+0x64,entries[i].parent);nm[0]=strlen(entries[i].name);memcpy(nm+1,entries[i].name,nm[0]);break;
   }
 }
 P16(c+0x10,err);
}
static void test(const char *path,short vref,long dir,int err,long parent,const char *leaf) {
 long reg[20]={0};unsigned char nm[256],*pb=mem+1000;int n=strlen(path),step=0,handled; memset(mem,0,sizeof mem);memset(mem+2000,0xcc,70);P32(mem+WDCBS,700);P32(mem+704,10);P16(mem+600+0x4e,-7);P16(pb+0x16,vref);P32(pb+0x30,dir);P32(pb+0x1c,2000);nm[0]=n;memcpy(nm+1,path,n);reg[8]=1000;reg[16]=4096+PATHX;reg[17]=100;pending=0;
 handled=pathspec((char *)reg,pb,nm);need(handled,path);
 while(pending==MPATH&&step++<260){pending=0;answer();reg[16]=4096+PATHX;pathnext((char *)reg,4096);}
 need(step<260,"bounded");need((short)G16(pb+0x10)==err,path);need(reg[17]==102,"PC");
 if(leaf){need((short)G16(mem+2000)==-7,"real volume");need(G32(mem+2002)==parent,"parent");need(mem[2006]==strlen(leaf)&&!memcmp(mem+2007,leaf,strlen(leaf)),"leaf");}
 else need(mem[2000]==0xcc,"no invalid output");
}
/* PRODUCTION_GUARDS */
static void faulttests(void) {
 int k,i;long r[20],save[20];unsigned char pb[52]={0},name[]={5,':','D','o','c','s'};struct uispecpath s;
 for(k=1;k<=3;k++) {
  memset(r,0,sizeof r);r[8]=1000;r[16]=4096+PATHX;r[17]=100;memcpy(save,r,sizeof r);P16(pb+22,-7);P32(pb+48,2);
  outcalls=auxcalls=pending=0;failout=k;
  need(pathspec((char *)r,pb,name)==0,"initial state fault");need(!memcmp(r,save,sizeof r),"initial registers unchanged");need(!auxcalls&&!pending,"no incomplete guest request");
 }
 failout=0;outcalls=auxcalls=pending=0;memset(r,0,sizeof r);r[8]=1000;r[16]=4096+PATHX;r[17]=100;memcpy(mem+1000,pb,sizeof pb);
 need(pathspec((char *)r,pb,name)==1,"prepare continuation");answer();r[16]=4096+PATHX;pending=0;failout=outcalls+1;
 need(pathnext((char *)r,4096)==1,"continued state fault done");need((short)r[0]==-50&&r[17]==102&&!pending,"continued fault no guest execution");
 failout=0;nativevolume=1;memset(&s,0,sizeof s);s.path[0]=10;s.pos=6;s.stage=P_VOLUME;
 memcpy(mem+4096+PATHPB+256,&s,sizeof s);P16(mem+4096+0x10,0);P16(mem+4096+0x16,-8);r[16]=4096+PATHX;r[17]=100;
 need(pathnext((char *)r,4096)==2&&pending==MOPEN,"native fallback retains stub");nativevolume=0;
}
int main(void) {
 test(":Docs:file",-7,2,0,10,"file");test("Unix:Docs:Sub:deep",-9,2,0,11,"deep");test("::file",-7,11,0,10,"file");test(":Docs:",-7,2,0,2,"Docs");test("Unix:",-7,2,0,1,"Unix");test(":",-7,10,0,2,"Docs");test("::",-7,11,0,2,"Docs");test(":missing",-7,10,-43,10,"missing");test(":missing:leaf",-7,10,-43,0,NULL);test(":file:leaf",-7,10,-120,0,NULL);test(":denied:leaf",-7,10,-54,0,NULL);test(":missing",-7,999,-43,0,NULL);test("Other:file",-7,2,-35,0,NULL);test(":file",0,0,0,10,"file");test(":file",1,10,0,10,"file");test(":file",-32767,0,0,10,"file");test(":file",-32000,0,0,10,"file");test(":::Docs:file",-7,11,0,10,"file");
 {char s[80];s[0]=':';memset(s+1,'x',64);s[65]=0;test(s,-7,10,-37,0,NULL);}
 trashdir=11;trashpar=10;trashvref=-7;test(":Sub:",-7,10,0,2,"Sub");trashdir=0;
 faulttests();guardtests();
 printf("FSMakeFSSpec continuation: %d checks PASS\n",checks);return 0;
}
