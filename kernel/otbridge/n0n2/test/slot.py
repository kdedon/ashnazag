from pathlib import Path
import subprocess,sys
root=Path(__file__).resolve().parents[4]
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
s=(root/'kernel/guest/mod/uinter/uivideo.c').read_text()
s=s[s.index('#define\tUV_SLOT'):s.index('/* syscall 66 */')]
prefix=r'''
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef char *caddr_t;
typedef unsigned int u_int;
#define FBF_CMAP 1
struct fbinfo {int fi_depth,fi_flags,fi_width,fi_height,fi_rowbytes;};
struct {void *u_procp;} u;
struct {unsigned char l_pram[256];} ui;
static unsigned char output[4096];
#define G8(p) (*(unsigned char *)(p))
#define G16(p) ((uint32_t)G8(p)<<8 | G8((char *)(p)+1))
#define G32(p) (G16(p)<<16 | G16((char *)(p)+2))
#define P8(p,v) (*(unsigned char *)(p)=(v))
#define P16(p,v) do {uint32_t x=(v);P8(p,x>>8);P8((char *)(p)+1,x);} while(0)
#define P32(p,v) do {uint32_t y=(v);P16(p,y>>16);P16((char *)(p)+2,y);} while(0)
static int copyout(const void *src,void *dst,int n) {if((uintptr_t)dst!=0x1000||n>sizeof output)return 1;memcpy(output,src,n);return 0;}
static int copyin(void *src,void *dst,int n) {return 1;}
'''
p=out/'slot-production.c';p.write_text(prefix+s+'\n'+(root/'kernel/otbridge/n0n2/test/slot.c').read_text())
subprocess.run(['nice','-n19','cc','-std=gnu89','-w','-O1','-g','-fsanitize=address,undefined','-I'+str(root/'kernel/guest/mod/uinter'),str(p),'-o',str(out/'slot-test')],check=True)
subprocess.run([str(out/'slot-test')],check=True,env={'ASAN_OPTIONS':'detect_leaks=0'})
