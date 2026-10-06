#include "../hostfswire.h"
typedef unsigned int U;
typedef int L;
typedef unsigned char B;
extern void *allocmem(U), *findtask(void), *getmsg(void *), *createport(void), *createio(void *);
extern void freemem(void *,U),waitport(void *),putmsg(void *,void *),deleteio(void *),deleteport(void *),closedevice(void *);
extern void forbid(void),permit(void),closelibrary(void *);
extern void *openlibrary(char *);
extern L adddosentry(void *,void *);
static U rootkey,volume;
extern L opendevice(char *,void *),doio(void *);
struct packet { void *link,*port; L type,res1,res2,arg[7]; };
struct lock { U next,key; L access; void *task; U volume; };
typedef char abi_word[(sizeof(U)==4 && sizeof(void *)==4)?1:-1];
typedef char abi_packet[(sizeof(struct packet)==48)?1:-1];
typedef char abi_lock[(sizeof(struct lock)==20)?1:-1];
typedef char abi_wire[(__builtin_offsetof(struct mig_fs_mailbox,request)==16)?1:-1];
static void zero(void *p,U n) { B *b=p; while(n--) *b++=0; }
static void copy(void *d,const void *s,U n) { B *a=d; const B *b=s; while(n--) *a++=*b++; }
static void *ptr(U b) { return (void *)(b<<2); }
static U bptr(void *p) { return (U)p>>2; }
static U key(U b) { return b ? ((struct lock *)ptr(b))->key : rootkey; }
/* strips a device prefix; nonzero for a leading colon, the volume root.
 * Other prefixes (assigns) are relative to the lock DOS passes. */
