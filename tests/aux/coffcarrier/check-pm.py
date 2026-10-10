#!/usr/bin/env python3
"""Exercise the bounded process-table diagnostic reader."""
import pathlib,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parents[3]
s=(root/'tests/src/t_mac.c').read_text()
mock=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <sys/types.h>
static unsigned char memory[0x20000];static long cursor;static int shortread,codes,rows;
long lseek(int fd,long off,int whence){assert(fd==3 && whence==0);cursor=off;return off;}
long read(int fd,char *out,long n){assert(fd==3);if(shortread || cursor<0 || cursor+n>sizeof memory)return 0;memcpy(out,memory+cursor,n);return n;}
void t_info(char *name,char *fmt,...){if(!strcmp(name,"carrier_pm_selector"))rows++;if(!strcmp(name,"carrier_pm_code"))codes++;}
static void p32(long a,long n){memory[a]=n>>24;memory[a+1]=n>>16;memory[a+2]=n>>8;memory[a+3]=n;}
int main(void){long a;int i;int sel[]={16,17,55,59};
 p32(0xb7c,0x15000);for(i=0;i<4;i++){a=0x15000-TABLEBASE+8*sel[i];p32(a,0x18000+16*i);memory[a+5]=4;memory[a+7]=2;}
 coffpm81(3);assert(rows==4 && codes==4);
 rows=codes=0;p32(0xb7c,0);coffpm81(3);assert(!rows && !codes);
 p32(0xb7c,0x1000000);coffpm81(3);assert(!rows && !codes);
 p32(0xb7c,0x15000);shortread=1;coffpm81(3);assert(!rows && !codes);
 shortread=0;p32(0x15000-TABLEBASE+55*8,0x18001);coffpm81(3);assert(rows==4 && codes==3);
 puts("carrier: 5 process-table reader cases PASS");return 0;}
'''
for name,tail,base in [('coffpm81','static void\ncommanddirect81once()',3308),('coffpm76','static int\ncommandpid76()',3330)]:
 fn=s[s.index('static void\n'+name+'(fd)'):s.index(tail)]
 with tempfile.TemporaryDirectory() as d:
  p=pathlib.Path(d)
  text=mock.replace('TABLEBASE',str(base)).replace('coffpm81(',name+'(')
  (p/'t.c').write_text(text.replace('int main(void)',fn+'int main(void)',1))
  subprocess.run(['cc','-std=gnu89','-w',str(p/'t.c'),'-o',str(p/'t')],check=True)
  subprocess.run([str(p/'t')],check=True)
