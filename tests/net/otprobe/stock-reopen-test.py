#!/usr/bin/env python3
"""Exercise production stock open and CloseOT/InitOT repeat routines."""
from pathlib import Path
import subprocess,sys
source,out=map(Path,sys.argv[1:]);out.mkdir(parents=True,exist_ok=True)
text=source.read_text()
def function(signature):
 start=text.index(signature);brace=text.index('{',start);depth=1;end=brace+1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
assert text.index('stockcycle(desc,0);') < text.index('a[0]=(long)names[i]; conf=descriptorcalln',text.index('static int providers'))
assert 'stock marker open result' in text
assert 'stockreopen(conn,util,initdesc,closedesc,stockenabled);' in text
c=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
static int stockenabled,checks,failures,initializes,terminates,configs,opens,closes,disposed,symbols,allocated;
static int initerr,configerr,openerr,closeerr,symbolerr,allocerr;
static void out(const char *s){if(!strcmp(s,"otprobe P stock_reclose\n"))++checks;}
static void hex(long n){(void)n;}
static int check(const char *name,int valid){assert(name);++checks;if(!valid)++failures;return valid;}
static void codeinfo(const char *name,unsigned long pointer){assert(name && pointer);}
static long descriptorcall(void *d){if(d==(void*)1){++initializes;return initerr;}assert(d==(void*)2);++terminates;return 0;}
static long providersymbol(long conn,const char *name){assert(conn==(strstr(name,"Create")?20:10));++symbols;check("provider_symbol",symbols!=symbolerr);return symbols==symbolerr?0:2+symbols;}
static void *newdescriptor(long target,long info){++allocated;assert(info==(target==4?0x3ff1:0xf1));return allocated==allocerr?0:(void*)target;}
static void disposedescriptor(void *d){assert(d);++disposed;}
static long descriptorcalln(void *d,long *args,long n){
 if(d==(void*)3){assert(n==1 && (!strcmp((char*)args[0],"tcp")||!strcmp((char*)args[0],"udp")));++configs;return configs==configerr?0:0x100+configs;}
 if(d==(void*)4){assert(n==4 && args[0] && !args[1] && !args[2]);++opens;*(long*)args[3]=opens==openerr?-3221:0;return opens==openerr?0:0x200+opens;}
 assert(d==(void*)5 && n==1 && args[0]>=0x200);++closes;return closes==closeerr?-1:0;
}
'''+function('static void stockcycle(desc,repeat)')+'\n'+function('static void stockreopen(client,util,initdesc,closedesc,enabled)')+r'''
static void reset(void){stockenabled=1;checks=failures=initializes=terminates=configs=opens=closes=disposed=symbols=allocated=0;initerr=configerr=openerr=closeerr=symbolerr=allocerr=0;}
int main(void){void *d[3]={(void*)3,(void*)4,(void*)5};int cases=0;
reset();stockcycle(d,0);assert(checks==6 && !failures && configs==2 && opens==2 && closes==2);++cases;
reset();stockreopen(10,20,(void*)1,(void*)2,stockenabled);assert(checks==14 && !failures && initializes==1 && terminates==1 && disposed==3 && configs==2 && opens==2 && closes==2);++cases;
reset();stockenabled=0;stockreopen(10,20,(void*)1,(void*)2,stockenabled);assert(!checks && !initializes && !terminates);++cases;
reset();initerr=-1;stockreopen(10,20,(void*)1,(void*)2,stockenabled);assert(checks==1 && failures==1 && !terminates && !symbols);++cases;
reset();symbolerr=2;stockreopen(10,20,(void*)1,(void*)2,stockenabled);assert(failures==1 && disposed==1 && terminates==1 && !configs);++cases;
reset();allocerr=2;stockreopen(10,20,(void*)1,(void*)2,stockenabled);assert(failures==1 && disposed==1 && terminates==1 && !configs);++cases;
reset();configerr=1;stockreopen(10,20,(void*)1,(void*)2,stockenabled);assert(failures==1 && configs==2 && opens==1 && closes==1 && disposed==3 && terminates==1);++cases;
reset();openerr=1;stockreopen(10,20,(void*)1,(void*)2,stockenabled);assert(failures==1 && configs==2 && opens==2 && closes==1 && terminates==1);++cases;
reset();closeerr=1;stockreopen(10,20,(void*)1,(void*)2,stockenabled);assert(failures==1 && closes==2 && terminates==1);++cases;
printf("PASS%d production stock/reopen cohorts including failure cleanup\n",cases);return 0;}
'''
p=out/'stock-reopen.c';p.write_text(c)
subprocess.run(['nice','-n19','cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(out/'stock-reopen')],check=True)
subprocess.run([str(out/'stock-reopen')],check=True,env={'ASAN_OPTIONS':'detect_leaks=0'})
