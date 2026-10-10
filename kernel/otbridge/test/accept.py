from pathlib import Path
import subprocess,sys,os
r=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
h=(r/'otbridge.h').read_text();s=(r/'otxti.c').read_text()
structs=h[h.index('struct otx {'):h.index('extern struct otbs\totb_sess')]
helpers=s[s.index('static struct otx *\nbycookie'):s.index("/* bytes for the engine's copy")]
up=s[s.index('static int\notx_upmsg'):s.index('/* host messages that could not')]
pre=r'''
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define OTB_HOST 1
#include "otbridge.h"
#define OTB_NIND 8
#define M_DATA 0
#define M_PROTO 1
#define M_PCPROTO 0x83
#define BPRI_MED 1
struct queue {int unused;};typedef struct queue queue_t;
struct datab {int db_type;};
typedef struct msg {unsigned char*b_rptr,*b_wptr;struct datab*b_datap;struct msg*b_cont;} mblk_t;
'''
shim=r'''
static int live,nomem;
static mblk_t*allocb(int n,int pri){mblk_t*m;(void)pri;if(nomem)return 0;m=calloc(1,sizeof(*m)+sizeof(struct datab)+n);assert(m);m->b_datap=(struct datab*)(m+1);m->b_rptr=m->b_wptr=(unsigned char*)(m->b_datap+1);live++;return m;}
static void freeb(mblk_t*m){assert(live);live--;free(m);}
static void freemsg(mblk_t*m){mblk_t*c;while(m){c=m->b_cont;freeb(m);m=c;}}
static int msgdsize(mblk_t*m){int n=0;for(;m;m=m->b_cont)n+=m->b_wptr-m->b_rptr;return n;}
static void otb_post(struct otx*x,int event){(void)x;(void)event;}
static void uprec(queue_t*q,struct otx*x,mblk_t*m){(void)q;(void)x;freemsg(m);}
'''
p=out/'accept-production.c';p.write_text(pre+structs+shim+helpers+up+(r/'test/accept.c').read_text())
subprocess.run(['nice','-n19','cc','-std=gnu89','-w','-O1','-g','-fsanitize=address,undefined','-I'+str(r),str(p),str(r/'otxlate.c'),'-o',str(out/'accept-test')],check=True)
subprocess.run([str(out/'accept-test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
