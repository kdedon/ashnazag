#include "uinter.h"
#include "sys/mman.h"
#include "vm/as.h"

extern void dlm_cacheflush();
static struct guest_proc *routeowner;
static long routepc, routetail, routehelper, routelo, routehi;
static unsigned char routenative[28] = {
    0x4e,0x56,0,0,0x48,0xe7,0,0x38,0x20,0x2e,0,0x0c,0x67,0,0,0xaa,
    0x0c,0xae,0xff,0xff,0xff,0xff,0,0x0c,0x67,0,0,0x9e
};
static unsigned char routestub[38] = {
    0x48,0xe7,0xff,0xfe,0x42,0xe7,0x2f,0x2f,0,0x46,0x4e,0xb9,0,0,0,0,
    0x58,0x8f,0x44,0xdf,0x4c,0xdf,0x7f,0xff,0x4e,0x56,0,0,0x48,0xe7,0,0x38,
    0x4e,0xf9,0,0,0,0
};
static int
routerange(a,n,lo,hi)
long a,n,lo,hi;
{
    return a>=lo && !(a&1) && n>0 && a<=hi-n;
}
static int
routecode(pc,tail,helper,lo,hi,installed)
long pc,tail,helper,lo,hi;
int installed;
{
    unsigned char n[28],s[38],h[16];long c;int i;
    if(!routerange(pc,28,2,0x7fffffffL) || !routerange(tail,38,lo,hi) ||
        !routerange(helper,16,lo,hi) ||
        (pc<hi && pc+28>lo) || (tail<helper+16 && helper<tail+38) ||
        copyin((caddr_t)pc,(caddr_t)n,sizeof n) ||
        copyin((caddr_t)tail,(caddr_t)s,sizeof s) ||
        copyin((caddr_t)helper,(caddr_t)h,sizeof h))return 0;
    if(installed) {
        if(G16(n)!=0x4ef9 || G32(n+2)!=tail || G16(n+6)!=0x4e71)return 0;
    } else if(bcmp((char *)n,(char *)routenative,8))return 0;
    if(bcmp((char *)n+8,(char *)routenative+8,20))return 0;
    for(i=0;i<sizeof s;i++)
        if((i<12 || i>=16) && (i<34 || i>=38) && s[i]!=routestub[i])return 0;
    if(G32(s+12)!=helper || (G32(s+34)!=pc+8 && (installed || G32(s+34))) || G16(h)!=0x4ef9 ||
        G32(h+6)!=0x4f544144L || G16(h+10)!=1 || G32(h+12)!=0)return 0;
    c=G32(h+2);
    return routerange(c,2,lo,hi) && !(c>=tail && c<tail+38) &&
        !(c>=helper && c<helper+16) &&
        !as_checkprot(u.u_procp->p_as,(caddr_t)pc,sizeof n,PROT_READ|PROT_WRITE|PROT_EXEC) &&
        !as_checkprot(u.u_procp->p_as,(caddr_t)tail,sizeof s,PROT_READ|PROT_WRITE|PROT_EXEC) &&
        !as_checkprot(u.u_procp->p_as,(caddr_t)helper,sizeof h,PROT_READ|PROT_EXEC) &&
        !as_checkprot(u.u_procp->p_as,(caddr_t)c,2,PROT_READ|PROT_EXEC);
}

