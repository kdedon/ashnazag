import pathlib,subprocess,tempfile,sys
s=pathlib.Path(sys.argv[1]).read_text();part=s[s.index('long\nsdev(pa, storage)'):s.index('/* 1 if the Sound Manager')]
assert 'probe_write' not in part
assert "trace('q');\n    probe_write();" in s
head=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define G16(p) (*(unsigned short *)(p))
#define G32(p) (*(unsigned long *)(p))
#define noErr 0
#define siUnknownInfoType (-231)
#define badComponentSelector 0x80008002L
unsigned long probe[22];int probe_enabled=1;long mixer,outdesc,running,openerror;int delegated,kicked;
void trace(int x){}long closemixer(long x){return 0;}
long openmixer(void *d,long f,long *m){if(!openerror)*m=42;return openerror;}
long delegate(char *p,long m){delegated++;return 0;}
void kick(void){kicked++;}long getinfo(char *p,long s,char *v){return 0;}long setinfo(char *p,long s,long v){return 0;}
'''
main=r'''
int main(void){unsigned long p[8];char *pa=(char *)p;memset(p,0,sizeof p);
G16(pa+2)=-1;assert(sdev(pa,0)==0&&probe[13]==1&&(long)probe[12]==-1);
G16(pa+2)=1;openerror=-2004;assert(sdev(pa,0)==-2004&&probe[14]==1&&(long)probe[15]==-2004&&!mixer);
openerror=0;assert(sdev(pa,0)==0&&mixer==42&&probe[14]==2&&!probe[15]);
G16(pa+2)=0x101;assert(sdev(pa,1)==0&&delegated==1&&probe[21]==1&&probe[17]==0x101);
G16(pa+2)=0x105;assert(sdev(pa,0)==0&&kicked==1);
G16(pa+2)=-2;assert(sdev(pa,0)==0&&!mixer&&!running);
G16(pa+2)=0x102;assert(sdev(pa,0)==badComponentSelector&&probe[16]==badComponentSelector&&probe[17]==0x102);
G16(pa+2)=-3;G16(pa+4)=1;assert(sdev(pa,0)==1&&probe[18]==1);
probe_enabled=0;G16(pa+2)=-1;assert(sdev(pa,0)==0&&probe[13]==1);puts("PASS 9 actual component diagnostic/dispatch cases; entry writes no probe file");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='component-proof-') as d:
 p=pathlib.Path(d);(p/'t.c').write_text(head+part+main)
 subprocess.run(['gcc','-std=gnu89','-w','-fsanitize=undefined','-fno-sanitize-recover=all',str(p/'t.c'),'-o',str(p/'t')],check=True);subprocess.run([str(p/'t')],check=True)
