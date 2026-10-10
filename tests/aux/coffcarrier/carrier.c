/* A native application owns the world used by an original Unix child. */
extern long gettooltrap(),auxsys4(),stackcontext(),auxsysr(),auxspawn(),auxwait(),auxwrite(),getnext(),getcurrent(),getprocess(),currenta5(),systemversion();
extern void settooltrap(),waitnext(),exitapp(),setstackbase();
#define L(p,n) (*(long *)((p)+(n)))
static char **coffenvironment;
static void zero(char *p,int n) { while(n--) *p++=0; }
static void out(char *s) { long fd; int n=0; while(s[n]) n++;
    fd=auxsysr(5L,(long)"/tmp/coffprobe.out",0x109L,0666L);
    if(fd>=0) { auxsysr(4L,fd,(long)s,(long)n); auxsysr(6L,fd,0L,0L); } }
static void value(char *label,long v) { char b[16],s[16]; int n=0,i=0; unsigned long u=v;
    out(label); if(v<0) {out("-");u=-(unsigned long)v;}
    do {b[n++]='0'+u%10;} while((u/=10)); while(n) s[i++]=b[--n];s[i]=0;out(s);out("\n"); }
long stackalloc(void) {
    long id,a,e;
    id=auxsys4(52L,3L,0L,65536L,0600L);
    if(id<0) return 0;
    a=auxsys4(52L,0L,id,0x70000000L,0L);
    e=auxsys4(52L,1L,id,0L,0L);
    if(a<0) return 0;
    if(e<0 || a!=0x70000000L) {
        auxsys4(52L,2L,a,0L,0L);
        if(e<0) auxsys4(52L,1L,id,0L,0L);
        return 0;
    }
    zero((char *)(a+65472L),64);
    return a;
}
void stackrestore(void) {
    long *c=(long *)stackcontext();char psn[8];
    zero(psn,8);
    if(currenta5()==c[2] && auxsysr(20L,0L,0L,0L)==c[5] &&
        getcurrent(psn)==0 && L(psn,0)==c[6] && L(psn,4)==c[7] &&
        (!c[8] || c[9]==c[10])) setstackbase(c[3]);
}
static const long hooktraps[4]={0xa8fdL,0xa9b4L,0xa9eaL,0xa82eL};
static const long hooktargets[4]={0x136e0L,0x1376aL,0x137b8L,0x137a6L};
static unsigned int rotate(unsigned int v,int n) {return (v>>n)|(v<<(32-n));}
static int hookdigest(void) {
    static const unsigned int k[64]={
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
    static const unsigned int expected[8]={0xe13626c4U,0x8494b1d6U,0x95baf81aU,0xce34fca0U,0xea9f26e9U,0xc156b47eU,0x6907dc97U,0x512e1410U};
    unsigned int h[8]={0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
    unsigned int w[64],a,b,c,d,e,f,g,q,t1,t2,v;int block,i,j,index;
    for(block=0;block<5;block++) {
        for(i=0;i<16;i++) {
            v=0;for(j=0;j<4;j++) {
                index=block*64+4*i+j;
                v=(v<<8)|(index<284 ? ((const unsigned char *)0x136e0L)[index] :
                    index==284 ? 128U : index>=316 ? (2272U>>((319-index)*8))&255U : 0U);
            }
            w[i]=v;
        }
        for(i=16;i<64;i++)w[i]=w[i-16]+(rotate(w[i-15],7)^rotate(w[i-15],18)^(w[i-15]>>3))+
            w[i-7]+(rotate(w[i-2],17)^rotate(w[i-2],19)^(w[i-2]>>10));
        a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];q=h[7];
        for(i=0;i<64;i++) {
            t1=q+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            t2=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
            q=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=q;
    }
    for(i=0;i<8;i++)if(h[i]!=expected[i])return 0;
    return 1;
}
static int hookcapture(long *c) {
    int i;long p;
    for(i=0;i<4;i++) {
        p=gettooltrap(hooktraps[i]);
        if((p&1) || !((p>=0x10000L && p<0x1000000L) ||
            (p>=0x40800000L && p<0x40900000L)) ||
            (p>=0x136e0L && p<0x137fcL)) return 0;
        c[12+i]=p;
    }
    return 1;
}
static int hookrestore(long *c,long version) {
    char psn[8];int i,j,installed=0;long mask,result=0,p;
    zero(psn,8);
    if((version!=0x0761L && version!=0x0810L) || systemversion()!=version ||
        !c[8] || c[9]!=c[10] || currenta5()!=c[2] ||
        auxsysr(20L,0L,0L,0L)!=c[5] || getcurrent(psn)!=0 ||
        L(psn,0)!=c[6] || L(psn,4)!=c[7]) return -1;
    mask=auxsysr(130L,-1L,0L,0L);if(mask<0)return -1;
    for(i=0;i<4;i++) {
        p=gettooltrap(hooktraps[i]);
        if(p==hooktargets[i])installed++;
        else if(p!=c[12+i]) {result=-2;goto done;}
    }
    if(!installed)goto done;
    if(installed!=4) {result=-2;goto done;}
    for(i=0;i<4;i++)if(*(long *)(0x87d30L+4*i)!=c[12+i]) {result=-3;goto done;}
    if(!hookdigest()) {result=-3;goto done;}
    for(i=0;i<4;i++) {
        if(gettooltrap(hooktraps[i])!=hooktargets[i])break;
        settooltrap(hooktraps[i],c[12+i]);
        if(gettooltrap(hooktraps[i])!=c[12+i])break;
    }
    if(i==4)result=1;
    else {
        for(j=0;j<i;j++)if(gettooltrap(hooktraps[j])==c[12+j])
            settooltrap(hooktraps[j],hooktargets[j]);
        result=-4;
    }
done:
    if(auxsysr(131L,mask,0L,0L)<0)return -5;
    return result;
}
void childexec(long mask) {
    long fd,e; char *args[2];
    auxsysr(131L,mask,0L,0L);
    auxsysr(6L,1L,0L,0L); fd=auxsysr(5L,(long)"/tmp/coffchild.out",0x101L,0666L);
    if(fd!=1 && fd>=0) {auxsysr(41L,fd,0L,0L);auxsysr(6L,fd,0L,0L);}
    auxsysr(6L,2L,0L,0L);auxsysr(41L,1L,0L,0L);
    auxsysr(4L,1L,(long)"child_before_exec\n",18L);
    args[0]="/mac/bin/CommandShell";args[1]=0;
    e=auxsysr(59L,(long)args[0],(long)args,(long)coffenvironment);
    value("exec_error ",e); auxsysr(1L,127L,0L,0L);
}
void start(void) {
    char psn[8],info[60],name[64],ev[16]; long pid,fd,e=0,owner,request,status=0,mask,reaped=0; char raw[32],sigcld[12];long world[5];
    int i,found=0;long a5,location,size,version=systemversion();long *context=(long *)stackcontext();
    owner=auxsysr(20L,0L,0L,0L);context[5]=owner;value("private_stack_base ",context[0]);value("private_stack_size ",65536L); value("owner ",owner);
    a5=currenta5();value("CurrentA5 ",a5);zero(psn,8);
    zero(info,60);L(info,0)=60;L(info,4)=(long)name;
    if(getcurrent(psn)==0 && getprocess(psn,info)==0 &&
        L(info,20)==0x434f6666L && L(info,8)==L(psn,0) && L(info,12)==L(psn,4)) {
        value("native_psn_high ",L(psn,0));value("native_psn_low ",L(psn,4));found=1;
        location=L(info,28);size=L(info,32);
        value("native_location ",location);value("native_size ",size);
        value("native_a5_contained ",location>0 && size>0 &&
            (unsigned long)a5>=(unsigned long)location &&
            (unsigned long)a5-(unsigned long)location<(unsigned long)size);
    }
    context[6]=L(psn,0);context[7]=L(psn,4);value("native_current ",found);if(!found) {exitapp();return;}
    fd=auxsysr(5L,(long)"/dev/uinter0",2L,0L);
    value("uinter ",fd); if(fd<0) {exitapp();return;}
    {
        long environment[2];
        e=auxsysr(54L,fd,0x40085195L,(long)environment);
        value("environment_result ",e);
        if(e || !environment[0]) {auxsysr(6L,fd,0L,0L);exitapp();return;}
        coffenvironment=(char **)environment[0];
        value("environment_count ",environment[1]);
    }
    zero(sigcld,sizeof sigcld);
    e=auxsysr(129L,18L,0L,(long)sigcld);
    value("sigcld_query ",e);value("sigcld_handler ",L(sigcld,0));value("sigcld_flags ",L(sigcld,8));
    if(!hookcapture(context)) {value("hooks_capture ",-1L);auxsysr(6L,fd,0L,0L);exitapp();return;}
    zero(raw,sizeof raw);
    mask=auxsysr(130L,-1L,0L,0L);
    if(mask<0) {auxsysr(6L,fd,0L,0L);exitapp();return;}
    pid=auxspawn(raw,mask);
    auxsysr(131L,mask | (1L<<17),0L,0L);
    value("fork_parent_d0 ",L(raw,0)); value("fork_parent_d1 ",L(raw,4));
    value("fork_parent_ccr ",*(unsigned short *)(raw+8));value("fork_parent_sp ",L(raw,12));
    value("child ",pid);
    if(pid>0) {
        request=pid;world[0]=pid;world[1]=L(psn,0);world[2]=L(psn,4);world[3]=a5;world[4]=0;
        for(i=0;i<300;i++) {
            if(!world[4]) {
                e=auxsysr(54L,fd,0xc0145190L,(long)world);
                if(e) {zero(ev,16);waitnext(ev);continue;}
                context[8]=world[4];context[10]=pid;value("world_token ",world[4]);
            }
            e=auxsysr(54L,fd,0x80045126L,(long)&request);
            if(!e) {
                reaped=auxwait(pid,&status,1L);
                if(reaped) break;
                i=-1;continue;
            }
            if(e!=-22 && e!=-4) break;
            zero(ev,16);waitnext(ev);
        }
        value("handover ",e); value("attempts ",i);
        if(!reaped) {
            reaped=auxwait(pid,&status,1L);
            if(!reaped) {
                auxsysr(37L,pid,9L,0L);
                reaped=auxwait(pid,&status,0L);
            }
        }
        context[9]=reaped;value("hooks_cleanup ",hookrestore(context,version));value("reaped ",reaped);value("status ",status);
        value("fork_child_d0 ",L(raw,16));value("fork_child_d1 ",L(raw,20));
        value("fork_child_ccr ",*(unsigned short *)(raw+24));value("fork_child_sp ",L(raw,28));
    }
    auxsysr(131L,mask,0L,0L);
    auxsysr(6L,fd,0L,0L);out("done\n");exitapp();
}
