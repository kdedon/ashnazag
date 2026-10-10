import pathlib,subprocess,sys
source=pathlib.Path(sys.argv[1]).read_text()
start=source.index("int\nui_otacceptdiag(");end=source.index("\nstatic void\notproberecord(",start)
function=source[start:end]
work=pathlib.Path(sys.argv[2]);work.mkdir(parents=True,exist_ok=True)
harness=r'''#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define EACCES 13
#define EINVAL 22
typedef char *caddr_t;
struct guest_proc {int unused;};
static struct guest_proc owner,foreign;
static struct {struct guest_proc *l_gp;} ui;
static struct guest_proc *aslmowner;
static int uinter_aslmtrace,admitted=1,deny;
static long aslmdiagptr[3];
static unsigned char memory[1056];
#define G32(p) (((uint32_t)(p)[0]<<24)|((uint32_t)(p)[1]<<16)|((uint32_t)(p)[2]<<8)|(p)[3])
#define P32(p,v) do{uint32_t x=(v);(p)[0]=x>>24;(p)[1]=x>>16;(p)[2]=x>>8;(p)[3]=x;}while(0)
static int ui_task(struct guest_proc *p){return p==&owner&&admitted;}
static int copyin(caddr_t src,caddr_t dst,unsigned int n){uintptr_t a=(uintptr_t)src;if(deny||a<0x1000||a-0x1000>sizeof memory||n>sizeof memory-(a-0x1000))return 1;memcpy(dst,memory+a-0x1000,n);return 0;}
FUNCTION
int main(void){unsigned char b[16];int count;
ui.l_gp=aslmowner=&owner;uinter_aslmtrace=1;aslmdiagptr[1]=0x1000;
P32(memory,0x4f544447);P32(memory+4,1);P32(memory+8,1);P32(memory+896,0x4f544143);P32(memory+900,1);
for(count=0;count<=16;count++){memset(b,0,sizeof b);P32(b,1);P32(memory+920,count);assert(ui_otacceptdiag(&owner,b)==0);assert(G32(b+4)==0x1380&&G32(b+8)==160&&G32(b+12)==(unsigned)count);}
P32(memory+920,17);assert(ui_otacceptdiag(&owner,b)==EINVAL);P32(memory+920,1);
assert(ui_otacceptdiag(&foreign,b)==EACCES);admitted=0;assert(ui_otacceptdiag(&owner,b)==EACCES);admitted=1;
uinter_aslmtrace=0;assert(ui_otacceptdiag(&owner,b)==EACCES);uinter_aslmtrace=1;
P32(b,3);assert(ui_otacceptdiag(&owner,b)==EINVAL);P32(b,1);
deny=1;assert(ui_otacceptdiag(&owner,b)==EINVAL);deny=0;
P32(memory+900,2);assert(ui_otacceptdiag(&owner,b)==EINVAL);P32(memory+900,1);
P32(memory+8,2);assert(ui_otacceptdiag(&owner,b)==EINVAL);P32(memory+8,1);
aslmdiagptr[1]=0x7ffffbe0;assert(ui_otacceptdiag(&owner,b)==EINVAL);
printf("PASS 25 production accept-query authentication/count/version/fault/bounds cases\n");return 0;}
'''
p=work/'query.c';p.write_text(harness.replace('FUNCTION',function))
subprocess.run(['cc','-std=gnu89','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(work/'query')],check=True)
subprocess.run([str(work/'query')],check=True)
