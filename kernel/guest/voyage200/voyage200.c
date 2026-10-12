#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/procfs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nativehost.h"
#include "profile.h"
#include "hash.h"

extern int munmap(), mprotect();
extern unsigned long v_call(), v_raw();
extern void v_idle();
#define W(a) (*(volatile unsigned short *)(a))
#define L(a) (*(volatile unsigned long *)(a))
#define B(a) (*(volatile unsigned char *)(a))
static struct nh_display display;
static volatile sig_atomic_t quitting, input_failed;
static int bridge = -1, attached, ram_owned, image_owned, zero_owned;
static unsigned int modifiers;
static unsigned char bitmap[3840];
static unsigned short home, system_app, graph;
static char *initial_stage = "heap";

static void
word(p, value)
unsigned char *p;
unsigned int value;
{
	p[0] = value >> 8; p[1] = value;
}
static void
longword(p, value)
unsigned char *p;
unsigned long value;
{
	word(p, value >> 16); word(p + 2, value);
}
static unsigned long
call0(entry)
unsigned long entry;
{
	return v_call(entry, (char *)0, 0L);
}
static unsigned long
callword(entry, value)
unsigned long entry;
unsigned int value;
{
	unsigned char args[2];
	word(args, value);
	return v_call(entry, args, 2L);
}
static unsigned long
record(handle)
unsigned int handle;
{
	unsigned long table = L(0x79ca), address;
	if (!handle || handle >= 2000 || table < 0x10000 || table > 0x2f000 - 8000)
		return 0;
	address = L(table + handle * 4);
	return !(address & 1) && address >= TI_RAM && address <= TI_RAM_END - 24 ? address : 0;
}
static unsigned short
register_app(identity, resource, predecessor)
unsigned long identity, resource;
unsigned int predecessor;
{
	unsigned char args[10];
	unsigned short handle;
	unsigned long r;
	longword(args, identity); longword(args + 4, resource); word(args + 8, predecessor);
	handle = v_call(TI_REGISTER, args, 10L);
	r = record(handle);
	if (!r || W(r + 2) != handle || L(r + 12) != identity || L(r + 20) != resource)
		return 0;
	return handle;
}
static void
dispatch(owner, event)
unsigned int owner;
unsigned char *event;
{
	unsigned char args[6];
	word(args, owner); longword(args + 2, (unsigned long)event);
	v_call(TI_DISPATCH, args, 6L);
}
static void
lifecycle(owner, kind)
unsigned int owner, kind;
{
	unsigned char event[32];
	memset(event, 0, sizeof event); word(event, kind);
	dispatch(owner, event);
}
static int
refresh()
{
	int i;
	for (i = 0; i < 16; i++)
		if (!(call0(TI_REFRESH) & 0xffff)) return 0;
	errno = EIO;
	return -1;
}
static int
initialize()
{
	unsigned long h, s, g;
	L(0x8428) = 0x10000; L(0x5de4) = 0x2f000;
	if ((call0(TI_HEAP_INIT) & 0xffff) != 1 || L(0x79ca) != 0x10000 ||
	    W(0x79b4) != 2000 || L(0x10000) != 0xffffffffUL) return -1;
	initial_stage = "math/error/expression stack";
	L(0x7d3e) = 0x2d168e;
	call0(TI_MATH_INIT); call0(TI_ERROR_INIT); call0(TI_EXPRESSION_INIT);
	if (!W(0x6182) || !W(0x68e0) || !L(0x68d4)) return -1;
	initial_stage = "graph/symbol/editor defaults";
	L(0x6a6e) = 0x72e6; L(0x6a72) = 0x7614;
	callword(TI_GRAPH_INIT, 0); call0(TI_CLIPBOARD_INIT); call0(TI_PARSER_INIT);
	if (!(call0(TI_SYMBOLS_INIT) & 0xffff)) return -1;
	call0(TI_EDITOR_INIT);
	if (!W(0x7bda) || !W(0x7be0) || !W(0x69d4)) return -1;
	initial_stage = "graphics/window defaults";
	L(0x7d3e) = 0x2d168e; call0(TI_GRAPHICS_INIT);
	B(0x6660) = B(0x6661) = 0; B(0x6662) = 239; B(0x6663) = 127;
	if (L(0x6648) != TI_BITMAP || W(0x665c) != 30) return -1;
	call0(TI_WINDOW_INIT);
	initial_stage = "Home/system registration";
	home = register_app(TI_HOME_ID, TI_HOME_RESOURCE, 0);
	system_app = register_app(TI_SYSTEM_ID, TI_SYSTEM_RESOURCE, 0);
	if (!home || !system_app) return -1;
	W(0x6474) = home; W(0x83be) = W(0x7d3c) = system_app;
	W(0x8062) = W(0x8064) = system_app; W(0x8066) = 0xfffd;
	W(0x8060) = W(0x806a) = 0;
	initial_stage = "cold/Home lifecycle";
	call0(TI_COLD_INIT); lifecycle(home, 0x701);
	W(0x8062) = W(0x8064) = home;
	callword(TI_HOME_ACTIVATE, 0); lifecycle(home, 0x703);
	if (W(0x6326) != 30 || !W(0x6370)) return -1;
	initial_stage = "Graph registration/lifecycle";
	graph = register_app(TI_GRAPH_ID, TI_GRAPH_RESOURCE, home);
	if (!graph) return -1;
	W(0x796c) = graph;
	h = record(home); s = record(system_app); g = record(graph);
	W(g + 6) = system_app; W(s + 4) = graph;
	if (W(g + 4) != home || W(h + 6) != graph || W(h + 4) || W(s + 6)) return -1;
	W(0x8062) = W(0x8064) = system_app;
	lifecycle(graph, 0x701); W(0x8062) = W(0x8064) = home;
	if (refresh() < 0) return -1;
	memset((void *)TI_QUEUE, 0, 40); W(TI_QUEUE + 4) = 32;
	return 0;
}
static void
queue(key)
unsigned int key;
{
	unsigned char args[6];
	word(args, key); longword(args + 2, TI_QUEUE);
	v_raw(TI_QUEUE_PUSH, args, 6L);
}
static unsigned int
key(code, down)
unsigned int code;
int down;
{
	static char normal[128] = {
		[0x00]='a',[0x01]='s',[0x02]='d',[0x03]='f',[0x04]='h',[0x05]='g',[0x06]='z',[0x07]='x',
		[0x08]='c',[0x09]='v',[0x0b]='b',[0x0c]='q',[0x0d]='w',[0x0e]='e',[0x0f]='r',
		[0x10]='y',[0x11]='t',[0x12]='1',[0x13]='2',[0x14]='3',[0x15]='4',[0x16]='6',[0x17]='5',
		[0x18]='=',[0x19]='9',[0x1a]='7',[0x1b]='-',[0x1c]='8',[0x1d]='0',[0x1e]=']',[0x1f]='o',
		[0x20]='u',[0x21]='[',[0x22]='i',[0x23]='p',[0x25]='l',[0x26]='j',[0x27]='\'',[0x28]='k',
		[0x29]=';',[0x2a]='\\',[0x2b]=',',[0x2c]='/',[0x2d]='n',[0x2e]='m',[0x2f]='.',[0x31]=' ',[0x32]='`',
		[0x41]='.',[0x43]='*',[0x45]='+',[0x4b]='/',[0x4e]='-',[0x51]='=',
		[0x52]='0',[0x53]='1',[0x54]='2',[0x55]='3',[0x56]='4',[0x57]='5',[0x58]='6',[0x59]='7',[0x5b]='8',[0x5c]='9'
	};
	static char shifted[128] = {
		[0x12]='!',[0x13]='@',[0x14]='#',[0x15]='$',[0x16]='^',[0x17]='%',
		[0x18]='+',[0x19]='(',[0x1a]='&',[0x1b]='_',[0x1c]='*',[0x1d]=')',[0x1e]='}',
		[0x21]='{',[0x27]='"',[0x29]=':',[0x2a]='|',[0x2b]='<',[0x2c]='?',[0x2f]='>',[0x32]='~'
	};
	unsigned int bit = code == 0x38 ? 1 : code == 0x7b ? 2 : code == 0x36 ? 4 : code == 0x7d ? 8 : 0;
	int c;
	if (bit) { if (down) modifiers |= bit; else modifiers &= ~bit; return 0; }
	if (!down || code >= 128) return 0;
	if (modifiers & 12) {
		if (code == 0x22) return 189;
		if (code == 0x25) return 263;
		if (code == 0x0c) { quitting = 1; return 264; }
		return 0;
	}
	switch (code) {
	case 0x24: case 0x34: case 0x4c: return 13;
	case 0x33: return 257; case 0x35: return 264;
	case 0x3b: return 337; case 0x3c: return 340; case 0x3d: return 338; case 0x3e: return 344;
	case 0x7a: return 268; case 0x78: return 269; case 0x63: return 270;
	case 0x76: return 271; case 0x60: return 272; case 0x61: return 273;
	}
	c = normal[code];
	if (modifiers & 3) {
		if (shifted[code]) c = shifted[code];
		else if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
	}
	return c >= 32 && c <= 126 ? c : 0;
}
static void
present()
{
	unsigned char *target = (unsigned char *)display.mapping + display.info.fi_offset;
	unsigned int x,y,dx,dy,ink;
	if (!memcmp(bitmap, (void *)TI_BITMAP, sizeof bitmap)) return;
	memcpy(bitmap, (void *)TI_BITMAP, sizeof bitmap);
	for(y=0;y<128;y++) for(x=0;x<240;x++) {
		ink = bitmap[y*30+x/8] & (0x80>>(x%8)) ? 0 : 255;
		for(dy=0;dy<3;dy++) for(dx=0;dx<3;dx++)
			target[(108+y*3+dy)*display.info.fi_rowbytes+40+x*3+dx] = ink;
	}
}
static void
wake(signal)
int signal;
{
	struct inev event;
	unsigned int action;
	int n, saved = errno, count = 0;
	while(count++ < 256 && (n = nh_input(display.keyboard, &event)) > 0) {
		if (event.ie_type == IE_DROP) { modifiers = 0; continue; }
		if (event.ie_type != IE_KEY) continue;
		action = key(event.ie_code, event.ie_value != 0);
		if (action) queue(action);
	}
	if (count <= 256 && n < 0 && errno != EINTR && errno != EAGAIN) {
		input_failed = quitting = 1; queue(264);
	}
	present();
	errno = saved;
}
void
v_wait()
{
	struct pollfd p;
	p.fd = display.keyboard; p.events = POLLIN; p.revents = 0;
	poll(&p, 1, 20); wake(0);
}
static void
stop(signal)
int signal;
{
	quitting = 1;
	if (attached) queue(264);
}
static int
conflicts()
{
	char name[40];
	prmap_t *maps;
	int fd, n, i, result = -1;
	unsigned long lo, hi;
	sprintf(name, "/proc/%05ld", (long)getpid());
	fd = open(name, O_RDONLY);
	if (fd < 0) return -1;
	maps = calloc(4097, sizeof *maps);
	if (!maps) goto done;
	if (ioctl(fd, PIOCNMAP, &n) < 0 || n < 0 || n > 4096) { free(maps); goto done; }
	if (ioctl(fd, PIOCMAP, maps) == 0) {
		result = 0;
		for(i=0;i<n;i++) {
			lo = (unsigned long)maps[i].pr_vaddr; hi = lo + maps[i].pr_size;
			if (hi < lo || (lo < TI_RAM_END && hi > 0) ||
			    (lo < TI_IMAGE_END && hi > TI_IMAGE)) { errno = EBUSY; result = -1; break; }
		}
	}
	free(maps);
done:
	close(fd); return result;
}
static int
admission(path)
char *path;
{
	static unsigned char expected[32] = {
		0xac,0xf4,0xdb,0x4c,0xa0,0xb3,0xd9,0x55,0x1f,0xac,0xd6,0xfa,0xd1,0xdc,0x0c,0x6c,
		0xbd,0xd5,0xb3,0x9f,0xb5,0x52,0xaf,0x18,0xc3,0x89,0x0c,0xf9,0x16,0x0f,0x36,0x5a
	};
	unsigned char digest[32], *image, *zero;
	struct stat st;
	int fd = -1, z = -1, result = -1;
	unsigned long done = 0, i;
	long n;
	if (sysconf(_SC_PAGESIZE) != 4096 || conflicts() < 0) return -1;
	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size != TI_IMAGE_SIZE) goto out;
	z = open("/dev/zero", O_RDWR); if (z < 0) goto out;
	image = mmap((void *)TI_IMAGE, TI_IMAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED, z, 0);
	if (image == (void *)-1) goto out;
	image_owned = 1;
	while(done<TI_IMAGE_SIZE) {
		n = read(fd,image+done,TI_IMAGE_SIZE-done);
		if(n<0 && errno==EINTR)continue;
		if(n<=0)goto out;
		done+=n;
	}
	v_sha256(image, TI_IMAGE_SIZE, digest);
	if(memcmp(digest,expected,32) || v_crc(image,TI_IMAGE_SIZE)!=TI_IMAGE_CRC ||
	    v_crc(image+0x21000,4096)!=0xedbebefaUL || W(0x221102)!=0x40c1 ||
	    W(0x22115c)!=0x40c1 || W(0x221130)!=0x44c1 || W(0x221188)!=0x44c1) {errno=ENOEXEC;goto out;}
	if(mprotect(image,TI_IMAGE_SIZE,PROT_READ|PROT_EXEC)<0 ||
	    mprotect(image+0x21000,4096,PROT_READ|PROT_WRITE)<0)goto out;
	W(0x221102)=W(0x22115c)=0x42c1;
	if(mprotect(image+0x21000,4096,PROT_READ|PROT_EXEC)<0 ||
	    v_crc(image+0x21000,4096)!=0xfc5c9fbbUL || v_crc(image,TI_IMAGE_SIZE)!=TI_EXEC_CRC)goto out;
	if(mmap((void *)TI_RAM,TI_RAM_END-TI_RAM,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_FIXED,z,0)==(void *)-1)goto out;
	ram_owned=1;
	zero=mmap((void *)0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_FIXED,z,0);
	if(zero==(void *)-1)goto out;
	zero_owned=1;
	for(i=0;i<4096;i++) *((volatile unsigned char *)zero+i)=image[i];
	if(W(0)!=0 || mprotect(zero,4096,PROT_READ)<0)goto out;
	result=0;
