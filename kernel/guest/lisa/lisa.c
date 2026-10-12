#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/procfs.h>
#include <sys/ioctl.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "lisa_runtime.h"
#include "contract.h"
#include "bootstrap.h"
#include "resources.h"
#include "digest.h"
#include "lisa_host.h"
#include "lisa_hw.h"
#include "lisa_services.h"
extern int mprotect(),munmap(),mincore();
extern void lisa_start(),lisa_service_entry(),lisa_signal_return();
const char *lisa_program="Office System";
static struct lisa_config config;
static struct lisa_frame frame;
static struct lisa_hw hardware;
static struct lisa_host host;
static int bridge=-1,attached,delivered,requested;
static volatile sig_atomic_t pending,frontstop,dispatching;
static unsigned long init_callback,terminate_entry,terminate_a5,page;
static int desktop_seen;
static unsigned char parent_event[48];
static unsigned char native_stack[65536];
static unsigned long alias_base[5]={0xd60000UL,0xf60000UL,0xf80000UL,0xfa0000UL,0xfc0000UL};
static unsigned long alias_size[5]={0x200000UL,0x20000UL,0x20000UL,0x20000UL,0x20000UL};
static int alias_prot[5]={3,7,3,3,7};
static int slot_prot[16];
static void *surface;
static const char *stage="inputs";
static unsigned long mapped_base[70],mapped_size[70];
static int mapped_count;
static void
putword(p,n)
unsigned char *p;
unsigned long n;
{
	p[0]=n>>8;p[1]=n;
}
static void
putlong(p,n)
unsigned char *p;
unsigned long n;
{
	putword(p,n>>16);putword(p+2,n);
}
static unsigned long
getlong(p)
const unsigned char *p;
{
	return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];
}
static int
contained(a,n,b,z)
unsigned long a,n,b,z;
{
	return a>=b && a-b<=z && n<=z-(a-b);
}
int
lisa_executable(a,n)
unsigned long a,n;
{
	unsigned long i;
	if(a&1) return 0;
	for(i=0;i<config.count;i++) if((config.region[i].flags&LP_EXEC) && contained(a,n,config.region[i].base,config.region[i].size)) return 1;
	return 0;
}
void *
lisa_pointer(a,n,write)
unsigned long a,n;
int write;
{
	unsigned long i,low=a,first,last,j;
	if(n>~0UL-a) return 0;
	for(i=0;i<5;i++) if(contained(a,n,alias_base[i]|0xff000000UL,alias_size[i])) {low=a&0xffffffUL;break;}
	if(a>=0xff000000UL && low==a) return 0;
	for(i=0;i<config.count;i++) if(contained(low,n,config.region[i].base,config.region[i].size)) {
		if(write && !(config.region[i].flags&LP_WRITE)) return 0;
		if(contained(low,n,0xd60000UL,0x200000UL)) {
			first=(low-0xd60000UL)/0x20000UL;last=n?(low+n-1-0xd60000UL)/0x20000UL:first;
			for(j=first;j<=last && j<16;j++) if(!(slot_prot[j]&PROT_READ) || (write && !(slot_prot[j]&PROT_WRITE))) return 0;
		}
		return (void *)low;
	}
	return 0;
}
int
lisa_protect(base,size,protection)
unsigned long base,size;
int protection;
{
	unsigned long request[3];int index;
	if(base<0xd60000UL || base>=0xf60000UL || (base-0xd60000UL)%0x20000UL || !size || size>0x20000UL || (protection!=0 && protection!=1 && protection!=3)) {errno=EINVAL;return -1;}
	request[0]=base;request[1]=0x20000UL;request[2]=protection;
	if(ioctl(bridge,LP_PROTECT,request)<0) return -1;
	index=(base-0xd60000UL)/0x20000UL;slot_prot[index]=protection;return 0;
}
static void *
validate(opaque,a,n,write)
void *opaque;
unsigned long a,n;
int write;
{
	return lisa_pointer(a,n,write);
}
static int
interrupted(opaque)
void *opaque;
{
	return pending!=0;
}
int
lisa_wait_tick()
{
	long hz=sysconf(_SC_CLK_TCK);
	if(pending) {errno=EINTR;return -1;}
	if(lisa_hw_poll(&hardware)<0) return -1;
	if(hz<=0) {errno=EINVAL;return -1;}
	if(poll((struct pollfd *)0,0,(1000+hz-1)/hz)<0) return -1;
	if(pending) {errno=EINTR;return -1;}
	return lisa_hw_poll(&hardware);
}
int
lisa_slot_resident(base,size)
unsigned long base,size;
{
	unsigned char result[64];
	unsigned long first,n,i;
	if(!size || size>0x20000UL) {errno=EINVAL;return -1;}
	first=base&~(page-1);n=((base-first)+size+page-1)/page;
	if(n>sizeof result || mincore((void *)first,n*page,(char *)result)<0) return -1;
	for(i=0;i<n;i++) if(!(result[i]&1)) return 0;
	return 1;
}
int
lisa_resident()
{
	int a=lisa_slot_resident(0xf60000UL,0x20000UL),b=lisa_slot_resident(0xf80000UL,0x20000UL);
	return a<0 || b<0?-1:a&&b;
}
int
lisa_lock_code(pc)
unsigned long pc;
{
	unsigned long i,values[2];
	for(i=0;i<config.count;i++) if((config.region[i].flags&LP_CODE) && contained(pc,2,config.region[i].base,config.region[i].size)) {
		values[0]=config.region[i].mapped;values[1]=config.region[i].mapped_size;
		return ioctl(bridge,LP_LOCK,values);
	}
	errno=EINVAL;return -1;
}
void
lisa_set_init(callback)
unsigned long callback;
{
	init_callback=callback;
}
int
lisa_set_terminate(entry,a5)
unsigned long entry,a5;
{
	unsigned char *p;
	if(!entry) {terminate_entry=0;return 0;}
	p=lisa_pointer(entry,6,0);
	if(!p || !lisa_executable(entry,6) || p[0]!=0x4e || p[1]!=0xf9 || !lisa_executable(getlong(p+2),2) || !lisa_pointer(a5,4,0)) {errno=EINVAL;return -1;}
	terminate_entry=entry;terminate_a5=a5;return 0;
}
static void
route(reg,sr,pc,cause)
unsigned long *reg;
unsigned short sr;
unsigned long pc,cause;
{
	unsigned char *env=(unsigned char *)0xfc0000UL,*data=(unsigned char *)0xfc0048UL,*sp=(unsigned char *)(0xfe0000UL-512-12);
	int i;
	memset(env,0,70);putlong(env,pc);putword(env+4,sr);
	for(i=0;i<8;i++) putlong(env+6+i*4,reg[1+i]);
	for(i=0;i<7;i++) putlong(env+38+i*4,reg[9+i]);
	putlong(env+66,reg[0]);memset(data,0,48);putlong(data,cause);
	putlong(sp,(unsigned long)lisa_signal_return);putlong(sp+4,(unsigned long)data);putlong(sp+8,(unsigned long)env);
	reg[0]=(unsigned long)sp;reg[14]=terminate_a5;delivered=1;
}
void
lisa_default_terminate()
{
	struct sigaction action;sigset_t mask;
	memset(&action,0,sizeof action);action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);
	sigaction(SIGTERM,&action,0);sigemptyset(&mask);sigaddset(&mask,SIGTERM);sigprocmask(SIG_UNBLOCK,&mask,0);kill(getpid(),SIGTERM);_exit(143);
}
static void
signal_handler(signal,info,context)
int signal;
siginfo_t *info;
void *context;
{
	ucontext_t *u=(ucontext_t *)context;
	unsigned long regs[16];
	int i;
	if(dispatching) {pending=1;return;}
	if(delivered || !terminate_entry) lisa_default_terminate();
	if(nh_native_begin()) _exit(143);
	regs[0]=u->uc_mcontext.gregs[R_SP];
	for(i=0;i<15;i++) regs[i+1]=u->uc_mcontext.gregs[i];
	route(regs,u->uc_mcontext.gregs[R_PS],u->uc_mcontext.gregs[R_PC],requested?0:3);
	u->uc_mcontext.gregs[R_SP]=regs[0];u->uc_mcontext.gregs[R_A5]=regs[14];u->uc_mcontext.gregs[R_PC]=terminate_entry;
	u->uc_mcontext.gregs[R_PS]&=0xff;sigaddset(&u->uc_sigmask,SIGTERM);
	if(nh_native_end()) _exit(143);
}
int
lisa_terminate(event)
const unsigned char *event;
{
	if(delivered || !terminate_entry) {fprintf(stderr,"lisa: original recursive Terminate delivered %d entry %lx\n",delivered,terminate_entry);_exit(lisa_services_close()<0?1:0);}
	memset(parent_event,0,sizeof parent_event);putlong(parent_event,getpid());putlong(parent_event+4,1);
	if(event) memcpy(parent_event+8,event,40);
	requested=1;return kill(getpid(),SIGTERM);
}
static void
desktop_provenance()
{
	unsigned long fp=frame.reg[15],next,ret;unsigned char *record;int i;
	if(desktop_seen)return;
	for(i=0;i<64 && !(fp&1);i++) {
		if(!contained(fp,8,0xf60000UL,0x20000UL) && !contained(fp,8,0xfff60000UL,0x20000UL)) return;
		record=lisa_pointer(fp,8,0);if(!record)return;
		next=getlong(record);ret=getlong(record+4);
		if(ret==0x64748UL && lisa_executable(ret,2)) {desktop_seen=1;fprintf(stderr,"lisa: original DeskMgr idle return %lx frame %lx usp %lx\n",ret,fp,frame.reg[0]);return;}
		if((next&0xffffffUL)<=(fp&0xffffffUL))return;fp=next;
	}
}
void
lisa_dispatch(vector,selector,stack)
unsigned long vector,selector,stack;
{
	unsigned char *args;
	int result,i;unsigned long bytes=0;sigset_t mask;
	dispatching=1;
	if(vector==33) {
		if(selector==58)desktop_provenance();
		for(i=0;i<sizeof lisa_os_rows/sizeof lisa_os_rows[0];i++) if(lisa_os_rows[i].selector==selector) {bytes=lisa_os_rows[i].pop;break;}
		if(selector==59)bytes=8;if(selector==144)bytes=7;
		args=lisa_pointer(stack,bytes,1);
		result=args?lisa_os_dispatch(selector,args,frame.reg):-1;
	} else {if(selector==58 || selector==148)desktop_provenance();result=lisa_hw_dispatch(&hardware,frame.reg,selector);}
	if(result<0 && errno==EINTR && pending) result=0;
	if(result<0) {fprintf(stderr,"lisa: service vector %lu selector %lu sp %lx failed: %s\n",vector,selector,stack,strerror(errno));lisa_services_close();_exit(1);}
	if(pending) {
		pending=0;
		if(delivered || !terminate_entry) lisa_default_terminate();
		sigemptyset(&mask);sigaddset(&mask,SIGTERM);if(sigprocmask(SIG_BLOCK,&mask,0)<0) lisa_default_terminate();
		route(frame.reg,frame.sr,frame.pc,requested?0:3);frame.pc=terminate_entry;
	}
	dispatching=0;
}
static int
conflicts(base,size)
unsigned long base,size;
{
	char path[40];prmap_t *maps;
	unsigned long low,high;
	int fd,n,i,result=-1;
	sprintf(path,"/proc/%05ld",(long)getpid());fd=open(path,O_RDONLY);if(fd<0) return -1;
	if(ioctl(fd,PIOCNMAP,&n)<0 || n<0 || n>4096) goto done;
	maps=calloc(n+1,sizeof *maps);if(!maps) goto done;
	if(ioctl(fd,PIOCMAP,maps)<0) {free(maps);goto done;}
	result=0;
	for(i=0;i<n;i++) {low=(unsigned long)maps[i].pr_vaddr;high=low+maps[i].pr_size;if(high<low || (base<high && base+size>low)) {errno=EBUSY;result=-1;break;}}
	free(maps);
done:close(fd);return result;
}
static int
map_fixed(base,size,protection,fd)
unsigned long base,size;
int protection,fd;
{
	void *p;
	if(mapped_count>=70 || conflicts(base,size)<0) return -1;
	p=mmap((void *)base,size,protection,MAP_SHARED|MAP_FIXED,fd,0);
	if(p!=(void *)base) return -1;
	mapped_base[mapped_count]=base;mapped_size[mapped_count++]=size;return 0;
}
static int
read_exact(path,out,n,hash)
const char *path,*hash;
unsigned char *out;
unsigned long n;
{
	struct stat st;int fd,k;unsigned long done=0;
	fd=open(path,O_RDONLY);if(fd<0) return -1;
	if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode) || st.st_size!=n) {close(fd);errno=EINVAL;return -1;}
	while(done<n) {k=read(fd,out+done,n-done);if(k<0 && errno==EINTR) continue;if(k<=0) {close(fd);errno=EIO;return -1;}done+=k;}
	close(fd);
	if(!lisa_digest(out,n,hash)) {errno=EINVAL;return -1;}return 0;
}
static int
backing(root,name,size,persistent)
const char *root,*name;
unsigned long size;
int persistent;
{
	char path[600];int fd;struct stat st;
	if(persistent) sprintf(path,"%s/state/%s",root,name);
	else sprintf(path,"%s/state/.native-%ld-%s",root,(long)getpid(),name);
	fd=open(path,O_RDWR|(persistent?O_CREAT:O_CREAT|O_EXCL),0600);if(fd<0) return -1;
	if(!persistent) unlink(path);
	if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode) || st.st_uid!=getuid() || (st.st_mode&077)!=0 || (st.st_size!=0 && st.st_size!=size) || ftruncate(fd,size)<0) {close(fd);errno=EINVAL;return -1;}
	return fd;
}
static int
inputs(root,media)
const char *root,*media;
{
	unsigned char *bytes;unsigned long i;char path[600];
	bytes=malloc(10424128UL);if(!bytes)return -1;
	stage="media identity";
	if(read_exact(media,bytes,10424128UL,"288960bfdc4511287cc7d34da6a0d2e3f9e2cc18d76e973fa21c72dfb8b6336b")<0) {free(bytes);return -1;}
	free(bytes);
	for(i=0;i<sizeof lisa_resources/sizeof lisa_resources[0];i++) {
		bytes=malloc(lisa_resources[i].size);if(!bytes)return -1;
		sprintf(path,"%s/%s",root,lisa_resources[i].path);stage=lisa_resources[i].path;
		if(read_exact(path,bytes,lisa_resources[i].size,lisa_resources[i].hash)<0) {free(bytes);return -1;}
		free(bytes);
	}
	return 0;
}
static int
bootstrap(root)
const char *root;
{
	unsigned char *raw;unsigned long i;
	char path[600];
	raw=malloc(120832UL);if(!raw)return -1;
	sprintf(path,"%s/resources/41.raw",root);stage="original jump-table bootstrap";
	if(read_exact(path,raw,120832UL,"fd50fe99e6e31abc5d64771fe88ff24529db49500ec9b8d47f9e89201957fe13")<0 || !lisa_digest(raw+1330,1984UL,"2f1e43ca1cf3d39acf143243f96e9d52baa3240c9b44faa326b2ab8699369d1b")) {free(raw);errno=EINVAL;return -1;}
	memcpy((void *)LISA_JUMPTABLE,raw+1330,1984);free(raw);
	for(i=0;i<sizeof lisa_unit_pointers/sizeof lisa_unit_pointers[0];i++) putlong((unsigned char *)(LISA_POINTERTABLE+4*(lisa_unit_pointers[i][0]-1)),lisa_unit_pointers[i][1]);
	memset((void *)LISA_INITIALRECORD,0,32);putlong((unsigned char *)(LISA_INITIALRECORD+16),10964);
	if(!lisa_digest((const unsigned char *)0xf60000UL,0x20000UL,lisa_objects[55].hash) || !lisa_digest((const unsigned char *)0xf80000UL,0x20000UL,lisa_objects[56].hash)) {errno=EINVAL;return -1;}
	return 0;
}
static int
load(root,surface_fd)
const char *root;
int surface_fd;
{
	unsigned long i,n,base,values[2],a;
	int fd,z;unsigned char *bytes;
	char path[600];struct lisa_object *object;struct lisa_region *region;
	page=sysconf(_SC_PAGESIZE);if(page<2048 || page>65536 || (page&(page-1))) {errno=EINVAL;return -1;}
	z=open("/dev/zero",O_RDWR);if(z<0) return -1;
	for(i=0;i<55;i++) {
		object=&lisa_objects[i];stage=object->path;n=(object->size+page-1)&~(page-1);base=object->base;
		if(map_fixed(base,n,3,z)<0) {close(z);return -1;}
		sprintf(path,"%s/%s",root,object->path);
		bytes=(unsigned char *)base;
		if(read_exact(path,bytes,object->size,object->hash)<0 || !lisa_digest(bytes+8,object->size-8,object->payload) || bytes[0]!=0x85 || bytes[1] || ((bytes[2]<<8)|bytes[3])!=object->size || getlong(bytes+4)!=base || mprotect((void *)base,n,5)<0) {close(z);errno=EINVAL;return -1;}
		region=&config.region[config.count++];region->base=base+8;region->size=object->size-8;region->flags=LP_CODE|LP_EXEC;region->mapped=base;region->mapped_size=n;
	}
	close(z);
	for(i=0;i<5;i++) {
		stage="paired owned mappings";
		if(i==3) fd=surface_fd;else fd=backing(root,i==0?"data.ram":i==1?"stack":i==2?"shared":"signal",alias_size[i],i==0);
		if(fd<0) return -1;
		if(map_fixed(alias_base[i],alias_size[i],alias_prot[i],fd)<0 || map_fixed(alias_base[i]|0xff000000UL,alias_size[i],alias_prot[i],fd)<0) {if(i!=3)close(fd);return -1;}
		if(i!=3)close(fd);
		if(i==1 || i==2) {
			memset((void *)alias_base[i],0,alias_size[i]);
		}
		region=&config.region[config.count++];region->base=alias_base[i];region->size=alias_size[i];region->flags=LP_WRITE|((alias_prot[i]&4)?LP_EXEC:0);region->mapped=alias_base[i];region->mapped_size=alias_size[i];
		for(a=0;a<alias_size[i];a+=2048) if(*(volatile unsigned char *)(alias_base[i]+a)!=*(volatile unsigned char *)((alias_base[i]|0xff000000UL)+a)) {errno=EINVAL;return -1;}
	}
	if(bootstrap(root)<0) return -1;
	for(i=0;i<16;i++)slot_prot[i]=3;
	config.version=LP_VERSION;config.handler=(unsigned long)lisa_service_entry;config.frame=(unsigned long)&frame;config.stack=((unsigned long)native_stack+sizeof native_stack-16)&~3UL;
	stage="bridge ENTER";
	bridge=open("/dev/lplatform",O_RDWR);if(bridge<0 || ioctl(bridge,LP_ENTER,&config)<0) return -1;attached=1;
	stage="owned page locks";
	for(i=0;i<5;i++) {
		values[0]=alias_base[i];values[1]=alias_size[i];if(ioctl(bridge,LP_LOCK,values)<0)return -1;
		values[0]|=0xff000000UL;if(ioctl(bridge,LP_LOCK,values)<0)return -1;
	}
	return ioctl(bridge,LP_PUBLISH,0);
}
static void
unload()
{
	lisa_services_close();
	if(attached) {ioctl(bridge,LP_LEAVE,0);attached=0;}
	if(bridge>=0)close(bridge);bridge=-1;
	while(mapped_count) {mapped_count--;munmap((void *)mapped_base[mapped_count],mapped_size[mapped_count]);}
}
static void
frontend_stop(signo)
int signo;
{
	frontstop=signo;
}
int
main(argc,argv)
int argc;
char **argv;
{
	const char *root=getenv("NATIVEHOST_ROOT"),*media=getenv("NATIVEHOST_MEDIA"),*device=getenv("NATIVEHOST_DISPLAY");
	struct sigaction action;int fd,status,result=1,frontbridge=-1,ready[2],go[2];pid_t child,done;unsigned char byte;
	if(!root || root[0]!='/' || strlen(root)>480 || !media || media[0]!='/') {fprintf(stderr,"lisa: absolute environment root and media required\n");return 2;}
	if(!device)device="/dev/fb0";
	fd=backing(root,"surface.ram",0x20000UL,1);if(fd<0) {perror("lisa: surface");return 1;}
	surface=mmap(0,0x20000UL,3,MAP_SHARED,fd,0);if(surface==(void *)-1) {close(fd);return 1;}memset(surface,0,0x20000UL);
	lisa_host_init(&host);
	if(lisa_host_open(&host,device,1)<0) {perror("lisa: display");goto out;}
	/* Fork before attaching the original execution context. */
	if(pipe(ready)<0 || pipe(go)<0)goto out;
	child=fork();if(child<0)goto out;
	if(!child) {
		close(ready[0]);close(go[1]);
		nh_display_close(&host.display);
		if(inputs(root,media)<0 || load(root,fd)<0) {fprintf(stderr,"lisa: admission stage %s page %lu failed: %s\n",stage,page,strerror(errno));unload();_exit(1);}close(fd);
		if(lisa_hw_init(&hardware,(void *)0xfa0000UL,validate,0)<0 || lisa_services_init(root,media)<0) {perror("lisa: services");unload();_exit(1);}
		hardware.interrupted=interrupted;
		memset(&action,0,sizeof action);action.sa_handler=(void (*)())signal_handler;action.sa_flags=SA_SIGINFO;sigemptyset(&action.sa_mask);sigaddset(&action.sa_mask,SIGTERM);
		if(sigaction(SIGTERM,&action,0)<0) {unload();_exit(1);}
		byte=1;if(write(ready[1],&byte,1)!=1 || read(go[0],&byte,1)!=1) {unload();_exit(1);}
		close(ready[1]);close(go[0]);
		fprintf(stderr,"lisa: native Office admitted %lu regions, pid %ld\n",config.count,(long)getpid());
		lisa_start(0x21cdeUL,0xf7f4a4UL);unload();_exit(1);
	}
	close(ready[1]);close(go[0]);
	if(read(ready[0],&byte,1)!=1 || lisa_host_publish(&host,surface)<0 || write(go[1],&byte,1)!=1) {kill(child,SIGTERM);waitpid(child,&status,0);goto out;}
	close(ready[0]);close(go[1]);
	close(fd);fd=-1;
	frontbridge=open("/dev/lplatform",O_RDWR);
	memset(&action,0,sizeof action);action.sa_handler=frontend_stop;sigemptyset(&action.sa_mask);if(sigaction(SIGTERM,&action,0)<0 || sigaction(SIGINT,&action,0)<0 || sigaction(SIGHUP,&action,0)<0) {perror("lisa: frontend signal association");kill(child,SIGTERM);waitpid(child,&status,0);goto out;}
	for(;;) {
		if(frontstop) {if(kill(child,SIGTERM)<0)perror("lisa: termination signal");frontstop=0;}
		if((frontbridge>=0 && ioctl(frontbridge,LP_PUBLISH,0)<0) || lisa_host_publish(&host,surface)<0 || lisa_host_audio(&host,surface)<0) {perror("lisa: host service");kill(child,SIGTERM);}
		if(lisa_host_render(&host,surface)<0 && errno!=EAGAIN) {perror("lisa: display");kill(child,SIGTERM);}
		done=waitpid(child,&status,WNOHANG);
		if(done==child) {result=WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);break;}
		if(done<0 && errno!=EINTR)break;
		poll((struct pollfd *)0,0,8);
	}
out:
	if(frontbridge>=0)close(frontbridge);
	lisa_host_close(&host);munmap(surface,0x20000UL);if(fd>=0)close(fd);return result;
}
