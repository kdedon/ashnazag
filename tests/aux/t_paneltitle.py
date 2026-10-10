#!/usr/bin/env python3
import os, pathlib, subprocess, tempfile
source = pathlib.Path(__file__).resolve().parents[1] / "src/t_mac.c"
s = source.read_text()
start = s.index("static int\nwindowtitle81(")
end = s.index("static void\npanel81(", start)
fn = s[start:end]
harness = r'''#include <stdio.h>
#include <string.h>
#include <assert.h>
static unsigned char mem[16384];
static int reads;
static long g32(unsigned char *p) { return (long)p[0]<<24|(long)p[1]<<16|p[2]<<8|p[3]; }
static int rdmac(int fd,long a,char *b,int n) {
    reads++; if (a<0 || a+n>sizeof(mem)) return 0;
    memcpy(b,mem+a,n); return 1;
}
#define t_info(...) ((void)0)
''' + fn + r'''
static void put(long a,long v) { mem[a]=v>>24;mem[a+1]=v>>16;mem[a+2]=v>>8;mem[a+3]=v; }
static void reset(void) {
    memset(mem,0,sizeof(mem)); reads=0;
    put(0x9d6,0x1000); mem[0x1000+110]=1;
    put(0x1000+134,0x2000); put(0x2000,0x2100);
    mem[0x2100]=10; memcpy(mem+0x2101,"Appearance",10);
}
int main(void) {
    reset(); assert(windowtitle81(0,"Appearance"));
    reset(); assert(!windowtitle81(0,"Monitors"));
    reset(); put(0x9d6,0); assert(!windowtitle81(0,"Appearance"));
    reset(); put(0x9d6,0x1001); assert(!windowtitle81(0,"Appearance"));
    reset(); put(0x1000+134,0); assert(!windowtitle81(0,"Appearance"));
    reset(); put(0x2000,0x5000); assert(!windowtitle81(0,"Appearance"));
    reset(); mem[0x2100]=0; assert(!windowtitle81(0,"Appearance"));
    reset(); mem[0x1000+110]=0; put(0x1000+144,0x1000);
    assert(!windowtitle81(0,"Appearance") && reads<5);
    reset(); mem[0x1000+110]=0; put(0x1000+144,0x1100);
    put(0x1100+144,0x1000); assert(!windowtitle81(0,"Appearance") && reads==65);
    reset(); mem[0x1000+110]=0; put(0x1000+144,0x1100);
    mem[0x1100+110]=1; put(0x1100+134,0x2000);
    assert(windowtitle81(0,"Appearance"));
    reset(); mem[0x2100]=255; memset(mem+0x2101,'x',255);
    assert(!windowtitle81(0,"Appearance"));
    puts("panel window-title parser: 11 PASS");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = pathlib.Path(d)
    (p / "test.c").write_text(harness)
    subprocess.run(["cc", "-fsanitize=address,undefined", "-g", "-o", str(p/"test"), str(p/"test.c")], check=True)
    subprocess.run([str(p/"test")], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
