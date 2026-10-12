#include "sys/types.h"
#include "sys/conf.h"
#include "kinc.h"
#include "sys/mman.h"
#include "vm/as.h"
#include "vm/seg.h"
#include "guest_native.h"
#include "lisa_runtime.h"
#include "contract.h"
extern int nodev();
extern void dlm_cacheflush();
extern struct modwrapper lplatform_wrapper;
static struct guest_profile profile;
static int opened, device_flag;
__asm__(".weak hat_cm_ram");
extern long hat_cm_ram;
static long *volatile lp_hat4k=&hat_cm_ram;
struct lisa_state {
	struct guest_native native;
	struct lisa_config config;
	char frame[72];
	int active, vector, pop;
	unsigned long locks[128][2],lockdepth[128]; int nlocks;
	int slotprot[16];
	unsigned long services, extensions, lastpc, selector, count;
};
#define STATE(gp) ((struct lisa_state *)GUEST_PRIV(gp))
static int
range(s,a,n,flags)
struct lisa_state *s;
unsigned long a,n;
int flags;
{
	unsigned long i,low=a;
	struct lisa_region *p;
	if(n>~0UL-a) return 0;
	if(a>=0xffd60000UL && a<0xfffe0000UL) low=a&0xffffffUL;
	for(i=0;i<s->config.count;i++) {
		p=&s->config.region[i];
		if((p->flags&flags)==flags && low>=p->base && low-p->base<=p->size && n<=p->size-(low-p->base)) {
			if(a!=low && (p->flags&LP_CODE)) return 0;
			return 1;
		}
	}
	return 0;
}
static int
vur(gp,a,n)
struct guest_proc *gp;
caddr_t a;
unsigned int n;
{
	if((unsigned long)a>=0xff000000UL) return range(STATE(gp),(unsigned long)a,n,0);
	return __amix_valid_usr_range(a,n);
}
static int
reject(gp,r,v)
struct guest_proc *gp;
char *r;
int v;
{
	printf("NOTICE: Lisa pid %d rejected vector %d pc %x sp %x d0 %x d1 %x d7 %x\n",(int)curproc->p_pid,v,(int)GR_PC(r),(int)GR_USP(r),(int)GR_D(r,0),(int)GR_D(r,1),(int)GR_D(r,7));
	psignal(curproc,SIGILL);
	guest_trapret();
	return 0;
}
static int
trap(gp,r,v)
struct guest_proc *gp;
char *r;
int v;
{
	struct lisa_state *s=STATE(gp);
	unsigned long pc=GR_PC(r),sp=GR_USP(r),op,target,ret;
	unsigned short instruction;
	int i,ok=0,stackbytes;
	if(v==45) {
		if((unsigned long)GR_D(r,0)!=LP_RETURN) return guest_native_control(gp,&s->native,r);
		if(!s->active || !guest_native_active(gp,&s->native)) return reject(gp,r,v);
		if(copyin((caddr_t)s->config.frame,(caddr_t)s->frame,72)) return reject(gp,r,v);
		if((GR_SR(s->frame)&0xff00) || !range(s,GR_PC(s->frame),2,LP_EXEC) || !range(s,GR_USP(s->frame),4,LP_WRITE)) return reject(gp,r,v);
		GR_D(r,0)=GNC_LEAVE;
		if(guest_native_control(gp,&s->native,r)) return reject(gp,r,v);
		bcopy((caddr_t)s->frame,r,72);
		s->active=0;
		guest_trapret();return 0;
	}
	if(v==10) {
		if((GR_SR(r)&0x2000) || (GR_FV(r)&0xffff)!=40 || (pc&1) || !range(s,pc,4,LP_CODE) || copyin((caddr_t)pc,(caddr_t)&op,4)) return reject(gp,r,v);
		target=op&0xffffffUL;
		if((op>>24)==0xa0 || (op>>24)==0xa4) {
			if((target&1) || !range(s,target,2,LP_CODE)) return reject(gp,r,v);
			if((op>>24)==0xa0) {
				ret=pc+4;
				if((sp&1) || !range(s,sp-4,4,LP_WRITE) || copyout((caddr_t)&ret,(caddr_t)(sp-4),4)) return reject(gp,r,v);
				GR_USP(r)=sp-4;
			}
			GR_PC(r)=target;
		} else if((op>>24)>=0xa8 && (op>>24)<=0xab) {
			i=(op>>23)&7;target=(op&0x7fffffUL)<<1;
			if(i==7) GR_USP(r)=target;else GR_A(r,i)=target;
			GR_PC(r)=pc+4;
		} else if((op>>24)>=0xac && (op>>24)<=0xaf) {
			if((sp&1) || !range(s,sp-4,4,LP_WRITE) || copyout((caddr_t)&target,(caddr_t)(sp-4),4)) return reject(gp,r,v);
			GR_USP(r)=sp-4;GR_PC(r)=pc+4;
		} else return reject(gp,r,v);
		s->extensions++;guest_trapret();return 0;
	}
	if(v!=33 && v!=37) return 1;
	if(s->active || (pc&1) || !range(s,pc-2,2,LP_CODE) || copyin((caddr_t)(pc-2),(caddr_t)&instruction,2) || instruction!=(v==33?0x4e41:0x4e45)) return reject(gp,r,v);
	s->selector=(unsigned short)GR_D(r,v==33?0:7);s->count=(unsigned short)GR_D(r,1);s->lastpc=pc;s->pop=0;
	if(v==33) {
		for(i=0;i<sizeof lisa_os_rows/sizeof lisa_os_rows[0];i++) if(lisa_os_rows[i].selector==s->selector && lisa_os_rows[i].count==s->count && lisa_os_rows[i].pc==pc) {ok=1;s->pop=lisa_os_rows[i].pop;break;}
		stackbytes=s->selector==59?8:s->selector==144?7:s->pop;
		if(!ok || (sp&1) || !range(s,sp,stackbytes,LP_WRITE) || copyin((caddr_t)sp,(caddr_t)&ret,4) || (ret&1) || !range(s,ret,2,LP_EXEC)) return reject(gp,r,v);
	} else {
		for(i=0;i<sizeof lisa_hw_rows/sizeof lisa_hw_rows[0];i++) if(lisa_hw_rows[i].selector==s->selector && lisa_hw_rows[i].pc==pc) {ok=1;break;}
		if(!ok) return reject(gp,r,v);
	}
	bcopy(r,(caddr_t)s->frame,72);
	if(v==33) {GR_PC(s->frame)=ret;GR_USP(s->frame)=sp+s->pop;}
	if(copyout((caddr_t)s->frame,(caddr_t)s->config.frame,72)) return reject(gp,r,v);
	GR_D(r,0)=GNC_ENTER;
	if(guest_native_control(gp,&s->native,r) || GR_D(r,0)) return reject(gp,r,v);
	s->active=1;s->vector=v;s->services++;
	GR_D(r,0)=v;GR_D(r,1)=s->selector;GR_D(r,2)=sp;
	GR_PC(r)=s->config.handler;GR_USP(r)=s->config.stack;
	guest_trapret();return 0;
}
static void
exited(gp)
struct guest_proc *gp;
{
	struct lisa_state *s=STATE(gp);
	int i;
	for(i=0;i<s->nlocks;i++) as_ctl(curproc->p_as,(caddr_t)s->locks[i][0],s->locks[i][1],MC_UNLOCK,0,0,(unsigned long *)0,0);
	s->nlocks=0;
	guest_native_exit(gp,&s->native);
}
static int
forked(parent,child)
struct guest_proc *parent,*child;
{
	return EBUSY;
}
static int
open_device(dev,flags,kind,credentials)
dev_t *dev;
int flags,kind;
struct cred *credentials;
{
	if(getminor(*dev)) return ENXIO;
	opened++;return 0;
}
static int
close_device(dev,flags,kind,credentials)
dev_t dev;
int flags,kind;
struct cred *credentials;
{
	opened--;return 0;
}
static int
ioctl_device(dev,command,argument,mode,credentials,result)
dev_t dev;
int command,mode;
caddr_t argument;
struct cred *credentials;
int *result;
{
	struct guest_proc *gp=GUESTP(curproc);
	struct lisa_config config;
	struct lisa_state *s;
	struct lisa_region *p,*q;
	unsigned long i,j,end,values[6],actual_page=lp_hat4k?4096UL:2048UL;
	unsigned char byte,highbyte;
	unsigned long aliases[5]={0xd60000UL,0xf60000UL,0xf80000UL,0xfa0000UL,0xfc0000UL};
	unsigned long aliasbytes[5]={0x200000UL,0x20000UL,0x20000UL,0x20000UL,0x20000UL};
	int error;
	if(command==LP_ENTER) {
		if(gp) return EBUSY;
		if(copyin(argument,(caddr_t)&config,sizeof config)) return EFAULT;
		if(config.version!=LP_VERSION || config.count!=60 || (config.handler&1) || config.handler<0x80000000UL || config.handler>=0xc0000000UL || config.stack<0x80000000UL || config.stack>=0xc0000000UL || (config.stack&3) || copyin((caddr_t)config.handler,(caddr_t)&byte,1) || copyout((caddr_t)&byte,(caddr_t)config.frame,1)) return EINVAL;
		for(i=0;i<config.count;i++) {
			p=&config.region[i];end=p->base+p->size;
			if(i<55) {
				if(p->base!=lisa_objects[i].base+8 || p->size!=lisa_objects[i].size-8 || p->flags!=(LP_CODE|LP_EXEC)) return EINVAL;
			} else if(p->base!=aliases[i-55] || p->size!=aliasbytes[i-55] || p->flags!=(LP_WRITE|((i==56 || i==59)?LP_EXEC:0))) return EINVAL;
			if(p->mapped!=(p->base&~(actual_page-1)) || p->mapped_size!=((end+actual_page-1)&~(actual_page-1))-p->mapped || !p->size || end<p->base || p->base<p->mapped || end>p->mapped+p->mapped_size || (p->flags&~7) || ((p->flags&LP_CODE)&&(p->flags&LP_WRITE))) {printf("NOTICE: Lisa region %d page %x base %x size %x mapped %x bytes %x rejected\n",(int)i,(int)actual_page,(int)p->base,(int)p->size,(int)p->mapped,(int)p->mapped_size);return EINVAL;}
			for(j=0;j<i;j++) {q=&config.region[j];if(p->base<q->base+q->size && q->base<end) return EINVAL;}
			if(copyin((caddr_t)p->base,(caddr_t)&byte,1) || copyin((caddr_t)(end-1),(caddr_t)&byte,1)) return EFAULT;
			if((p->flags&LP_WRITE) && copyout((caddr_t)&byte,(caddr_t)(end-1),1)) return EFAULT;
			if((p->flags&LP_CODE) && !copyout((caddr_t)&byte,(caddr_t)(end-1),1)) return EINVAL;
		}
		if((error=guest_attach(&profile))) return error;
		gp=GUESTP(curproc);s=STATE(gp);s->config=config;
		gp->gp_flags=0;
		for(i=0;i<16;i++) s->slotprot[i]=PROT_READ|PROT_WRITE|PROT_USER;
		if((error=guest_native_init(gp,&s->native))) {guest_detach(&profile);return error;}
		for(i=0;i<5;i++) for(j=0;j<aliasbytes[i];j+=2048) {
			if(copyin((caddr_t)(aliases[i]+j),(caddr_t)&byte,1) || copyin((caddr_t)((aliases[i]|0xff000000UL)+j),(caddr_t)&highbyte,1) || byte!=highbyte) {guest_detach(&profile);return EFAULT;}
		}
		dlm_cacheflush();return 0;
	}
	if(command==LP_PUBLISH) {dlm_cacheflush();return 0;}
	if(!gp || gp->gp_prof!=&profile) return ENXIO;
	s=STATE(gp);
	if(command==LP_LEAVE) return guest_detach(&profile);
	if(command==LP_PROTECT) {
		if(copyin(argument,(caddr_t)values,12)) return EFAULT;
		if(values[0]<0xd60000UL || values[0]>=0xf60000UL || ((values[0]-0xd60000UL)%0x20000UL) || values[1]!=0x20000UL || (values[2]!=0 && values[2]!=1 && values[2]!=3)) return EINVAL;
		i=(values[0]-0xd60000UL)/0x20000UL;j=values[2]|PROT_USER;
		if((error=as_setprot(curproc->p_as,(caddr_t)values[0],values[1],j))) return error;
		if((error=as_setprot(curproc->p_as,(caddr_t)(values[0]|0xff000000UL),values[1],j))) {as_setprot(curproc->p_as,(caddr_t)values[0],values[1],s->slotprot[i]);return error;}
		s->slotprot[i]=j;return 0;
	}
	if(command==LP_LOCK) {
		if(copyin(argument,(caddr_t)values,8)) return EFAULT;
		for(i=0;i<s->nlocks;i++) if(s->locks[i][0]==values[0] && s->locks[i][1]==values[1]) {if(s->lockdepth[i]==~0UL)return EINVAL;s->lockdepth[i]++;return 0;}
		if(s->nlocks>=128) return EINVAL;
		for(i=0;i<s->config.count;i++) {p=&s->config.region[i];if(values[0]==p->mapped && values[1]==p->mapped_size) break;}
		if(i==s->config.count) {
			if(values[0]<0xffd60000UL || values[0]>=0xfffe0000UL || !range(s,values[0],values[1],0)) return EINVAL;
		}
		if(as_ctl(curproc->p_as,(caddr_t)values[0],values[1],MC_LOCK,0,0,(unsigned long *)0,0)) return EFAULT;
		s->locks[s->nlocks][0]=values[0];s->locks[s->nlocks][1]=values[1];s->lockdepth[s->nlocks++]=1;return 0;
	}
	if(command==LP_UNLOCK) {exited(gp);return 0;}
	if(command!=LP_INFO) return EINVAL;
	values[0]=LP_VERSION;values[1]=s->extensions;values[2]=s->lastpc;values[3]=s->selector;values[4]=s->count;values[5]=s->services;
	return copyout((caddr_t)values,argument,sizeof values)?EFAULT:0;
}
static int
load()
{
	int v;
	bzero((caddr_t)&profile,sizeof profile);
	profile.gpf_name="lisa";profile.gpf_wrapper=&lplatform_wrapper;profile.gpf_privsz=sizeof(struct lisa_state);profile.gpf_exit=exited;profile.gpf_fork=forked;profile.gpf_vur=vur;
	for(v=0;v<48;v++) if(v==10 || v==33 || v==37 || v==45) {profile.gpf_disp[v].gd_kind=GD_SYSCALL;profile.gpf_disp[v].gd_flags=GDF_USER;profile.gpf_disp[v].gd_fn=trap;}
	return guest_profile_add(&profile);
}
static int
unload()
{
	if(opened) return EBUSY;
	guest_profile_del(&profile);return 0;
}
struct mod_drv_data lplatform_drvdata[]={
{{nodev,nodev,nodev,nodev,nodev,nodev,nodev,0},0,0,
{open_device,close_device,nodev,nodev,ioctl_device,nodev,nodev,nodev,nodev,nodev,0,0,&device_flag},LP_MAJOR,1}};
MOD_DRV_WRAPPER(lplatform,load,unload,0,"Lisa native services");