int
ui_otroute(gp,b)
struct guest_proc *gp;
char *b;
{
    unsigned char version[2],jump[8],resume[4];long pc=G32(b),tail=G32(b+4),helper=G32(b+8);
    long lo=G32(b+12),size=G32(b+16),hi;int e;
    if(gp!=ui.l_gp || gp!=ui.l_active || gp->gp_proc!=u.u_procp || ui.l_state!=LS_INUSE)
        return EPERM;
    if(copyin((caddr_t)0x15a,(caddr_t)version,2))return EFAULT;
    if(G16(version)!=0x0810 || lo<2 || size<54 || size>0x40000L || lo>0x7fffffffL-size)
        return EINVAL;
    hi=lo+size;
    if(routeowner) {
        if(routeowner!=gp || routepc!=pc || routetail!=tail || routehelper!=helper ||
            routelo!=lo || routehi!=hi)return EBUSY;
        return routecode(pc,tail,helper,lo,hi,1) ? 0 : EINVAL;
    }
    if(!routecode(pc,tail,helper,lo,hi,0))return EINVAL;
    P32(resume,pc+8);
    if(copyout((caddr_t)resume,(caddr_t)(tail+34),4))return EFAULT;
    dlm_cacheflush();
    if(!routecode(pc,tail,helper,lo,hi,0))return EINVAL;
    P16(jump,0x4ef9);P32(jump+2,tail);P16(jump+6,0x4e71);
    routeowner=gp;routepc=pc;routetail=tail;routehelper=helper;routelo=lo;routehi=hi;
    e=copyout((caddr_t)jump,(caddr_t)pc,8);
    dlm_cacheflush();
    if(e) {
        /* The preflight owns these bytes, including a partial publication. */
        if(copyout((caddr_t)routenative,(caddr_t)pc,8)) {
            psignal(gp->gp_proc,SIGSEGV);return EFAULT;
        }
        routeowner=0;routepc=routetail=routehelper=routelo=routehi=0;
        dlm_cacheflush();return EFAULT;
    }
    return 0;
}

void
ui_otrouteforget(gp)
struct guest_proc *gp;
{
    if(gp!=routeowner)return;
    routeowner=0;routepc=routetail=routehelper=routelo=routehi=0;
}

int
ui_otrouteclear(gp)
struct guest_proc *gp;
{
    if(gp!=routeowner)return 0;
    if(gp->gp_proc!=u.u_procp || !routecode(routepc,routetail,routehelper,routelo,routehi,1) ||
        copyout((caddr_t)routenative,(caddr_t)routepc,8)) {
        psignal(gp->gp_proc,SIGSEGV);return EFAULT;
    }
    dlm_cacheflush();routeowner=0;routepc=routetail=routehelper=routelo=routehi=0;
    return 0;
}

int
ui_otroutecontext(gp,b)
struct guest_proc *gp;
char *b;
{
    unsigned char version[2],header[4],table[72],word[2];
    long context=G32(b),v,load,unload;
    if(gp!=ui.l_gp || gp!=ui.l_active || gp->gp_proc!=u.u_procp || ui.l_state!=LS_INUSE)
        return EPERM;
    if(copyin((caddr_t)0x15a,(caddr_t)version,2))return EFAULT;
    if(G16(version)!=0x0810 || !routerange(context,4,2,0x7fffffffL))return EINVAL;
    if(copyin((caddr_t)context,(caddr_t)header,4))return EFAULT;
    v=G32(header);
    if(!routerange(v,sizeof table,2,0x7fffffffL))return EINVAL;
    if(copyin((caddr_t)v,(caddr_t)table,sizeof table))return EFAULT;
    load=G32(table+64);unload=G32(table+68);
    if(!routerange(load,2,2,0x7fffffffL) || !routerange(unload,2,2,0x7fffffffL) ||
        as_checkprot(u.u_procp->p_as,(caddr_t)context,4,PROT_READ) ||
        as_checkprot(u.u_procp->p_as,(caddr_t)v,sizeof table,PROT_READ) ||
        as_checkprot(u.u_procp->p_as,(caddr_t)load,2,PROT_READ|PROT_EXEC) ||
        as_checkprot(u.u_procp->p_as,(caddr_t)unload,2,PROT_READ|PROT_EXEC))return EINVAL;
    if(copyin((caddr_t)load,(caddr_t)word,2) || copyin((caddr_t)unload,(caddr_t)word,2))return EFAULT;
    P32(b+4,routeowner!=0);
    return 0;
}
