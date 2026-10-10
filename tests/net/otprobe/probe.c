void *memcpy(to,from,n) void *to; const void *from; unsigned long n;
{
    unsigned char *d=to; const unsigned char *s=from;
    while(n--)*d++=*s++;
    return to;
}
extern long auxsysr(), cfmcall(), vector4(), vector9(), descriptorcall(), descriptorcalln();
extern void exitapp(), disposedescriptor();
extern void *newdescriptor();
extern void *newnativecallback();

static void out(s) char *s;
{
    int n=0; long fd;
    while(s[n]) n++;
    auxsysr(4L,1L,(long)s,(long)n);
    fd=auxsysr(5L,(long)"/tmp/otprobe.out",0x109L,0666L);
    if(fd>=0) {
        auxsysr(4L,fd,(long)s,(long)n);
        auxsysr(6L,fd,0L,0L);
    }
}
static void hex(v) unsigned long v;
{
    char b[9]; int i;
    for(i=7;i>=0;i--) { b[i]="0123456789abcdef"[v&15]; v>>=4; }
    b[8]=0; out(b);
}
static int check(name,ok) char *name; int ok;
{
    out(ok ? "otprobe P " : "otprobe F "); out(name); out("\n");
    return ok;
}
static void pstr(p,s) char *p,*s;
{
    int n=0;
    while(s[n]) { p[n+1]=s[n]; n++; }
    p[0]=n;
}
static void library(libname, names, count) char *libname,**names; int count;
{
    char lib[64],sym[64],err[256],cls=0;
    long a[6],conn=0,addr=0,mainaddr=0,e; int i;
    pstr(lib,libname);
    a[0]=(long)lib; a[1]=0x6d36386bL; a[2]=1;
    a[3]=(long)&conn; a[4]=(long)&mainaddr; a[5]=(long)err;
    out("otprobe I loading "); out(libname); out("\n");
    e=cfmcall(a,6L,1L);
    out("otprobe I library "); out(libname); out(" result "); hex(e); out(" connection "); hex(conn); out("\n");
    if(!check("cfm_load",e==0 && conn)) return;
    for(i=0;i<count;i++) {
        pstr(sym,names[i]); addr=0; cls=0;
        a[0]=conn; a[1]=(long)sym; a[2]=(long)&addr; a[3]=(long)&cls;
        e=cfmcall(a,4L,5L);
        out("otprobe I symbol "); out(names[i]); out(" result "); hex(e);
        out(" class "); hex((unsigned char)cls); out(" address "); hex(addr); out("\n");
        check("cfm_symbol",e==0 && addr && cls==2);
    }
    pstr(sym,"MissingOTProbeSymbol"); addr=0;
    a[0]=conn; a[1]=(long)sym; a[2]=(long)&addr; a[3]=(long)&cls;
    check("cfm_missing_symbol",cfmcall(a,4L,5L)<0 && !addr);
    a[0]=(long)&conn;
    check("cfm_close",cfmcall(a,1L,4L)==0);
}
static long providersymbol(conn,name)
long conn; char *name;
{
    char sym[64],cls=0; long a[4],addr=0,e;
    pstr(sym,name);
    a[0]=conn; a[1]=(long)sym; a[2]=(long)&addr; a[3]=(long)&cls;
    e=cfmcall(a,4L,5L);
    out("otprobe I provider symbol "); out(name); out(" result "); hex(e); out("\n");
    return check("provider_symbol",e==0 && addr && cls==2) ? addr : 0;
}
static void loadlibrary_info(client,util)
long client,util;
{
    char library[64]="AUXLib$hostinet",sym[64],cls=0;
    long a[4],fd,n,addr=0,e,conn; void *desc=0; int i;
    fd=auxsysr(5L,(long)"/mac/lib/OTLoadProbe",0L,0L);
    if(fd<0) return;
    n=auxsysr(3L,fd,(long)library,63L);
    auxsysr(6L,fd,0L,0L);
    if(n>0) {
        library[n]=0;
        for(i=0;i<n;i++) if(library[i]=='\r' || library[i]=='\n') { library[i]=0; break; }
    }
    out("otprobe I explicit ASLM library "); out(library); out("\n");
    for(i=0;i<2 && !addr;i++) {
        conn=i ? client : util; cls=0; addr=0;
        pstr(sym,"OTLoadASLMLibrary");
        a[0]=conn; a[1]=(long)sym; a[2]=(long)&addr; a[3]=(long)&cls;
        e=cfmcall(a,4L,5L);
        out("otprobe I explicit load symbol "); out(i ? "client" : "utilities");
        out(" result "); hex(e); out(" class "); hex((unsigned char)cls);
        out(" address "); hex(addr); out("\n");
        if(e || !addr || cls!=2) addr=0;
    }
    if(!addr) return;
    desc=newdescriptor(addr,0xf1L);
    out("otprobe I explicit load descriptor "); hex((long)desc); out("\n");
    if(!desc) return;
    a[0]=(long)library; e=descriptorcalln(desc,a,1L);
    out("otprobe I explicit load result "); hex(e); out("\n");
    disposedescriptor(desc);
}
extern unsigned long loadword(unsigned long);
static int macrange(unsigned long address,unsigned long size,unsigned long top)
{
    return address>=1024 && !(address&1) && top>=1024 && top<=0x10000000UL &&
        address<top && size<=top-address;
}
static int snapshot(unsigned long address,void *buffer,unsigned long size)
{
    long fd,written,got;
    if(!address || !size || size>256 || address>0xffffffffUL-size)return 0;
    fd=auxsysr(5L,(long)"/tmp/otmeta",0x502L,0600L);
    if(fd<0)return 0;
    if(auxsysr(10L,(long)"/tmp/otmeta",0L,0L)<0) {
        auxsysr(6L,fd,0L,0L);return 0;
    }
    written=auxsysr(4L,fd,address,size);
    got=-1;
    if(written==(long)size && auxsysr(19L,fd,0L,0L)==0)
        got=auxsysr(3L,fd,(long)buffer,size);
    auxsysr(6L,fd,0L,0L);
    return got==(long)size;
}
static unsigned long byteslong(unsigned char *p)
{
    return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|
        ((unsigned long)p[2]<<8)|p[3];
}
static void codeinfo(char *name,unsigned long address)
{
    unsigned char bytes[64];int i;
    out("otprobe I snapshot ");out(name);out(" address ");hex(address);
    if(!snapshot(address,bytes,sizeof bytes)) {out(" inaccessible\n");return;}
    out("\n");
    for(i=0;i<16;i++) {
        if(!(i&7)) {out("otprobe I snapshot ");out(name);out(" words +");hex(4*i);}
        out(" ");hex(byteslong(bytes+4*i));if((i&7)==7)out("\n");
    }
}
static void completioninfo(long endpoint)
{
    unsigned char b[160];unsigned long self,context,cell,queue,sync,qi,put,slot,globals,poll,callback=0,target,binding,descriptor,receiver,table;int i;
    self=loadword((unsigned long)endpoint);
    if(!snapshot(self,b,64))return;
    context=byteslong(b+16);
    out("otprobe I completion self ");hex(self);out(" operation ");hex(byteslong(b+28));out(" busy ");hex(b[53]);out(" context ");hex(context);out("\n");
    if(!snapshot(context,b,26))return;
    cell=byteslong(b);receiver=byteslong(b+16);
    out("otprobe I completion context flags ");hex(b[25]);out(" cell ");hex(cell);
    out(" receiver ");hex(receiver);out("\n");
    if(snapshot(receiver,b,12)) {
        table=byteslong(b);
        if(table<=0xffffffffUL-8 && snapshot(table+8,b,4)) {
            target=byteslong(b);
            out("otprobe I completion receiver method ");hex(target);out("\n");
            if(snapshot(target,b,32)) {
                out("otprobe I completion receiver code");
                for(i=0;i<8;i++){out(" ");hex(byteslong(b+4*i));}out("\n");
                if(byteslong(b)==0x4e560000UL && byteslong(b+4)==0x48e71e38UL &&
                    byteslong(b+8)==0x266e0008UL && byteslong(b+12)==0x2c2e000cUL &&
                    byteslong(b+16)==0x2a2e0010UL && byteslong(b+20)==0x286e0014UL &&
                    table<=0xffffffffUL-108 && snapshot(table+108,b,4)) {
                    target=byteslong(b);
                    out("otprobe I completion control method ");hex(target);out("\n");
                    if(snapshot(target,b,32)) {
                        out("otprobe I completion control code");
                        for(i=0;i<8;i++){out(" ");hex(byteslong(b+4*i));}out("\n");
                        if(byteslong(b)==0x4e56ffacUL && byteslong(b+4)==0x48e71838UL &&
                            byteslong(b+8)==0x286e0008UL && byteslong(b+12)==0x422effc0UL &&
                            target<=0xffffffffUL-0x90 && snapshot(target+0x84,b,12) &&
                            b[0]==0x4e && b[1]==0xb9 && byteslong(b+6)==0x201f2d40UL &&
                            b[10]==0xff && b[11]==0xd4) {
                            target=byteslong(b+2);
                            if(snapshot(target,b,124) && byteslong(b)==0x4e56ffe0UL &&
                                byteslong(b+4)==0x48e70038UL && byteslong(b+8)==0x266e0008UL &&
                                byteslong(b+12)==0x286e000cUL && byteslong(b+16)==0x70002440UL &&
                                byteslong(b+20)==0x2d4cfffcUL && b[0x70]==0x20 && b[0x71]==0x79 &&
                                byteslong(b+0x76)==0x20680128UL && b[0x7a]==0x4e && b[0x7b]==0x90) {
                                slot=byteslong(b+0x72);
                                out("otprobe I completion read method ");hex(target);out(" slot ");hex(slot);out("\n");
                                if(snapshot(slot,b,4)) {
                                    globals=byteslong(b);
                                    if(globals<=0xffffffffUL-300 && snapshot(globals+296,b,4)) {
                                        target=byteslong(b);
                                        out("otprobe I completion read globals ");hex(globals);out(" dispatch ");hex(target);out("\n");
                                        if(snapshot(target,b,32)) {
                                            out("otprobe I completion dispatch code");
                                            for(i=0;i<8;i++){out(" ");hex(byteslong(b+4*i));}out("\n");
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if(!snapshot(cell,b,4))return;
    queue=byteslong(b);
    if(!snapshot(queue,b,24))return;
    qi=byteslong(b);sync=byteslong(b+16);
    out("otprobe I completion queue ");hex(queue);out(" first ");hex(byteslong(b+4));out(" last ");hex(byteslong(b+8));out(" sync ");hex(sync);out("\n");
    if(snapshot(sync,b,33)){out("otprobe I completion sync flags ");hex(b[32]);out("\n");}
    if(!snapshot(qi,b,4))return;
    put=byteslong(b);
    if(!snapshot(put,b,32) || byteslong(b)!=0x4e56ffdcUL || byteslong(b+4)!=0x48e71c38UL ||
        byteslong(b+8)!=0x206e0008UL || byteslong(b+12)!=0x20280014UL ||
        byteslong(b+16)!=0x2d40ffdcUL || byteslong(b+20)!=0x246e000cUL ||
        byteslong(b+24)!=0x206a0014UL || byteslong(b+28)!=0x1428000dUL)return;
    if(!snapshot(queue,b,24))return;
    if(snapshot(byteslong(b+20),b,104)) {
        poll=byteslong(b+36);
        out("otprobe I completion head flags ");hex(byteslong(b+12));out(" poll ");hex(poll);out(" async ");hex(byteslong(b+100));out("\n");
        if(snapshot(poll,b,24)){out("otprobe I completion poll mask ");hex(byteslong(b+8));out(" events ");hex(byteslong(b+20));out("\n");}
    }
    if(put>0xffffffffUL-0x81e || !snapshot(put+0x80e,b,18) ||
        byteslong(b)!=0x2f082079UL || byteslong(b+8)!=0x41e801fcUL ||
        byteslong(b+12)!=0x2f084eb9UL)return;
    slot=byteslong(b+4);
    if(!snapshot(slot,b,4))return;
    globals=byteslong(b);
    out("otprobe I completion globals ");hex(globals);out("\n");
    if(globals<=0xffffffffUL-512 && snapshot(globals+308,b,48)) {
        callback=byteslong(b+4);
        out("otprobe I completion scheduler work ");hex(byteslong(b));out(" callback ");hex(byteslong(b+4));
        out(" critical ");hex(byteslong(b+32));out(" scheduled ");hex(byteslong(b+36));
        out(" flags ");hex(byteslong(b+40));out("\n");
    }
    if(globals<=0xffffffffUL-512 && snapshot(globals+504,b,8)) {
        out("otprobe I completion roots ");hex(byteslong(b));out(" ");hex(byteslong(b+4));out("\n");
    }
    if(!snapshot(callback,b,0x88) || byteslong(b)!=0x4e560000UL || byteslong(b+4)!=0x2f0a7000UL ||
        b[8]!=0x20 || b[9]!=0x79 || byteslong(b+14)!=0x41e8015eUL ||
        byteslong(b+0x78)!=0x42a74879UL || b[0x80]!=0x4e || b[0x81]!=0xb9 ||
        b[0x86]!=0x50 || b[0x87]!=0x4f)return;
    target=byteslong(b+0x82);
    out("otprobe I completion RS import ");hex(target);out("\n");
    if(!snapshot(target,b,24) || b[0]!=0x41 || b[1]!=0xf9)return;
    binding=byteslong(b+2);
    if(!snapshot(binding,b,4))return;
    target=byteslong(b);
    if(!snapshot(target,b,18) || b[0]!=0x41 || b[1]!=0xf9 ||
        byteslong(b+6)!=0x4a906602UL || byteslong(b+10)!=0x4e754aa8UL ||
        byteslong(b+14)!=0x00086718UL)return;
    descriptor=byteslong(b+2);
    if(!snapshot(descriptor,b,24))return;
    out("otprobe I completion RS descriptor ");hex(descriptor);out(" base ");hex(byteslong(b));
    out(" size ");hex(byteslong(b+4));out(" inuse ");hex(byteslong(b+8));out("\n");
}
static int relativeaddress(unsigned long base,int delta,unsigned long *result)
{
    if(base>0xffffffffUL || (delta<0 && base<(unsigned long)-delta) ||
        (delta>=0 && base>0xffffffffUL-(unsigned long)delta))return 0;
    *result=delta<0?base-(unsigned long)-delta:base+(unsigned long)delta;
    return !(*result&1) && *result>=1024;
}
static void importinfo(unsigned long target,unsigned long global)
{
    static const unsigned char stub[] = {
        0x41,0xed,0xff,0x36,0x20,0x10,0x67,0x04,
        0x20,0x40,0x4e,0xd0,0x4e,0xfa,0x00,0x66
    };
    unsigned char code[16],binding[14];unsigned long function,record,slot;int i;
    if(!snapshot(target,code,sizeof code))return;
    for(i=0;i<sizeof stub;i++)if(code[i]!=stub[i])return;
    if(global<1226 || global>0xffffffffUL || (global&1))return;
    slot=global-202;
    if(!snapshot(slot,binding,sizeof binding))return;
    function=byteslong(binding);record=byteslong(binding+4);
    out("otprobe I endpoint import slot ");hex(slot);out(" function ");hex(function);
    out(" record ");hex(record);out(" ordinal ");hex(byteslong(binding+10));out("\n");
    codeinfo("endpoint import record",record);
    if(function)codeinfo("endpoint resolved entry",function);
}
static void thunkinfo(unsigned long address)
{
    static const unsigned char thunk[] = {
        0x41,0xfa,0xff,0x94,0x22,0x50,0x50,0x90,0x22,0xdf,0x22,0x8d,
        0x2a,0x60,0x48,0x7a,0xff,0xa0,0x60,0x00,0x01,0x6e
    };
    unsigned char code[22],fields[8];unsigned long slot,target,ret;int i;
    if(address>0xffffffffUL-22 || !snapshot(address,code,sizeof code))return;
    for(i=0;i<sizeof thunk;i++)if(code[i]!=thunk[i])return;
    if(!relativeaddress(address+2,-108,&slot) || slot<1028 ||
        !relativeaddress(address+16,-96,&ret) ||
        !relativeaddress(address+20,366,&target))return;
    out("otprobe I endpoint thunk signature match slot ");hex(slot);
    out(" return ");hex(ret);out(" target ");hex(target);out("\n");
    if(snapshot(slot-4,fields,sizeof fields)) {
        out("otprobe I endpoint thunk A5 ");hex(byteslong(fields));
        out(" saved-frame cursor ");hex(byteslong(fields+4));out("\n");
        codeinfo("endpoint thunk A5",byteslong(fields));
        importinfo(target,byteslong(fields));
    }
    codeinfo("endpoint thunk target",target);
    codeinfo("endpoint thunk return",ret);
}
static void endpointinfo(unsigned long address)
{
    static const unsigned char signature[] = {
        0x2a,0x51,0x4e,0x56,0xff,0xf8,0x2f,0x0d,0x20,0x6d,0xff,0xc0,
        0x48,0x50,0x2f,0x2e,0x00,0x14,0x2f,0x2e,0x00,0x10,0x2f,0x2e,
        0x00,0x0c,0x2f,0x2e,0x00,0x08,0x2f,0x3c,0x00,0x00,0xff,0xf1,
        0x2f,0x2d,0xff,0x90,0x22,0x6d,0xff,0xd8,0x20,0x59,0x4e,0x90,
        0x4f,0xef,0x00,0x1c,0x2a,0x57,0x4e,0x5e,0x4e,0x74,0x00,0x10
    };
    unsigned char transition[8],code[64],field[4],record[32];
    unsigned long program,global,inner;int n;
    if(!snapshot(address,transition,sizeof transition))return;
    program=byteslong(transition);global=byteslong(transition+4);
    out("otprobe I OTOpenEndpoint transition code ");hex(program);out(" A5 ");hex(global);out("\n");
    codeinfo("OTOpenEndpoint code",program);
    if(!snapshot(program,code,sizeof code))return;
    for(n=0;n<sizeof signature;n++)if(code[n]!=signature[n]) {
        out("otprobe I OTOpenEndpoint wrapper signature mismatch\n");return;
    }
    out("otprobe I OTOpenEndpoint wrapper signature match\n");
    if(global<112)return;
    if(snapshot(global-64,field,4)) {
        out("otprobe I OTOpenEndpoint client context ");hex(byteslong(field));out("\n");
        codeinfo("client context",byteslong(field));
    }
    if(!snapshot(global-112,field,4))return;
    inner=byteslong(field);codeinfo("endpoint inner object",inner);
    thunkinfo(inner);
    if(!snapshot(inner,record,sizeof record) || record[0]!=0xaa || record[1]!=0xfe ||
        record[2]!=7 || record[10] || record[11])return;
    program=byteslong(record+20);
    if(record[17]==0 && !record[18] && !record[19])codeinfo("endpoint inner code",program);
    else if(record[17]==0x10 && !record[18] && record[19]==4 && snapshot(program,transition,8)) {
        out("otprobe I endpoint inner transition code ");hex(byteslong(transition));
        out(" A5 ");hex(byteslong(transition+4));out("\n");
        codeinfo("endpoint inner code",byteslong(transition));
    }
}
static void objectinfo(char *name,unsigned long address)
{
    unsigned char bytes[128];unsigned long table,method;int i;
    out("otprobe I ASLM object ");out(name);out(" address ");hex(address);
    if(!snapshot(address,bytes,64)) { out(" inaccessible\n");return; }
    out("\n");
    for(i=0;i<16;i++) {
        if(!(i&7)) { out("otprobe I ASLM ");out(name);out(" words +");hex(4*i); }
        out(" ");hex(byteslong(bytes+4*i));if((i&7)==7)out("\n");
    }
    table=byteslong(bytes);
    out("otprobe I ASLM vtable ");out(name);out(" address ");hex(table);
    if(!snapshot(table,bytes,sizeof(bytes))) { out(" inaccessible\n");return; }
    out("\n");
    for(i=0;i<32;i++) {
        if(!(i&7)) { out("otprobe I ASLM ");out(name);out(" vtable +");hex(4*i); }
        out(" ");hex(byteslong(bytes+4*i));if((i&7)==7)out("\n");
    }
    if(name[0]!='m')return;
    method=byteslong(bytes+76);
    out("otprobe I ASLM lookup method address ");hex(method);
    if(!snapshot(method,bytes,64)) { out(" inaccessible\n");return; }
    out("\n");
    for(i=0;i<16;i++) {
        if(!(i&7)) { out("otprobe I ASLM lookup words +");hex(4*i); }
        out(" ");hex(byteslong(bytes+4*i));if((i&7)==7)out("\n");
    }
}
static void slminfo(void)
{
    unsigned long top=loadword(0x108),expand=loadword(0x2b6),global;
    out("otprobe I ASLM MemTop ");hex(top);out(" ExpandMem ");hex(expand);out("\n");
    if(!macrange(expand,0x110,top))return;
    global=loadword(expand+0x10c);
    out("otprobe I ASLM globals ");hex(global);out("\n");
    if(!macrange(global,140,top))return;
    out("otprobe I ASLM version ");hex(loadword(global));
    out(" systemtype ");hex(loadword(global+108)&0xffff);
    out(" CPUFlag ");hex(loadword(0x12c)&255);
    out(" currentclient ");hex(loadword(global+12));
    out(" catalog ");hex(loadword(global+16));out("\n");
    out("otprobe I ASLM extensions directory ");hex(loadword(global+104));
    out(" volume ");hex((long)(short)(loadword(global+108)>>16));
    out(" manager ");hex(loadword(global+36));out("\n");
    objectinfo("manager",loadword(global+36));
    objectinfo("catalog",loadword(global+16));
}
extern void notify_entry(), notify_cfm_entry(), notify_selfcall();
static void callback_vector(unsigned long *vector,long a5)
{
    vector[0]=(unsigned long)notify_cfm_entry;vector[1]=a5;
}
extern long tickcount(), currenta5(), systask();
struct netbuf { unsigned long max, len; void *buf; };
struct inetaddr { unsigned short type, port; unsigned long host; unsigned char pad[8]; };
struct callinfo { struct netbuf addr, opt, data; long sequence; };
struct unitinfo { struct netbuf addr, opt, data; };
struct notifyctx {
    long a5;
    volatile unsigned long bind, connected, data, listen, accepted, errors, calls;
    volatile long bindresult, connectresult, acceptresult;
    volatile unsigned long callbackstage, lastevent;
    volatile long lastresult;
    volatile void *lastcookie;
};
#ifdef __m68k__
typedef char inet_size[(sizeof(struct inetaddr)==16)?1:-1];
typedef char call_size[(sizeof(struct callinfo)==40)?1:-1];
#endif
void notify_event(ctx,event,result,cookie)
struct notifyctx *ctx; unsigned long event; long result; void *cookie;
{
    ctx->callbackstage=2; ctx->lastevent=event;
    ctx->lastresult=result; ctx->lastcookie=cookie; ctx->calls++;
    if(event==0x20000001UL) { ctx->bindresult=result; ctx->bind++; }
    else if(event==2) { ctx->connectresult=result; ctx->connected++; }
    else if(event==4) ctx->data++;
    else if(event==1) ctx->listen++;
    else if(event==0x20000003UL) { ctx->acceptresult=result;ctx->accepted++; }
    else if(event==0x10||event==0x20||event==0x40||event==0x24000002UL) ctx->errors++;
    ctx->callbackstage=3;
}

enum { PBIND,PCONNECT,PRCONNECT,PSEND,PRECV,PUSEND,PURECV,PNOTIFY,
       PASYNC,PNONBLOCK,PREMOVE,PIDLE,PLISTEN,PACCEPT,PNUM };
static void callback_native_state(long ep,unsigned long *vector)
{
    unsigned char b[32];unsigned long self,table,target,notifier;
    out("otprobe I registered vector entry ");hex(vector[0]);out(" A5 ");hex(vector[1]);out("\n");
    completioninfo(ep);self=loadword((unsigned long)ep);
    if(!snapshot(self,b,4))return;
    table=byteslong(b);
    if(table>0xffffffffUL-104 || !snapshot(table+8,b,4))return;
    target=byteslong(b);
    if(!snapshot(target,b,24) || byteslong(b)!=0x4e560000UL ||
        byteslong(b+4)!=0x48e71e38UL || byteslong(b+8)!=0x266e0008UL ||
        byteslong(b+12)!=0x2c2e000cUL || byteslong(b+16)!=0x2a2e0010UL ||
        byteslong(b+20)!=0x286e0014UL)return;
    if(!snapshot(table+100,b,4))return;
    target=byteslong(b);codeinfo("native notification dispatch",target);
    if(self<=0xffffffffUL-64)codeinfo("native notification state",self+64);
    if(self>0xffffffffUL-24 || !snapshot(self+20,b,4))return;
    notifier=byteslong(b);codeinfo("native bound provider",self);
    codeinfo("installed native notifier",notifier);
    if(!snapshot(notifier,b,28))return;
    target=byteslong(b+12);codeinfo("installed notifier callback",target);
    table=byteslong(b);
    if(table<=0xffffffffUL-12 && snapshot(table+8,b,4))
        codeinfo("installed notifier method",byteslong(b));
}
static void callback_outcome(char *name,struct notifyctx *ctx)
{
    out("otprobe I callback ");out(name);out(" stage ");hex(ctx->callbackstage);
    out(" calls ");hex(ctx->calls);out(" event ");hex(ctx->lastevent);
    out(" result ");hex(ctx->lastresult);out(" cookie ");hex((long)ctx->lastcookie);out("\n");
    out("otprobe I callback counts bind ");hex(ctx->bind);out(" connect ");hex(ctx->connected);
    out(" accept ");hex(ctx->accepted);out(" errors ");hex(ctx->errors);out("\n");
}

static long invoke(desc,count,v0,v1,v2,v3)
void *desc; long count,v0,v1,v2,v3;
{
    long a[4]; a[0]=v0;a[1]=v1;a[2]=v2;a[3]=v3;
    return descriptorcalln(desc,a,count);
}
static void idle(desc) void *desc;
{
    descriptorcall(desc); systask();
}
static long install_notifier(desc,idledesc,ep,notifier,context)
void *desc,*idledesc; long ep,notifier,context;
{
    unsigned long start=tickcount(),spins=0;
    long result;
    result=invoke(desc,3,ep,notifier,context,0);
    while(result==-3168L && (unsigned long)(tickcount()-start)<600 &&
        ++spins<100000UL) {
        idle(idledesc);
        result=invoke(desc,3,ep,notifier,context,0);
    }
    out("otprobe I notifier retries ");hex(spins);out(" result ");hex(result);out("\n");
    return result;
}
static int await(desc,value) void *desc; volatile unsigned long *value;
{
    unsigned long start=tickcount(), spins=0;
    while(!*value && (unsigned long)(tickcount()-start)<600 && ++spins<1000000UL) idle(desc);
    return *value!=0;
}
static int same(a,b,n) char *a,*b; long n;
{
    while(n--) if(*a++!=*b++)return 0;
    return 1;
}
static void traffic(client,ep,protocol)
long client,ep; int protocol;
{
    char *names[]={"OTBind","OTConnect","OTRcvConnect","OTSnd","OTRcv",
        "OTSndUData","OTRcvUData","OTInstallNotifier","OTSetAsynchronous",
        "OTSetNonBlocking","OTRemoveNotifier","OTIdle","OTListen","OTAccept"};
    unsigned long infos[]={0xff1,0xff1,0x3f1,0x3ff1,0x3ff1,0x3f1,0xff1,0xff1,
        0xf1,0xf1,0xc1,1,0x3f1,0xff1};
    void *desc[PNUM],*notifier=0,*cfmnotifier=0;
    unsigned long callbackvector[2];
    struct notifyctx ctx;
    struct inetaddr remote={2,7,0x0a000264UL,{0}},source={0,0,0,{0}};
    struct callinfo connection;
    struct unitinfo unit;
    char payload[]="OT81!\n",buffer[16];
    unsigned long flags=0,start,spins;
    long target,e,n=0,marker,armfd; unsigned long armargs[3]; int i,installed=0;
    marker=auxsysr(5L,(long)"/mac/lib/OTDataProbe",0L,0L);
    if(marker<0)return;
    auxsysr(6L,marker,0L,0L);
    if(protocol) { remote.host=0x7f000001UL;remote.port=7007; }
    for(i=0;i<PNUM;i++)desc[i]=0;
    for(i=0;i<(int)sizeof(ctx);i++)((char*)&ctx)[i]=0;
    ctx.a5=currenta5();
    for(i=0;i<PNUM;i++) {
        target=providersymbol(client,names[i]);
        if(!target)goto done;
        desc[i]=newdescriptor(target,infos[i]);
        if(!check("traffic_descriptor",desc[i]!=0))goto done;
    }
    callback_vector(callbackvector,ctx.a5);
    notifier=newnativecallback((long)notify_entry,0x3fc0L);
    if(!check("notifier_descriptor",notifier!=0))goto done;
    e=install_notifier(desc[PNOTIFY],desc[PIDLE],ep,(long)callbackvector,(long)&ctx);
    completioninfo(ep);
    out("otprobe I notifier install return ");hex(e);out(" endpoint ");hex(ep);
    out(" notifier ");hex((long)callbackvector);out(" self descriptor ");hex((long)notifier);out(" context ");hex((long)&ctx);out("\n");
    if(!check("notifier_install",e==0))goto done;
    installed=1;
    if(!check("endpoint_async",invoke(desc[PASYNC],1,ep,0,0,0)==0))goto done;
    if(!check("endpoint_nonblocking",invoke(desc[PNONBLOCK],1,ep,0,0,0)==0))goto done;
    out("otprobe I bind callback context ");hex((long)&ctx);
    out(" bytes ");hex(sizeof(ctx));out(" stage-offset ");hex(44);out("\n");
    codeinfo("bind notifier",(long)callbackvector);
    codeinfo("bind self descriptor",(long)notifier);
    codeinfo("bind callback",(long)notify_entry);
    completioninfo(ep);
    notify_selfcall(notifier,&ctx);
    out("otprobe I notifier selfcall stage ");hex(ctx.callbackstage);
    out(" calls ");hex(ctx.calls);out(" event ");hex(ctx.lastevent);out("\n");
    if(!check("notifier_selfcall",ctx.callbackstage==4 && ctx.calls==1 &&
        ctx.lastevent==0 && ctx.lastresult==0 && ctx.lastcookie==0))goto done;
    ctx.callbackstage=ctx.calls=ctx.lastevent=0;ctx.lastresult=0;ctx.lastcookie=0;
    armargs[0]=(long)notify_entry;armargs[1]=(long)notifier;armargs[2]=(long)&ctx;
    armfd=auxsysr(5L,(long)"/dev/uinter0",2L,0L);
    e=armfd<0 ? armfd : auxsysr(54L,armfd,0x800c5193UL,(long)armargs);
    if(armfd>=0)auxsysr(6L,armfd,0L,0L);
    out("otprobe I fault capture arm ");hex(e);out(" current A5 ");hex(ctx.a5);out("\n");
    cfmnotifier=newdescriptor((long)callbackvector,0x3fc0L);
    if(!check("notifier_cfm_descriptor",cfmnotifier!=0))goto done;
    codeinfo("bind CFM self descriptor",(long)cfmnotifier);
    notify_selfcall(cfmnotifier,&ctx);
    out("otprobe I notifier CFM selfcall stage ");hex(ctx.callbackstage);
    out(" calls ");hex(ctx.calls);out(" event ");hex(ctx.lastevent);out("\n");
    if(!check("notifier_cfm_selfcall",ctx.callbackstage==4 && ctx.calls==1 &&
        ctx.lastevent==0 && ctx.lastresult==0 && ctx.lastcookie==0))goto done;
    ctx.callbackstage=ctx.calls=ctx.lastevent=0;ctx.lastresult=0;ctx.lastcookie=0;
    e=invoke(desc[PBIND],3,ep,0,0,0);
    out("otprobe I bind submission ");hex(e);out("\n");
    callback_outcome("bind submitted",&ctx);
    e=await(desc[PIDLE],&ctx.bind);
    callback_outcome("bind awaited",&ctx);
    callback_native_state(ep,callbackvector);
    if(!check("bind_notify",e&&ctx.bindresult==0))goto done;
    for(i=0;i<(int)sizeof(connection);i++)((char*)&connection)[i]=0;
    connection.addr.max=connection.addr.len=16;connection.addr.buf=&remote;
    if(!protocol) {
        e=invoke(desc[PCONNECT],3,ep,(long)&connection,0,0);
        out("otprobe I connect submission ");hex(e);out("\n");
        if(!check("connect_notify",await(desc[PIDLE],&ctx.connected)&&ctx.connectresult==0))goto done;
        if(!check("connect_receive",invoke(desc[PRCONNECT],2,ep,0,0,0)==0))goto done;
        e=invoke(desc[PSEND],4,ep,(long)payload,6,0);
        if(!check("tcp_send",e==6))goto done;
        start=tickcount();spins=0;
        while(n<6 && (unsigned long)(tickcount()-start)<600 && ++spins<1000000UL) {
            e=invoke(desc[PRECV],4,ep,(long)(buffer+n),6-n,(long)&flags);
            if(e>0&&e<=6-n)n+=e;
            else idle(desc[PIDLE]);
        }
        check("tcp_echo",n==6&&same(buffer,payload,6));
    } else {
        for(i=0;i<(int)sizeof(unit);i++)((char*)&unit)[i]=0;
        unit.addr=connection.addr;unit.data.max=unit.data.len=6;unit.data.buf=payload;
        if(!check("udp_send",invoke(desc[PUSEND],2,ep,(long)&unit,0,0)==0))goto done;
        unit.addr.max=16;unit.addr.len=0;unit.addr.buf=&source;
        unit.data.max=16;unit.data.len=0;unit.data.buf=buffer;
        start=tickcount();spins=0;e=-1;
        while(e!=0 && (unsigned long)(tickcount()-start)<600 && ++spins<1000000UL) {
            e=invoke(desc[PURECV],3,ep,(long)&unit,(long)&flags,0);
            if(e!=0)idle(desc[PIDLE]);
        }
        check("udp_echo",e==0&&unit.data.len==6&&same(buffer,payload,6));
        check("udp_source",e==0&&unit.addr.len==16&&source.type==2&&source.port==remote.port&&source.host==remote.host);
    }
    check("data_notify",ctx.data!=0);
    check("notifier_no_errors",ctx.errors==0);
done:
    if(installed)invoke(desc[PREMOVE],1,ep,0,0,0);
    if(cfmnotifier)disposedescriptor(cfmnotifier);
    if(notifier)disposedescriptor(notifier);
    for(i=0;i<PNUM;i++)if(desc[i])disposedescriptor(desc[i]);
}

static void acceptdiagnostic(void)
{
    unsigned long a[4]={1,0,0,0};unsigned char b[160];long fd,e;int i,j;
    fd=auxsysr(5L,(long)"/dev/uinter0",2L,0L);
    e=fd<0 ? fd : auxsysr(54L,fd,0xc0105194UL,(long)a);
    if(fd>=0)auxsysr(6L,fd,0L,0L);
    out("otprobe I accept diagnostic query ");hex(e);out("\n");
    if(e || a[2]!=160 || a[3]>16 || !snapshot(a[1],b,sizeof b) ||
        byteslong(b)!=0x4f544143UL || byteslong(b+4)!=1)return;
    for(i=0;i<8+(int)a[3]*2;i+=4) {
        out("otprobe I accept identity +");hex(i*4);
        for(j=i;j<i+4 && j<40;j++){out(" ");hex(byteslong(b+j*4));}
        out("\n");
    }
}

static void listenprobe(client,create,open,close)
long client; void *create,*open,*close;
{
    char *names[]={"OTBind","OTConnect","OTRcvConnect","OTSnd","OTRcv",
        "OTInstallNotifier","OTSetAsynchronous","OTSetNonBlocking",
        "OTRemoveNotifier","OTIdle","OTListen","OTAccept"};
    unsigned long infos[]={0xff1,0xff1,0x3f1,0x3ff1,0x3ff1,0xff1,
        0xf1,0xf1,0xc1,1,0x3f1,0xff1};
    void *d[12],*upp=0;
    unsigned long callbackvector[2];
    struct notifyctx ctx[2];
    struct inetaddr address={2,7008,0x7f000001UL,{0}},peer={0,0,0,{0}};
    struct { struct netbuf addr;unsigned long queue; } bind;
    struct callinfo request,indication;
    long ep[2]={0,0},conf,target,err,e;
    unsigned long flags=0,start,spins;
    char payload[]="accept",buffer[8];
    int i,installed[2]={0,0},n=0;
    for(i=0;i<12;i++)d[i]=0;
    for(i=0;i<(int)sizeof(ctx);i++)((char*)ctx)[i]=0;
    for(i=0;i<2;i++)ctx[i].a5=currenta5();
    for(i=0;i<12;i++) {
        target=providersymbol(client,names[i]);if(!target)goto done;
        d[i]=newdescriptor(target,infos[i]);if(!check("listen_descriptor",d[i]!=0))goto done;
    }
    callback_vector(callbackvector,ctx[0].a5);
    upp=newnativecallback((long)notify_entry,0x3fc0L);if(!check("listen_notifier_descriptor",upp!=0))goto done;
    for(i=0;i<2;i++) {
        conf=invoke(create,1,(long)"auxtcp",0,0,0);if(!check("listen_configuration",conf!=0))goto done;
        err=-1;ep[i]=invoke(open,4,conf,0,0,(long)&err);
        out("otprobe I listener open index ");hex(i);out(" endpoint ");hex(ep[i]);out(" error ");hex(err);out("\n");
        if(!check("listen_endpoint",ep[i]!=0&&err==0))goto done;
        codeinfo("owned listener",(unsigned long)ep[i]);
        codeinfo("native provider",loadword((unsigned long)ep[i]));
        e=install_notifier(d[5],d[9],ep[i],(long)callbackvector,(long)&ctx[i]);
        completioninfo(ep[i]);
        out("otprobe I listen notifier return ");hex(e);out(" endpoint ");hex(ep[i]);
        out(" notifier ");hex((long)callbackvector);out(" self descriptor ");hex((long)upp);out(" context ");hex((long)&ctx[i]);out("\n");
        if(!check("listen_notifier",e==0))goto done;
        installed[i]=1;
        if(!check("listen_async",invoke(d[6],1,ep[i],0,0,0)==0))goto done;
        if(!check("listen_nonblocking",invoke(d[7],1,ep[i],0,0,0)==0))goto done;
    }
    bind.addr.max=bind.addr.len=16;bind.addr.buf=&address;bind.queue=1;
    e=invoke(d[0],3,ep[0],(long)&bind,0,0);
    out("otprobe I listener bind ");hex(e);out("\n");
    e=await(d[9],&ctx[0].bind);callback_outcome("listener bind",&ctx[0]);
    if(!check("listen_bind",e&&ctx[0].bindresult==0))goto done;
    invoke(d[0],3,ep[1],0,0,0);
    e=await(d[9],&ctx[1].bind);callback_outcome("connector bind",&ctx[1]);
    if(!check("connector_bind",e&&ctx[1].bindresult==0))goto done;
    for(i=0;i<(int)sizeof(request);i++)((char*)&request)[i]=0;
    request.addr.max=request.addr.len=16;request.addr.buf=&address;
    e=invoke(d[1],3,ep[1],(long)&request,0,0);
    out("otprobe I local connect ");hex(e);out("\n");
    if(!check("listen_notify",await(d[9],&ctx[0].listen)))goto done;
    for(i=0;i<(int)sizeof(indication);i++)((char*)&indication)[i]=0;
    indication.addr.max=16;indication.addr.buf=&peer;
    if(!check("listen_receive",invoke(d[10],2,ep[0],(long)&indication,0,0)==0))goto done;
    e=invoke(d[11],3,ep[0],ep[0],(long)&indication,0);
    out("otprobe I accept submission ");hex(e);out(" sequence ");hex(indication.sequence);out("\n");
    e=await(d[9],&ctx[0].accepted);acceptdiagnostic();
    if(!check("accept_notify",e&&ctx[0].acceptresult==0))goto done;
    if(!check("accepted_connect_notify",await(d[9],&ctx[1].connected)&&ctx[1].connectresult==0))goto done;
    if(!check("accepted_connect_receive",invoke(d[2],2,ep[1],0,0,0)==0))goto done;
    if(!check("accepted_send",invoke(d[3],4,ep[1],(long)payload,6,0)==6))goto done;
    start=tickcount();spins=0;
    while(n<6&&(unsigned long)(tickcount()-start)<600&&++spins<1000000UL) {
        e=invoke(d[4],4,ep[0],(long)(buffer+n),6-n,(long)&flags);
        if(e>0&&e<=6-n)n+=e;else idle(d[9]);
    }
    check("accepted_data",n==6&&same(buffer,payload,6));
done:
    for(i=0;i<2;i++)if(ep[i]) {
        if(installed[i])invoke(d[8],1,ep[i],0,0,0);
        check("listen_close",invoke(close,1,ep[i],0,0,0)==0);
    }
    if(upp)disposedescriptor(upp);
    for(i=0;i<12;i++)if(d[i])disposedescriptor(d[i]);
}

extern long hfileinfo(void *);
static void pbput4(unsigned char *p,unsigned long v)
{ p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v; }
static unsigned long pbget4(unsigned char *p)
{ return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3]; }
static void fileinfo(long volume,long directory,char *filename)
{
    unsigned char pb[80],name[256];long err,start=tickcount();int i,index,hit=0;
    for(index=0;index<=512;index++) {
        if(index && tickcount()-start>=120)break;
        for(i=0;i<80;i++)pb[i]=0;
        for(i=0;i<256;i++)name[i]=0;
        if(!index)for(i=0;i<=filename[0];i++)name[i]=filename[i];
        pbput4(pb+18,(unsigned long)name);pb[22]=volume>>8;pb[23]=volume;
        pb[28]=index>>8;pb[29]=index;pbput4(pb+48,directory);
        err=hfileinfo(pb);
        if(err) {
            out("otprobe I fileinfo index ");hex(index);out(" error ");hex(err);out("\n");
            if(index)break;
            continue;
        }
        hit=name[0]==(unsigned char)filename[0];
        for(i=1;hit && i<=name[0];i++)if(name[i]!=(unsigned char)filename[i])hit=0;
        if(!index || hit) {
            out("otprobe I fileinfo index ");hex(index);out(" type ");hex(pbget4(pb+32));
            out(" creator ");hex(pbget4(pb+36));out(" flags ");hex(((long)pb[40]<<8)|pb[41]);
            out(" attribute ");hex(pb[30]);out("\n");
        }
        if(index && hit)break;
    }
    out("otprobe I fileinfo enumeration stop ");hex(index);out(" match ");hex(index && hit);out("\n");
}
extern long findextensions(short *,long *), hopenresource(long,long,char *);
extern long resourcecount(long), resourceref(), resourceerror(), namedresource(long,char *);
extern void useresource(long);
extern long indexedresource(long,long), resourcesize(long);
static void resourceinfo(void)
{
    short volume=0; long directory=0,err,ref,saved,count,handle;
    char filename[64],name[64];
    unsigned char bytes[256];unsigned long pointer,size;int i;
    saved=resourceref();
    err=findextensions(&volume,&directory);
    out("otprobe I resources folder error ");hex(err);
    out(" volume ");hex((long)volume);out(" directory ");hex(directory);out("\n");
    if(err)return;
    pstr(filename,"AUX Host Internet Library");
    fileinfo((long)volume,directory,filename);
    ref=hopenresource((long)volume,directory,filename);err=resourceerror();
    out("otprobe I resources file reference ");hex(ref);out(" error ");hex(err);out("\n");
    if(ref<0)return;
    count=resourcecount(0x6c696272L);err=resourceerror();
    out("otprobe I resources libr count ");hex(count);out(" error ");hex(err);out("\n");
    pstr(name,"OTLib$hostinet");
    handle=namedresource(0x6c696272L,name);err=resourceerror();
    out("otprobe I resources named libr handle ");hex(handle);
    out(" error ");hex(err);out("\n");
    handle=indexedresource(0x6c696272L,1L);err=resourceerror();
    out("otprobe I resources indexed libr handle ");hex(handle);out(" error ");hex(err);out("\n");
    if(handle && !err) {
        size=resourcesize(handle);err=resourceerror();
        out("otprobe I resources indexed libr size ");hex(size);out(" error ");hex(err);out("\n");
        if(!err && size && size<=sizeof(bytes) && snapshot(handle,bytes,4)) {
            pointer=byteslong(bytes);
            out("otprobe I resources libr data ");hex(pointer);
            if(snapshot(pointer,bytes,size)) {
                out("\n");
                for(i=0;i<(int)size;i++) {
                    if(!(i&15)) { out("otprobe I resources libr bytes ");hex(i); }
                    out(" ");hex(bytes[i]);
                    if((i&15)==15 || i+1==(int)size)out("\n");
                }
            } else out(" inaccessible\n");
        }
    }
    useresource(saved);
}
static void stockcycle(desc,repeat)
void **desc;
int repeat;
{
    char *stock[]={"tcp","udp"};
    long a[4],conf,ep,err,e;int i;
    for(i=0;i<2;i++) {
        a[0]=(long)stock[i];conf=descriptorcalln(desc[0],a,1L);
        out("otprobe I stock configuration ");out(stock[i]);
        out(" repeat ");hex(repeat);out(" address ");hex(conf);out("\n");
        if(!check(repeat ? "stock_reopen_configuration" : "stock_configuration",conf!=0))continue;
        codeinfo(stock[i],(unsigned long)conf);
        err=-1;a[0]=conf;a[1]=0;a[2]=0;a[3]=(long)&err;
        ep=descriptorcalln(desc[1],a,4L);
        out("otprobe I stock endpoint ");out(stock[i]);
        out(" repeat ");hex(repeat);out(" reference ");hex(ep);out(" error ");hex(err);out("\n");
        check(repeat ? "stock_reopen_open" : "stock_open",ep!=0 && err==0);
        if(ep) {
            a[0]=ep;e=descriptorcalln(desc[2],a,1L);
            out("otprobe I stock close ");hex(e);out("\n");
            check(repeat ? "stock_reopen_close" : "stock_close",e==0);
        }
    }
}
static void stockreopen(client,util,initdesc,closedesc,enabled)
long client,util;
void *initdesc,*closedesc;
int enabled;
{
    char *names[]={"OTCreateConfiguration","OTOpenEndpoint","OTCloseProvider"};
    char *checks[]={"stock_reopen_create_descriptor","stock_reopen_open_descriptor","stock_reopen_close_descriptor"};
    long info[]={0xf1L,0x3ff1L,0xf1L};
    void *desc[3]={0,0,0};long target,e;int i;
    if(!enabled)return;
    out("otprobe I phase stock CloseOT InitOT repeat\n");
    e=descriptorcall(initdesc);out("otprobe I stock reinitialize result ");hex(e);out("\n");
    if(check("stock_reinitialize",e==0)) {
        for(i=0;i<3;i++) {
            target=providersymbol(i ? client : util,names[i]);
            if(!target)break;
            desc[i]=newdescriptor(target,info[i]);
            if(!check(checks[i],desc[i]!=0))break;
        }
        if(i==3)stockcycle(desc,1);
        descriptorcall(closedesc);out("otprobe P stock_reclose\n");
    }
    for(i=0;i<3;i++)if(desc[i])disposedescriptor(desc[i]);
}
static int providers(client,util)
long client,util;
{
    char *names[]={"auxtcp","auxudp"};
    void *desc[3]={0,0,0};
    long target,open_target,a[4],conf,ep,err,e,fd; int i,stockenabled=0;
    fd=auxsysr(5L,(long)"/mac/lib/OTProvProbe",0L,0L);
    if(fd<0) return 0;
    auxsysr(6L,fd,0L,0L);
    out("otprobe I phase providers\n");
    target=providersymbol(util,"OTCreateConfiguration");
    if(!target) goto done;
    desc[0]=newdescriptor(target,0xf1L);
    if(!check("provider_create_descriptor",desc[0]!=0)) goto done;
    target=providersymbol(client,"OTOpenEndpoint");open_target=target;
    if(!target) goto done;
    codeinfo("OTOpenEndpoint",target);
    endpointinfo((unsigned long)target);
    desc[1]=newdescriptor(target,0x3ff1L);
    if(!check("provider_open_descriptor",desc[1]!=0)) goto done;
    codeinfo("open descriptor",(unsigned long)desc[1]);
    target=providersymbol(client,"OTCloseProvider");
    if(!target) goto done;
    desc[2]=newdescriptor(target,0xf1L);
    if(!check("provider_close_descriptor",desc[2]!=0)) goto done;
    fd=auxsysr(5L,(long)"/mac/lib/OTStockProbe",0L,0L);
    out("otprobe I stock marker open result ");hex(fd);out("\n");
    if(fd>=0) {
        stockenabled=1;
        slminfo();resourceinfo();auxsysr(6L,fd,0L,0L);
        stockcycle(desc,0);
    }
    for(i=0;i<2;i++) {
        a[0]=(long)names[i]; conf=descriptorcalln(desc[0],a,1L);
        out("otprobe I configuration "); out(names[i]); out(" address "); hex(conf); out("\n");
        if(!check("provider_configuration",conf!=0)) continue;
        codeinfo(names[i],(unsigned long)conf);
        err=-1; a[0]=conf; a[1]=0; a[2]=0; a[3]=(long)&err;
        ep=descriptorcalln(desc[1],a,4L);
        out("otprobe I endpoint "); out(names[i]); out(" reference "); hex(ep);
        out(" error "); hex(err); out("\n");
        check("provider_open",ep!=0 && err==0);
        if(ep) {
            codeinfo("owned endpoint",(unsigned long)ep);
            codeinfo("native provider",loadword((unsigned long)ep));
            if(err==0) traffic(client,ep,i);
            a[0]=ep; e=descriptorcalln(desc[2],a,1L);
            out("otprobe I provider close "); hex(e); out("\n");
            check("provider_close",e==0);
        }
    }
    out("otprobe I endpoint after provider attempts\n");
    endpointinfo((unsigned long)open_target);
    fd=auxsysr(5L,(long)"/mac/lib/OTListenProbe",0L,0L);
    if(fd>=0) { auxsysr(6L,fd,0L,0L);listenprobe(client,desc[0],desc[1],desc[2]); }
done:
    for(i=0;i<3;i++) if(desc[i]) disposedescriptor(desc[i]);
    return stockenabled;
}
static void initialize(void)
{
    char lib[64],sym[64],err[256],cls=0;
    long a[6],conn=0,util=0,addr=0,mainaddr=0,e;
    void *initdesc=0,*closedesc=0;int stockenabled=0;
    out("otprobe I phase initialize\n");
    pstr(lib,"OpenTpt68KClientLib");
    a[0]=(long)lib; a[1]=0x6d36386bL; a[2]=1;
    a[3]=(long)&conn; a[4]=(long)&mainaddr; a[5]=(long)err;
    if(!check("client_load",cfmcall(a,6L,1L)==0 && conn)) return;
    pstr(sym,"InitOpenTransport");
    a[0]=conn; a[1]=(long)sym; a[2]=(long)&addr; a[3]=(long)&cls;
    if(check("init_symbol",cfmcall(a,4L,5L)==0 && addr && cls==2)) {
        out("otprobe I init target "); hex(addr); out("\n");
        initdesc=newdescriptor(addr,0x31L);
        if(check("init_descriptor",initdesc!=0)) {
            unsigned short *words=(unsigned short *)initdesc; int i;
            out("otprobe I descriptor "); hex((long)initdesc);
            out(" vector4 "); hex(vector4());
            out(" vector9 "); hex(vector9());
            out(" words");
            for(i=0;i<16;i++) { out(" "); hex(words[i]); }
            out("\n");
            e=descriptorcall(initdesc);
            out("otprobe I init result "); hex(e); out("\n");
            if(check("ot_initialize",e==0)) {
                pstr(lib,"OpenTpt68KUtilLib");
                a[0]=(long)lib; a[1]=0x6d36386bL; a[2]=1;
                a[3]=(long)&util; a[4]=(long)&mainaddr; a[5]=(long)err;
                e=cfmcall(a,6L,1L);
                out("otprobe I utilities result "); hex(e);
                out(" connection "); hex(util); out("\n");
                if(check("utilities_load",e==0 && util)) {
                    loadlibrary_info(conn,util);
                    stockenabled=providers(conn,util);
                }
                pstr(sym,"CloseOpenTransport"); addr=0; cls=0;
                a[0]=util; a[1]=(long)sym; a[2]=(long)&addr; a[3]=(long)&cls;
                e=cfmcall(a,4L,5L);
                out("otprobe I close symbol result "); hex(e);
                out(" address "); hex(addr); out("\n");
                if(check("close_symbol",e==0 && addr && cls==2)) {
                    closedesc=newdescriptor(addr,1L);
                    if(check("close_descriptor",closedesc!=0)) {
                        descriptorcall(closedesc);
                        out("otprobe P ot_close\n");
                        stockreopen(conn,util,initdesc,closedesc,stockenabled);
                    }
                }
            }
        }
    }
    if(closedesc) disposedescriptor(closedesc);
    if(initdesc) disposedescriptor(initdesc);
    if(util) { a[0]=(long)&util; check("utilities_close",cfmcall(a,1L,4L)==0); }
    a[0]=(long)&conn; check("client_close",cfmcall(a,1L,4L)==0);
}
void start(void)
{
    char *client[]={"InitOpenTransport","OTOpenEndpoint","OTBind","OTConnect",
        "OTSnd","OTRcv","OTSndUData","OTRcvUData","OTInstallNotifier","OTCloseProvider"};
    char *utils[]={"CloseOpenTransport","OTCreateConfiguration"};
    char *inet[]={"OTOpenInternetServices","OTInetStringToAddress",
        "OTInetGetInterfaceInfo"};
    out("otprobe I phase libraries\n");
    library("OpenTpt68KClientLib",client,10);
    library("OpenTpt68KUtilLib",utils,2);
    library("OpenTpt68KInetLib",inet,3);
    initialize();
    out("otprobe done\n"); exitapp();
}