static U bstring(char *dst,U src) {
 B *s=ptr(src); U n,i,skip=0;
 if(!s) { *dst=0; return 0; }
 n=*s++;
 for(i=0;i<n;i++) if(s[i]==':') { skip=i+1; break; }
 copy(dst,s+skip,n-skip); dst[n-skip]=0; return skip==1;
}
static void request(void *timer,volatile U *box) {
 MIG_FS_BARRIER();
 box[2]=MIG_FS_REQUEST;
 while(box[2]!=2) { *(unsigned short *)((B *)timer+28)=9; *(U *)((B *)timer+32)=0; *(U *)((B *)timer+36)=10000; doio(timer); }
 MIG_FS_BARRIER();
 box[2]=MIG_FS_IDLE;
}
static U newlock(U k,L access,void *port) {
 struct lock *l=allocmem(sizeof(*l));
 if(!l) return 0;
 l->key=k; l->access=access; l->task=port; l->volume=volume;
 return bptr(l);
}
static void fib(void *f,struct mig_fs_request *r) {
 B *b=f; U n=0,seconds;
 zero(f,260);
 *(L *)(b+4)=r->type; *(L *)(b+120)=r->type;
 while(r->path[n] && n<106) n++;
 b[8]=n; copy(b+9,r->path,n);
 *(U *)(b+116)=r->protect; *(U *)(b+124)=r->size; *(U *)(b+128)=(r->size+511)/512;
 seconds=r->mtime>252460800U ? r->mtime-252460800U:0;
 *(U *)(b+132)=seconds/86400; *(U *)(b+136)=(seconds%86400)/60; *(U *)(b+140)=(seconds%60)*50;
}
void handler(void) {
 volatile U *box=(volatile U *)MIG_FS_BASE;
 struct mig_fs_request *r=(void *)(box+4);
 void *port=(B *)findtask()+92,*tp=createport(),*timer=tp?createio(tp):0;
 struct packet *p; void *msg; U total,n; L op;
 L ready=timer && opendevice("timer.device",timer)==0;
 for(;;) {
  waitport(port); msg=getmsg(port); if(!msg) continue;
  p=*(struct packet **)((B *)msg+10);
  p->res1=0; p->res2=0;
  if(!ready || box[0]!=MIG_FS_MAGIC || box[1]!=MIG_FS_VERSION) { p->res2=218; goto reply; }
  if(p->type!=0 && box[3]!=(U)port) { p->res2=218; goto reply; }
  if(p->type==0) {
   forbid();
   if(box[3]) { permit(); p->res2=202; goto reply; }
   box[3]=(U)port; permit();
  }
  zero(r,__builtin_offsetof(struct mig_fs_request,data));
  op=p->type;
  switch(op) {
  case 0:
   r->op=MIG_FS_LOCK; request(timer,box);
   if(r->error) { box[3]=0; break; }
   rootkey=r->result;
   { U *v=allocmem(52); void *dos=openlibrary("dos.library");
     if(v && dos) {
      v[1]=2; v[2]=(U)port; v[8]=0x444f5301; v[10]=bptr(v+11); v[11]=0x05416d69; v[12]=0x67610000;
      if(adddosentry(dos,v)) volume=bptr(v); else freemem(v,52);
     } else if(v) freemem(v,52);
     if(dos) closelibrary(dos);
   }
   if(!volume) { r->op=MIG_FS_UNLOCK; r->handle=rootkey; request(timer,box); rootkey=0; box[3]=0; r->error=103; break; }
   if(p->arg[2]) *(void **)((B *)ptr(p->arg[2])+8)=port;
   p->res1=-1; break;
  case 7: p->res1=volume; break;
  case 25: case 26:
   r->op=MIG_FS_INFO; request(timer,box);
   if(!r->error) {
    U *info=ptr(p->arg[op==25?0:1]); zero(info,36);
    info[2]=r->flags?80:82; info[3]=r->result; info[4]=r->size;
    info[5]=r->length; info[6]=0x444f5301; info[7]=volume; info[8]=-1; p->res1=-1;
   } break;
  case 1027: case 27: p->res1=-1; break;
  /* notification requests are accepted; changes are never signalled */
  case 4097: case 4098: p->res1=-1; break;
  case 8: case 22:
   r->op=op==8?1:14; r->handle=key(p->arg[0]); if(bstring(r->path,p->arg[1])) r->handle=rootkey; r->flags=p->arg[2]==-1;
   request(timer,box); if(r->error) break;
   p->res1=newlock(r->result,op==8?p->arg[2]:-2,port);
   if(!p->res1) { r->handle=r->result; r->op=2; request(timer,box); r->error=103; }
   break;
  case 15:
   if(!p->arg[0]) { p->res1=-1; break; }
   r->op=2; r->handle=key(p->arg[0]); request(timer,box);
   if(!r->error) { freemem(ptr(p->arg[0]),sizeof(struct lock)); p->res1=-1; } break;
  case 19: case 29: case 1030: case 1031:
   r->op=(op==19 || op==1030)?3:4; r->handle=op>=1030?(U)p->arg[0]:key(p->arg[0]); request(timer,box);
   if(!r->error && r->result) { p->res1=newlock(r->result,-2,port); if(!p->res1) { r->op=2; r->handle=r->result; request(timer,box); r->error=103; }} break;
  case 1004: case 1005: case 1006:
   r->op=5; r->handle=key(p->arg[1]); r->flags=op==1005?0:op==1004?1:2; if(bstring(r->path,p->arg[2])) r->handle=rootkey; request(timer,box);
   if(!r->error) { *(void **)((B *)ptr(p->arg[0])+8)=port; *(U *)((B *)ptr(p->arg[0])+36)=r->result; p->res1=-1; } break;
  case 1007: r->op=6; r->handle=p->arg[0]; request(timer,box); if(!r->error) p->res1=-1; break;
  case 1008:
   r->op=9; r->handle=p->arg[0]; r->offset=p->arg[1]; r->flags=p->arg[2]+1; request(timer,box); p->res1=r->error?-1:r->result; break;
  case 82: case 87:
   if(p->arg[2]<0) { r->error=115; p->res1=-1; break; }
   total=0;
   while(total<(U)p->arg[2]) {
    n=(U)p->arg[2]-total; if(n>sizeof(r->data)) n=sizeof(r->data);
    r->op=op==82?7:8; r->handle=p->arg[0]; r->length=n;
    if(op==87) copy(r->data,(B *)p->arg[1]+total,n);
    request(timer,box); if(r->error) break;
    if(r->result<0 || (U)r->result>n) { r->error=212; break; }
    if(op==82) copy((B *)p->arg[1]+total,r->data,r->result);
    total+=r->result; if((U)r->result<n) break;
   }
   p->res1=total?(L)total:r->error?-1:0; break;
  case 23: case 24: case 1034:
   r->op=op==24?11:10; r->handle=op==1034?(U)p->arg[0]:key(p->arg[0]); request(timer,box);
   if(!r->error) { fib(ptr(p->arg[1]),r); p->res1=-1; } break;
  case 16:
   r->op=13; r->handle=key(p->arg[0]); if(bstring(r->path,p->arg[1])) r->handle=rootkey; request(timer,box); if(!r->error) p->res1=-1; break;
  case 17:
   r->op=12; r->handle=key(p->arg[0]); r->handle2=key(p->arg[2]); if(bstring(r->path,p->arg[1])) r->handle=rootkey; if(bstring(r->path2,p->arg[3])) r->handle2=rootkey; request(timer,box); if(!r->error) p->res1=-1; break;
  default: r->error=209;
  }
  p->res2=r->error;
reply:
  { void *dest=p->port; L failed=p->type==0 && !p->res1; p->port=port; putmsg(dest,p->link);
    if(failed) { if(ready) closedevice(timer); if(timer) deleteio(timer); if(tp) deleteport(tp); return; } }
 }
}