out:
	if(fd>=0)close(fd);if(z>=0)close(z);return result;
}
int
main(argc, argv)
int argc;
char **argv;
{
	char *media = getenv("NATIVEHOST_MEDIA");
	struct sigaction action;
	struct nh_clock clock;
	struct fbcmap colors;
	unsigned short red[256], green[256], blue[256];
	unsigned char event[32], args[6];
	long interval = 0, idle;
	int status = 1, i;
	if(argc>1)media=argv[1];
	nh_display_init(&display);
	if(!media) { fprintf(stderr,"voyage200: supply the admitted image path\n");return 1; }
	if(admission(media)<0) {perror("voyage200 image admission");goto cleanup;}
	bridge=open("/dev/tiplatform",O_RDWR);
	if(bridge<0 || ioctl(bridge,TI_ENTER,0)<0){perror("voyage200 native bridge");goto cleanup;}
	attached=1;
	if(nh_display_open(&display,"/dev/fb0","Voyage 200",1)<0 || display.info.fi_depth!=8 ||
	    display.info.fi_width<760 || display.info.fi_height<492) {perror("voyage200 display");goto cleanup;}
	for(i=0;i<256;i++)red[i]=green[i]=blue[i]=i*257;
	memset(&colors,0,sizeof colors);colors.cm_count=256;colors.cm_red=red;colors.cm_green=green;colors.cm_blue=blue;
	if(ioctl(display.framebuffer,FBIOPUTCMAP,&colors)<0) {perror("voyage200 palette");goto cleanup;}
	memset(display.mapping,255,display.info.fi_size);memset(bitmap,255,sizeof bitmap);
	memset(&action,0,sizeof action);sigemptyset(&action.sa_mask);action.sa_handler=wake;
	if(sigaction(SIGALRM,&action,0)<0)goto cleanup;
	action.sa_handler=stop;sigaddset(&action.sa_mask,SIGALRM);
	sigaction(SIGINT,&action,0);sigaction(SIGTERM,&action,0);sigaction(SIGHUP,&action,0);
	idle=(long)v_idle;
	if(ioctl(bridge,TI_IDLE,&idle)<0) {perror("voyage200 idle");goto cleanup;}
	if(initialize()<0) {fprintf(stderr,"voyage200 initialization failed: %s\n",initial_stage);goto cleanup;}
	if(nh_clock_read(&clock)<0) {perror("voyage200 clock");goto cleanup;}
	interval=clock.frequency/50;if(interval<1)interval=1;
	if(ioctl(bridge,TI_EVENTS,&interval)<0)goto cleanup;
	while(!quitting) {
		memset(event,0,sizeof event);word(args,0);longword(args+2,(unsigned long)event);
		v_call(TI_COLLECT,args,6L);
		if(quitting)break;
		if(!record(W(0x8062))) {errno=EINVAL;goto cleanup;}
		dispatch(W(0x8062),event);
		if(refresh()<0)goto cleanup;
		present();ioctl(display.framebuffer,FBIOVBLWAIT,1);
	}
	status=input_failed?1:0;
cleanup:
	signal(SIGALRM,SIG_IGN);interval=0;
	if(attached){ioctl(bridge,TI_EVENTS,&interval);ioctl(bridge,TI_LEAVE,0);attached=0;}
	if(bridge>=0)close(bridge);
	nh_display_close(&display);
	if(zero_owned)munmap((void *)0,4096);
	if(ram_owned)munmap((void *)TI_RAM,TI_RAM_END-TI_RAM);
	if(image_owned)munmap((void *)TI_IMAGE,TI_IMAGE_SIZE);
	return status;
}
