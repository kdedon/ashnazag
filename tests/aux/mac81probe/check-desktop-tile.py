#!/usr/bin/env python3
import ast,pathlib,re,subprocess,sys,tempfile
h=pathlib.Path(sys.argv[1]).read_text();t=pathlib.Path(sys.argv[2]).read_text()
node=next(n for n in ast.parse(h).body if isinstance(n,ast.FunctionDef) and n.name=='desktile')
ns={};exec(compile(ast.Module(body=[node],type_ignores=[]),'desktile','exec'),ns)
metric=ns['desktile'];w,h=800,600
px=bytes(c for y in range(h) for x in range(w) for c in (((x%128)//16*32, (y%128)//16*32, ((x+y)%128)//16*32)))
a=metric(w,h,px);assert a[0:2]==(1000,1000) and a[2]<900
assert metric(w,h,b'\xff'*len(px))==(1000,1000,1000)
assert metric(128,128,b'\x00'*(3*128*128))==(0,0,1000)
assert metric(w,h,px[:-1])==(0,0,1000)
def paint(x0,y0,x1,y1):
 p=bytearray(px)
 for y in range(y0,y1):
  for x in range(x0,x1):p[3*(y*w+x):3*(y*w+x)+3]=b'\xff\xff\xff'
 return metric(w,h,bytes(p))
assert min(paint(280,200,520,400)[:2])<995
assert min(paint(330,250,450,310)[:2])<995
assert min(paint(390,290,400,300)[:2])>=995
noise=bytes((i*71+(i//2400)*37)%256 for i in range(3*w*h));assert min(metric(w,h,noise)[:2])<995
lines=t[t.index('#define DESKOK'):].splitlines(); parts=[]
for line in lines:
 parts.append(line)
 if not line.endswith('\\'):break
macro='\n'.join(parts)
source='''#include <assert.h>
#include <stdio.h>
#define SYSVER version
static int version;
'''+macro+'''
int main(void){version=0x810;
assert(DESKOK(552,52,999,999,560));assert(DESKOK(900,52,0,0,1000));
assert(!DESKOK(552,52,994,999,560));assert(!DESKOK(552,52,999,994,560));
assert(!DESKOK(552,52,999,999,900));assert(!DESKOK(552,19,999,999,560));
assert(!DESKOK(552,300,999,999,560));assert(!DESKOK(552,52,0,0,1000));
version=0x761;assert(DESKOK(900,900,0,0,1000));assert(!DESKOK(552,900,999,999,560));
assert(!DESKOK(900,899,999,999,560));puts("11 production desktop gate checks PASS");}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=pathlib.Path(tmp);(p/'test.c').write_text(source)
 subprocess.run(['cc','-O1','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('8 production tile metrics PASS')
if len(sys.argv)>3:
 from PIL import Image
 im=Image.open(sys.argv[3]).convert('RGB');result=metric(*im.size,im.tobytes())
 assert result[0]>=995 and result[1]>=995 and result[2]<900
 print('Native tiled desktop:',*result,'PASS')
