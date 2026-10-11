/*
 * t_mac.c -- the Mac environment of A/UX: /dev/uinter0 (the uinter
 * module) and startmac.
 *
 * Registers auxexec for magic 0x150 and uinter for character major 54
 * (modules in /tests/aux/mod.d), then:
 *   - native: the first open loads uinter, GETVERSION is 5, layer
 *     commands refuse a native process, the module is held while open;
 *   - macabi, an A/UX program that makes itself a Mac task: its lines
 *     become results (A-line frames, privileged instructions, the
 *     virtual IPL, the tick, the read-only ROM, low memory, PRAM);
 *   - macjoin: a second A/UX process joins a session (UI_SYNC,
 *     UI_ATTACHLAYER, UI_SWITCH): one runs Mac code at a time, the tick
 *     and signals go to the one running, a joiner killed waiting or
 *     running hands the Mac back, and joiners go with the session;
 *   - startmac with TBVERBOSE, TBWARN and the System Folder
 *     /mac/sys/Sys7: its output is relayed to the console ("mac| "),
 *     the kernel records the task's uinter commands and opens in its
 *     trace ring, which goes to the log afterwards ("ktrace| "); checks:
 *     the Mac side passed doDispatch (its Memory Manager reports
 *     SysZone and ApplZone) and opened the System file after UI_SET;
 *     its display session comes to front and, on a direct boot with
 *     the host line, the screen dump shows the grey desktop
 *     (display/mac_desktop.png); Control-Option-Command-0 brings the
 *     console back, -1 the Mac, and its exit the console.
 * Skips without guest support, without the uinter module or without
 * the A/UX and ROM files, which are local only.
 *
 * Built with SYS76 (t_mac76.c), the startmac run uses the Mac OS 7.6.1
 * System Folder /macsys/S761, on a ufs volume (SCSI disk 1), as a
 * Quadra 800 (box flag 29) and checks 'boot' 3's second _AUXDispatch(36)
 * (it makes $HOME/.mac/unix, 0700), the Finder desktop, SysVersion $0761, the
 * About This Computer window, SimpleText opened from the desktop and
 * quit, the disk window and root's Shut Down, which halts.  Before it a
 * user's session (/macsys/S761u) shows Log Out in the Special menu, then
 * a root session; both end with the Apple menu's Log Out.
 * Built with SYS81 (t_mac81.c), the same with Mac OS 8.1 (/macsys/S81),
 * without the user's session.
 * Built with SYS6 (t_mac6.c), A/UX 2.0.1's startmac runs System 6.0.7
 * from its own root, /a201, on the IIci's ROM in 4 MB; checks as t_mac's
 * up to the System file, then the Finder's desktop without an alert,
 * SysVersion $0607, the Apple and Special menus, and the startup disk's
 * window opened and closed.  The session ends from the Finder: the first
 * run in a boot by Special > Logout, the next by the Apple menu's Log Out.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mkdev.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <termio.h>
#include <sys/resource.h>
#include <sys/procfs.h>
#include <dirent.h>
#include "sys/mod.h"
#include "dsio.h"
#include "t.h"

extern int getksym();

#define	MD	"/tests/aux/mod.d"
#define	UIMAJ	54
#define	TLOW	0x744c4f57		/* 'tLOW', the Mac RAM segment */
#define	LOGMAX	65536
#define	TBUF	8192		/* AUX_TBUF */
#define	PRAMF	"/etc/aux/pram"
#define	OTHERUID "101"		/* restart's user, group display */

#ifdef SYS6
#define	ROOT6	"/a201"
#define	SYSDIR	ROOT6 "/mac/sys/Sys6"
#define	SYS6DIR	ROOT6 "/mac/sys/System Folder"
#define	TBSYS	"/mac/sys/System Folder"
#define	SYSVER	0x0607
#define	TNAME	"mac6"
#define	SHOT	"mac6_"
#define	TBMEM	"TBMEMORY=4M"
#define	STARTMAC ROOT6 "/mac/bin/startmac"
#else
#define	STARTMAC "/mac/bin/startmac"
#endif
#ifndef TBSYS
#define	TBSYS	SYSDIR
#endif
#define	MACVOL	"/macsys"
#define	MACDEV	"/dev/dsk/c1d0s0"
#ifdef SYS6
#else
#ifdef SYS81
#define	SYS76
#define	SYSDIR	MACVOL "/S81"
#define	USERSYS	MACVOL "/S81u"
#define	SYSVER	0x0810
#ifdef COFF81
#ifdef COFF81D
#define TNAME "coff81d"
#define SHOT "coff81d_"
#else
#define TNAME "coff81"
#define SHOT "coff81_"
#endif
#else
#define	TNAME	"mac81"
#define	SHOT	"mac81_"
#endif
#define	TBMEM	"TBMEMORY=32M"
#else
#ifdef SYS76
#define	SYSDIR	MACVOL "/S761"
#define	USERSYS	MACVOL "/S761u"
#define	SYSVER	0x0761
#ifdef COFF76
#ifdef COFF76C
#define TNAME "coff76c"
#define SHOT "coff76c_"
#else
#define TNAME "coff76"
#define SHOT "coff76_"
#endif
#else
#define	TNAME	"mac76"
#define	SHOT	"mac76_"
#endif
#define	TBMEM	"TBMEMORY=16M"
#else
#define	SYSDIR	"/mac/sys/Sys7"
#define	TBMEM	"TBMEMORY=8M"
#define	SHOT	"mac_"
#endif
#endif
#endif
static char *macenv[] = {
	"PATH=/aux/bin:/usr/bin:/sbin", "HOME=/tmp", "TBVERBOSE=1", "TBWARN=1",
	"TBSYSTEM=" TBSYS, TBMEM, 0
};
static char out[8192];
static char klog[LOGMAX + 1];
static int zones;		/* 1: SysZone, 2: ApplZone reported */
static int novideo;		/* "No Suitable Video" seen */
static long macid;		/* the Mac's session, once in front */
static long mact;		/* when it came to front, ms */
static int hfd = -1, hseq;	/* host line */
static char hrep[128];
static int scrw = 800, scrh = 600;	/* the Mac's screen */

/* the desktop pattern under the menu bar: white, or 8.1's grey Platinum one */
/* a 50% grey desktop, or a tiled picture such as the CD's */
#define DESKOK(chk, top, tx, ty, dom) ((SYSVER == 0x0810 ? \
    (top) >= 20 && (top) < 300 : (top) >= 900) && ((chk) >= 900 || \
    ((tx) >= 995 && (ty) >= 995 && (dom) < 900)))
#define	SETTLE	25
#ifdef SYS81
static long heap81(), g32();
static int rdmac();
static long macwat;		/* a master pointer the first session's heap lost */
#endif		/* s from the Mac in front to the screen dump */

/* a host request (display/hostio.py); its answer, 0 if none in 20 s */
static char *
host(req)
char *req;
{
	char buf[160], line[256], *nl, *s;
	struct pollfd p;
	int n, len = 0, seq;
	long t0 = t_now_ms();

	if (hfd < 0)
		return 0;
	sprintf(buf, "@@ %d %s\n", ++hseq, req);
	write(hfd, buf, strlen(buf));
	p.fd = hfd;
	p.events = POLLIN;
	while (t_now_ms() - t0 < 20000) {
		if (poll(&p, 1L, 500) <= 0 || (n = read(hfd, line + len, sizeof line - 1 - len)) <= 0)
			continue;
		len += n;
		line[len] = 0;
		while ((nl = strchr(line, '\n')) != 0) {
			*nl = 0;
			if ((s = strstr(line, "@@ok ")) != 0 && sscanf(s + 5, "%d", &seq) == 1 &&
			    seq == hseq) {
				s = strchr(s + 5, ' ');
				strncpy(hrep, s ? s + 1 : "", sizeof hrep - 1);
				hrep[strcspn(hrep, "\r")] = 0;
				return hrep;
			}
			if ((s = strstr(line, "@@err ")) != 0 && sscanf(s + 6, "%d", &seq) == 1 &&
			    seq == hseq)
				return strcpy(hrep, "error");
			len -= nl + 1 - line;
			memmove(line, nl + 1, len + 1);
		}
		if (len > 200)
			len = 0;
	}
	return 0;
}

static void
hostopen()
{
	struct termio t;

	if ((hfd = open("/dev/term/b", O_RDWR | O_NOCTTY)) < 0)
		return;
	if (ioctl(hfd, TCGETA, &t) == 0) {
		t.c_iflag = IGNCR;
		t.c_oflag = 0;
		t.c_lflag = ICANON;
		t.c_cflag |= CREAD | CLOCAL;
		ioctl(hfd, TCSETAF, &t);
	}
	if (host("ping") == 0 || strcmp(hrep, "pong") != 0) {
		close(hfd);
		hfd = -1;
	}
}

/* the session in front, 0 the console, -1 no display */
static long
front()
{
	struct fbstate st;
	int fd = open("/dev/fb0", O_RDWR);

	st.st_front = -1;
	if (fd >= 0) {
		if (ioctl(fd, FBIOGSTATE, &st) < 0)
			st.st_front = -1;
		close(fd);
	}
	return st.st_front;
}

static int
waitfront(id, secs)
long id;
int secs;
{
	long t0 = t_now_ms();

	while (t_now_ms() - t0 < secs * 1000L) {
		if (front() == id)
			return 1;
		poll((struct pollfd *)0, 0L, 200);
	}
	return 0;
}

/* a kernel or module long through /dev/kmem */
static int
kmemrw(addr, val, wr)
unsigned long addr;
long *val;
int wr;
{
	int fd = open("/dev/kmem", wr ? O_RDWR : O_RDONLY), ok;

	if (fd < 0)
		return -1;
	ok = lseek(fd, (off_t)addr, 0) != -1 &&
	    (wr ? write(fd, (char *)val, 4) : read(fd, (char *)val, 4)) == 4;
	close(fd);
	return ok ? 0 : -1;
}

static int
setsym(name, v)
char *name;
long v;
{
	unsigned long a = 0, info;	/* 0: look up by name */

	if (getksym(name, &a, &info) < 0)
		return -1;
	return kmemrw(a, &v, 1);
}

static long
getsym(name)
char *name;
{
	unsigned long a = 0, info;
	long v;

	if (getksym(name, &a, &info) < 0 || kmemrw(a, &v, 0) < 0)
		return -1;
	return v;
}

static pid_t macpid;
static void pause_ms();

static int
cofframrange(fd,address,n)
int fd;
unsigned long address,n;
{
    prpsinfo_t ps;prmap_t maps[129];struct shmid_ds ram;
    unsigned long base,size;int count,id,i;
    if(!n || address>0xffffffffUL-n ||
        ioctl(fd,PIOCPSINFO,&ps)!=0 || ps.pr_zomb ||
        !(ps.pr_pid==macpid || (ps.pr_ppid==macpid && !strcmp(ps.pr_fname,"CommandShell"))) ||
        (id=shmget(TLOW,0,0))<0 || shmctl(id,IPC_STAT,&ram)!=0 || ram.shm_cpid!=macpid ||
        address>=(unsigned long)ram.shm_segsz || n>(unsigned long)ram.shm_segsz-address ||
        ioctl(fd,PIOCNMAP,&count)!=0 || count<1 || count>128)return 0;
    memset(maps,0,sizeof maps);
    if(ioctl(fd,PIOCMAP,maps)!=0)return 0;
    for(i=0;i<count;i++) {
        base=(unsigned long)maps[i].pr_vaddr;size=maps[i].pr_size;
        if((maps[i].pr_mflags&(MA_READ|MA_SHARED))==(MA_READ|MA_SHARED) &&
            base<=address && address-base<size && n<=size-(address-base))return 1;
    }
    return 0;
}

static void
coffcommtarget(fd)
int fd;
{
    prpsinfo_t ps;unsigned char bytes[32];char hex[65];long target;int i,j;

    if(ioctl(fd,PIOCPSINFO,&ps)!=0 || ps.pr_pid!=macpid ||
        ps.pr_ppid!=getpid() || ps.pr_zomb)return;
    if(lseek(fd,(off_t)0x62cL,0)==-1 || read(fd,(char *)bytes,4)!=4)return;
    target=((long)bytes[0]<<24)|((long)bytes[1]<<16)|((long)bytes[2]<<8)|bytes[3];
    t_info("coff_comm_target","%lx",target);
    if((target&1) || !((target>=0x10000L && cofframrange(fd,target,128)) ||
        (target>=0x40800000L && target<0x40900000L)))return;
    for(i=0;i<128;i+=32) {
        if(lseek(fd,(off_t)(target+i),0)==-1 || read(fd,(char *)bytes,32)!=32)break;
        for(j=0;j<32;j++)sprintf(hex+2*j,"%02x",bytes[j]);
        t_info("coff_comm_code","%lx %s",target+i,hex);
    }
}

static int
coffterminal(pid,verbose)
int pid,verbose;
{
    static unsigned char signature[]={0x20,0x79,0x10,0x04,0xc2,0x68,
        0xb1,0xf9,0x10,0x04,0xc2,0x50,0x66,0x20,0x4a,0x79,0x10,0x04,0x10,0xc8};
    unsigned char code[sizeof signature];
    char path[48];prpsinfo_t ps;
    long front=0,console=0;short reading=0,mode=0;
    int fd,result=-1;
    sprintf(path,"/proc/%d",pid);
    if(pid<=1 || (fd=open(path,O_RDONLY))<0)return -1;
    if(ioctl(fd,PIOCPSINFO,&ps)==0 && ps.pr_pid==pid && ps.pr_ppid==macpid &&
        !strcmp(ps.pr_fname,"CommandShell") && !ps.pr_zomb &&
        lseek(fd,(off_t)0x100123dcL,0)!=-1 &&
        read(fd,(char *)code,sizeof code)==sizeof code && !memcmp(code,signature,sizeof code) &&
        lseek(fd,(off_t)0x1004c268L,0)!=-1 && read(fd,(char *)&front,4)==4 &&
        lseek(fd,(off_t)0x1004c250L,0)!=-1 && read(fd,(char *)&console,4)==4 &&
        lseek(fd,(off_t)0x100410c8L,0)!=-1 && read(fd,(char *)&reading,2)==2 &&
        lseek(fd,(off_t)0x10040872L,0)!=-1 && read(fd,(char *)&mode,2)==2) {
        result=front!=0 && front!=console;
        if(verbose)t_info("coff_terminal_focus","front %lx console %lx reading %d mode %d ordinary %d",
            front,console,(int)reading,(int)mode,result);
    }
    close(fd);return result;
}

static void
coffordinary(pid)
int pid;
{
    int i,state=coffterminal(pid,1);
    if(state==0) {
        host("key meta_l+n");
        for(i=0;i<100 && coffterminal(pid,0)==0;i++)pause_ms(100L);
        state=coffterminal(pid,1);
    }
    t_check("coff_ordinary_terminal",state==1,"authenticated foreground backend %d",state);
}

static unsigned long
coffword(p)
unsigned char *p;
{
    return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];
}

static int
coffrect(port,bitmap,w,h,box)
unsigned char *port,*bitmap;
int w,h,*box;
{
    int t,l,b,r,i;
    t=(short)(((unsigned)port[16]<<8)|port[17])-(short)(((unsigned)bitmap[0]<<8)|bitmap[1]);
    l=(short)(((unsigned)port[18]<<8)|port[19])-(short)(((unsigned)bitmap[2]<<8)|bitmap[3]);
    b=(short)(((unsigned)port[20]<<8)|port[21])-(short)(((unsigned)bitmap[0]<<8)|bitmap[1]);
    r=(short)(((unsigned)port[22]<<8)|port[23])-(short)(((unsigned)bitmap[2]<<8)|bitmap[3]);
    box[0]=l+12;box[1]=t+12;box[2]=r-24;box[3]=b-24;
    for(i=0;i<2;i++)if(box[i]<0)box[i]=0;
    if(box[2]>w)box[2]=w;if(box[3]>h)box[3]=h;
    return box[2]-box[0]>=240 && box[3]-box[1]>=40;
}

static int
coffvisible(pid)
int pid;
{
    unsigned char ptr[4],port[24],bitmap[14];
    char path[48],request[160],*reply,*q="echo auxshellvisibleoutputauxshellvisibleoutput";
    prpsinfo_t ps;unsigned long window,pixmap;
    int fd,w=0,h=0,box[4],rows=-1,pixels=-1,ok=0;
    if(coffterminal(pid,0)<0)return 0;
    sprintf(path,"/proc/%d",pid);
    if((fd=open(path,O_RDONLY))<0)return 0;
    if(ioctl(fd,PIOCPSINFO,&ps)!=0 || ps.pr_pid!=pid || ps.pr_ppid!=macpid ||
        strcmp(ps.pr_fname,"CommandShell") || ps.pr_zomb ||
        lseek(fd,(off_t)0x1004c260L,0)==-1 || read(fd,(char *)ptr,4)!=4)goto out;
    window=coffword(ptr);
    if(window<0x10000L || !cofframrange(fd,window,sizeof port) ||
        lseek(fd,(off_t)window,0)==-1 || read(fd,(char *)port,sizeof port)!=sizeof port)goto out;
    if((port[6]&0xc0)==0xc0) {
        pixmap=coffword(port+2);
        if(pixmap<0x10000L || !cofframrange(fd,pixmap,4) ||
            lseek(fd,(off_t)pixmap,0)==-1 || read(fd,(char *)ptr,4)!=4)goto out;
        pixmap=coffword(ptr);
        if(pixmap<0x10000L || !cofframrange(fd,pixmap,sizeof bitmap) ||
            lseek(fd,(off_t)pixmap,0)==-1 || read(fd,(char *)bitmap,sizeof bitmap)!=sizeof bitmap)goto out;
        memmove(bitmap,bitmap+6,8);
    } else memcpy(bitmap,port+8,8);
    reply=host("shot " SHOT "coff_text_before");
    if(!reply || sscanf(reply,"%d %d",&w,&h)!=2 || !coffrect(port,bitmap,w,h,box))goto out;
    for(;*q;q++) {
        if(*q==' ')host("key spc");
        else {sprintf(request,"key %c",*q);host(request);}
    }
    host("key ret");pause_ms(1500L);
    host("shot " SHOT "coff_text_after");
    sprintf(request,"glyph " SHOT "coff_text_before " SHOT "coff_text_after %d %d %d %d",
        box[0],box[1],box[2],box[3]);
    reply=host(request);
    if(reply)sscanf(reply,"glyph %d %d",&rows,&pixels);
    ok=rows>=4 && rows<=24 && pixels>=128;
    t_info("coff_visible_pixels","rect %d,%d,%d,%d rows %d pixels %d",
        box[0],box[1],box[2],box[3],rows,pixels);
out:
    close(fd);return ok;
}

static unsigned long
cofffsword(b)
unsigned char *b;
{
    return ((unsigned long)b[0]<<24)|((unsigned long)b[1]<<16)|((unsigned long)b[2]<<8)|b[3];
}

static int
cofffsread(fd,address,b,n)
int fd,n;
long address;
unsigned char *b;
{
    return address>=0 && cofframrange(fd,(unsigned long)address,(unsigned long)n) &&
        lseek(fd,(off_t)address,0)!=-1 && read(fd,(char *)b,n)==n;
}

static void
cofffsdump(fd,label,address,n)
int fd,n;
char *label;
long address;
{
    unsigned char b[32];char hex[65];int j,offset,m;
    for(offset=0;offset<n;offset+=32) {
        m=n-offset<32?n-offset:32;
        if(!cofffsread(fd,address+offset,b,m))return;
        for(j=0;j<m;j++)sprintf(hex+2*j,"%02x",b[j]);
        t_info(label,"%lx +%d %s",address,offset,hex);
    }
}

static void
cofffsclose(fd,stack)
int fd;
unsigned long stack;
{
    static unsigned char hook[]={0x2f,0x00,0x0c,0x01,0x00,0x60,0x66,0x2a,0x0c,0x00};
    static unsigned char tail[]={0x4e,0xf0,0x01,0xf1,0x00,0x08,0x7d,0x28};
    unsigned char b[32],pb[26],fcb[24];unsigned long original,table,vcb,globals;
    unsigned short ref,size,length;
    if(!cofffsread(fd,0x13596L,b,sizeof hook) || memcmp(b,hook,sizeof hook) ||
        !cofffsread(fd,0x135daL,b,sizeof tail) || memcmp(b,tail,sizeof tail))return;
    cofffsdump(fd,"coff_fs_shadow_state",0x88694L,12);
    cofffsdump(fd,"coff_fs_chain",0x87d24L,16);
    cofffsdump(fd,"coff_fs_trap_entries",0x400L,20);
    if(cofffsread(fd,0x2b6L,b,4)) {
        globals=cofffsword(b);
        if(globals>=0x10000UL && cofframrange(fd,globals,0x188))
            cofffsdump(fd,"coff_fs_iodone_vector",globals+0x184,4);
    }
    if(!cofffsread(fd,0x88698L,b,4))return;
    original=cofffsword(b);
    if(original>=0x10000UL && cofframrange(fd,original,64))
        cofffsdump(fd,"coff_fs_original_pb",original,64);
    else if(stack>=0x10000UL && stack<=0xffffefffUL &&
        original>=stack && original<=stack+4096UL-64) {
        int offset,j;char hex[65];
        for(offset=0;offset<64;offset+=32) {
            if(lseek(fd,(off_t)(original+offset),0)==-1 || read(fd,(char *)b,32)!=32)break;
            for(j=0;j<32;j++)sprintf(hex+2*j,"%02x",b[j]);
            t_info("coff_fs_original_pb","%lx +%d %s",original,offset,hex);
        }
    }
    if(!cofffsread(fd,0x886a0L,pb,sizeof pb))return;
    ref=((unsigned short)pb[24]<<8)|pb[25];
    if(!cofffsread(fd,0x3f6L,b,2))return;
    size=((unsigned short)b[0]<<8)|b[1];
    if(size!=94 || ref<2 || (ref-2)%size)return;
    if(!cofffsread(fd,0x34eL,b,4))return;
    table=cofffsword(b);
    if(table<0x10000UL || !cofframrange(fd,table,2) || !cofffsread(fd,table,b,2))return;
    length=((unsigned short)b[0]<<8)|b[1];
    if(length<2 || ref>length || size>length-ref || !cofframrange(fd,table,length))return;
    if(!cofffsread(fd,table+ref,fcb,sizeof fcb))return;
    t_info("coff_fs_fcb","table %lx length %u size %u ref %x file %lx flags %x vcb %lx",
        table,(unsigned)length,(unsigned)size,(unsigned)ref,cofffsword(fcb),
        ((unsigned)fcb[4]<<8)|fcb[5],cofffsword(fcb+20));
    cofffsdump(fd,"coff_fs_fcb_bytes",table+ref,94);
    vcb=cofffsword(fcb+20);
    if(vcb>=0x10000UL && cofframrange(fd,vcb,82) && !(vcb&1))
        cofffsdump(fd,"coff_fs_vcb_identity",vcb+0x4c,6);
}

static void
coffqdchain(fd,a5)
int fd;
long a5;
{
    unsigned char b[32];char hex[65];unsigned long slot,port;int i,j,n;
    if(a5<0x10000L || !cofframrange(fd,a5,4) || lseek(fd,(off_t)a5,0)==-1 || read(fd,(char *)b,4)!=4)return;
    slot=coffword(b);
    t_info("coff_qd_slot","A5 %lx slot %lx",a5,slot);
    if(slot<0x10000L || !cofframrange(fd,slot,4) || lseek(fd,(off_t)slot,0)==-1 || read(fd,(char *)b,4)!=4)return;
    port=coffword(b);
    t_info("coff_qd_native_port","A5 %lx port %lx",a5,port);
    if(port<0x10000L || !cofframrange(fd,port,108))return;
    for(i=0;i<108;i+=32) {
        n=108-i<32?108-i:32;
        if(lseek(fd,(off_t)(port+i),0)==-1 || read(fd,(char *)b,n)!=n)break;
        for(j=0;j<n;j++)sprintf(hex+2*j,"%02x",b[j]);
        t_info("coff_qd_native_bytes","%lx +%d %s",port,i,hex);
    }
}

static void
coffqdtargets(fd)
int fd;
{
    unsigned char b[32];char hex[65];unsigned long target;int i,j,k;
    static unsigned short traps[6]={0xa874,0xa882,0xa884,0xa885,0xa8ec,0xa901};
    cofffsdump(fd,"coff_window_manager",0x9d6L,16);
    cofffsdump(fd,"coff_original_frontwindow",0x1004c260L,4);
    if(lseek(fd,(off_t)0xcb2,0)!=-1 && read(fd,(char *)b,1)==1)
        t_info("coff_mmu32bit","%u",(unsigned)b[0]);
    for(i=0;i<6;i++) {
        if(lseek(fd,(off_t)(0xe00L+4*(traps[i]&0x3ff)),0)==-1 || read(fd,(char *)b,4)!=4)continue;
        target=coffword(b);
        for(k=0;k<3;k++) {
            if(!((target>=0x10000L && cofframrange(fd,target,32)) ||
                (target>=0x40800000L && target<=0x40900000L-32)))break;
            if(lseek(fd,(off_t)target,0)==-1 || read(fd,(char *)b,32)!=32)break;
            for(j=0;j<32;j++)sprintf(hex+2*j,"%02x",b[j]);
            t_info("coff_qd_entry","trap %x depth %d target %lx %s",traps[i],k,target,hex);
            if(b[0]==0x4e && b[1]==0xf9)target=coffword(b+2);
            else if(traps[i]==0xa882 && !memcmp(b,"\040\125\040\120\014\150\000\061\000\110\147\004\116\372",14))
                target+=14+(short)(((unsigned)b[14]<<8)|b[15]);
            else break;
        }
    }
    if(lseek(fd,(off_t)0x1008L,0)!=-1 && read(fd,(char *)b,4)==4) {
        target=coffword(b);
        if(target>=0x10000L && cofframrange(fd,target,16) && lseek(fd,(off_t)target,0)!=-1 &&
            read(fd,(char *)b,16)==16 &&
            !memcmp(b,"\040\125\040\120\014\150\000\061\000\110\147\004\116\372",14)) {
            target+=14+(short)(((unsigned)b[14]<<8)|b[15]);
            if(target>=0x10000L && cofframrange(fd,target,12) && lseek(fd,(off_t)target,0)!=-1 &&
                read(fd,(char *)b,12)==12 && !memcmp(b,"\116\126\377\374\110\347\007\030\074\056\000\024",12) &&
                lseek(fd,(off_t)0x1aa0L,0)!=-1 && read(fd,(char *)b,4)==4) {
                target=coffword(b);
                for(i=0;i<64;i+=32) {
                    if(!((target>=0x10000L && cofframrange(fd,target,64)) ||
                        (target>=0x40800000L && target<=0x40900000L-64)) ||
                        lseek(fd,(off_t)(target+i),0)==-1 || read(fd,(char *)b,32)!=32)break;
                    for(j=0;j<32;j++)sprintf(hex+2*j,"%02x",b[j]);
                    t_info("coff_qd_dispatch","vector 1aa0 target %lx +%d %s",target,i,hex);
                }
            }
        }
    }
}

static int
coffparentrange(location,size)
long location,size;
{
    char path[48];int fd,ok;
    sprintf(path,"/proc/%d",(int)macpid);
    if((fd=open(path,O_RDONLY))<0)return 0;
    ok=location>0 && size>0 && cofframrange(fd,(unsigned long)location,(unsigned long)size);
    close(fd);return ok;
}

static void
coffparentstack(char *buf)
{
    char *p;long base=0,size=0,sp=0,location=0,native=0,cleanup=-99;
    p=strstr(buf,"hooks_cleanup ");if(p)sscanf(p,"hooks_cleanup %ld",&cleanup);
    t_check("coff_public_hooks_cleanup",cleanup>=0 &&
        (getsym("uinter_coffnative47")!=1 || cleanup==1),"%ld",cleanup);
    p=strstr(buf,"private_stack_base ");if(p)sscanf(p,"private_stack_base %ld",&base);
    p=strstr(buf,"private_stack_size ");if(p)sscanf(p,"private_stack_size %ld",&size);
    p=strstr(buf,"fork_parent_sp ");if(p)sscanf(p,"fork_parent_sp %ld",&sp);
    p=strstr(buf,"native_location ");if(p)sscanf(p,"native_location %ld",&location);
    p=strstr(buf,"native_size ");if(p)sscanf(p,"native_size %ld",&native);
    t_check("coff_parent_private_stack",base==0x70000000L && size==65536L &&
        sp>=base && sp-base<size && location>0 && native>0 &&
        coffparentrange(location,native) &&
        !(sp>=location && sp-location<native),
        "base %lx size %ld SP %lx native %lx/%ld",base,size,sp,location,native);
}

static void
coffsnapshot(pid)
int pid;
{
    char path[48],hex[65],buf[4097];
    unsigned char memory[32];
    prpsinfo_t ps;prstatus_t st;
    unsigned long at=0,info;
    long count,record[8],address;
    int fd,i,j,n,offset;
    t_info("coff_stackspace","calls %ld forward %ld allocations %ld refused %ld",
        getsym("uin_coffspacecalls"),getsym("uin_coffspaceforward"),
        getsym("uin_coffspacealloc"),getsym("uin_coffspacerefused"));
    sprintf(path,"/proc/%d",pid);
    if(pid>1 && (fd=open(path,O_RDONLY))>=0) {
        if(ioctl(fd,PIOCPSINFO,&ps)==0 && ps.pr_pid==pid &&
            !strcmp(ps.pr_fname,"CommandShell") && !ps.pr_zomb && ps.pr_ppid==macpid &&
            ioctl(fd,PIOCSTATUS,&st)==0) {
            t_info("coff_child_state","parent %d PC %lx SP %lx A5 %lx instr %lx",
                (int)ps.pr_ppid,(long)st.pr_reg[16],(long)st.pr_reg[15],
                (long)st.pr_reg[13],st.pr_instr);
            cofffsclose(fd,(unsigned long)st.pr_reg[15]);
            t_info("coff_child_a6","%lx",(long)st.pr_reg[14]);
            coffqdtargets(fd);
            t_info("coff_child_io","D0 %lx D1 %lx A0 %lx A1 %lx",
                (long)st.pr_reg[0],(long)st.pr_reg[1],(long)st.pr_reg[8],(long)st.pr_reg[9]);
            address=(long)st.pr_reg[8];
            if(address>=0x10000L && cofframrange(fd,address,64))
                for(offset=0;offset<64;offset+=32) {
                    if(lseek(fd,(off_t)(address+offset),0)==-1 ||
                        read(fd,(char *)memory,32)!=32)break;
                    for(j=0;j<32;j++)sprintf(hex+2*j,"%02x",memory[j]);
                    t_info("coff_io_pb","%lx +%d %s",address,offset,hex);
                }
            for(i=0;i<3;i++) {
                address=i==2?0x3e0L:i?0x390L:0x350L;
                if(lseek(fd,(off_t)address,0)!=-1 && read(fd,(char *)memory,32)==32) {
                    for(j=0;j<32;j++)sprintf(hex+2*j,"%02x",memory[j]);
                    t_info("coff_fs_lowmem","%lx %s",address,hex);
                }
            }
            for(i=0;i<2;i++) {
                address=(long)st.pr_reg[i?15:16];
                if(lseek(fd,(off_t)address,0)!=-1 &&
                    (n=read(fd,(char *)memory,i?32:16))>0) {
                    for(j=0;j<n;j++)sprintf(hex+2*j,"%02x",memory[j]);
                    t_info("coff_child_memory","%lx %s",address,hex);
                }
            }
            address=0x904L;
            if(lseek(fd,(off_t)address,0)!=-1 && read(fd,(char *)memory,4)==4) {
                long a5=((long)memory[0]<<24)|((long)memory[1]<<16)|((long)memory[2]<<8)|memory[3];
                t_info("coff_qd_current_a5","%lx",a5);
                for(i=0;i<2;i++) {
                    address=i?(long)st.pr_reg[13]:a5;
                    if(address<4 || !cofframrange(fd,address-4,8))continue;
                    coffqdchain(fd,address);
                    if(lseek(fd,(off_t)(address-4),0)!=-1 && read(fd,(char *)memory,4)==4) {
                        long port=((long)memory[0]<<24)|((long)memory[1]<<16)|((long)memory[2]<<8)|memory[3];
                        t_info("coff_qd_port","A5 %lx port %lx",address,port);
                        if(port>=0x10000L && cofframrange(fd,port,108))
                            for(offset=0;offset<108;offset+=32) {
                                n=108-offset<32?108-offset:32;
                                if(lseek(fd,(off_t)(port+offset),0)==-1 ||
                                    read(fd,(char *)memory,n)!=n)break;
                                for(j=0;j<n;j++)sprintf(hex+2*j,"%02x",memory[j]);
                                t_info("coff_qd_port_bytes","%lx +%d %s",port,offset,hex);
                            }
                    }
                }
            }
            at=0;
            if(getksym("uin_coffqdfirst",&at,&info)==0) {
                int km=open("/dev/kmem",O_RDONLY);
                long port=0;
                if(km>=0) {
                    if(lseek(km,(off_t)(at+12),0)!=-1)
                        read(km,(char *)&port,4);
                    close(km);
                }
                if(port>=0x10000L && cofframrange(fd,port,108))
                    for(offset=0;offset<108;offset+=32) {
                        n=108-offset<32?108-offset:32;
                        if(lseek(fd,(off_t)(port+offset),0)==-1 ||
                            read(fd,(char *)memory,n)!=n)break;
                        for(j=0;j<n;j++)sprintf(hex+2*j,"%02x",memory[j]);
                        t_info("coff_qd_first_port","%lx +%d %s",port,offset,hex);
                    }
            }
            for(i=0;i<4096;i+=32) {
                address=(long)st.pr_reg[15]+i;
                if(lseek(fd,(off_t)address,0)==-1 || read(fd,(char *)memory,32)!=32)break;
                for(j=0;j<32;j++)sprintf(hex+2*j,"%02x",memory[j]);
                t_info("coff_child_stack","%lx %s",address,hex);
            }
        }
        close(fd);
    }
    t_info("coff_file_counts","reads %ld seeks %ld errors %ld captured %ld",
        getsym("aux_filecalls"),getsym("aux_filecalls")<0?-1:getsym("aux_filecount")-getsym("aux_filecalls"),getsym("aux_filebad"),getsym("aux_filefailedcount"));
    at=0;count=getsym("aux_filefailedcount");
    if(count>0 && count<=16 && getksym("aux_filefailed",&at,&info)==0 &&
        (fd=open("/dev/kmem",O_RDONLY))>=0) {
        for(i=0;i<count;i++)if(lseek(fd,(off_t)(at+i*sizeof record),0)!=-1 &&
            read(fd,(char *)record,sizeof record)==sizeof record)
            t_info("coff_file_error","PID %ld call %ld fd %ld errno %ld value %ld file %lx vnode %lx offset %lx",
                record[0],record[1],record[2],record[3],record[4],record[5],record[6],record[7]);
        close(fd);
    }
    at=0;count=getsym("aux_openfailedcount");
    if(count>0 && count<=16 && getksym("aux_openfailed",&at,&info)==0 &&
        (fd=open("/dev/kmem",O_RDONLY))>=0) {
        long opened[23];
        for(i=0;i<count;i++)if(lseek(fd,(off_t)(at+i*sizeof opened),0)!=-1 &&
            read(fd,(char *)opened,sizeof opened)==sizeof opened)
            t_info("coff_open_error","PID %ld errno %ld flags %lx mode %lx address %lx PC %lx copied %ld path %.64s",
                opened[0],opened[1],opened[2],opened[3],opened[4],opened[5],opened[6],(char *)(opened+7));
        close(fd);
    }
    if((fd=open("/dev/kmem",O_RDONLY))>=0) {
        char *symbols[12],*counts[12];int k,words;
        symbols[0]="uin_coffdraw";symbols[1]="uin_coffset";symbols[2]="uin_coffnatdraw";
        symbols[3]="uin_cofftooltext";symbols[4]="uin_cofftoolbits";symbols[5]="uin_cofffont";symbols[6]="uin_cofffontpost";symbols[7]="uin_coffpixels";symbols[8]="uin_coffvis";symbols[9]="uin_coffwin";symbols[10]="uin_coffviswrite";symbols[11]="uin_coffconvert";
        counts[0]="uin_coffdrawcount";counts[1]="uin_coffsetcount";counts[2]="uin_coffnatdrawcount";
        counts[3]="uin_cofftooltextcount";counts[4]="uin_cofftoolbitscount";counts[5]="uin_cofffontcount";counts[6]="uin_cofffontpostcount";counts[7]="uin_coffpixelscount";counts[8]="uin_coffviscount";counts[9]="uin_coffwincount";counts[10]="uin_coffviswritecount";counts[11]="uin_coffconvertcount";
        for(k=0;k<12;k++) {
            count=getsym(counts[k]);at=0;
            t_info("coff_qd_capture","%s count %ld",symbols[k],count);
            if(count<=0 || count>8 || getksym(symbols[k],&at,&info)!=0)continue;
            words=count*(k==11?40:k==1?8:k==8?32:k==10?128:k==9?96:k>=5&&k<7?64:48);
            for(i=0;i<words;i+=8)if(lseek(fd,(off_t)(at+i*4),0)!=-1 &&
                read(fd,(char *)record,sizeof record)==sizeof record)
                t_info(symbols[k],"%d %lx %lx %lx %lx %lx %lx %lx %lx",i,
                    record[0],record[1],record[2],record[3],record[4],record[5],record[6],record[7]);
        }
        close(fd);
    }
    t_info("coff_native_fault","count %ld PC %lx address %lx",
        getsym("guest_nfault7"),getsym("guest_f7pc"),getsym("guest_f7ea"));
    t_info("coff_alert_count","%ld",getsym("uin_coffalertcount"));
    t_info("coff_console_state","bind %ld out %ld in %ld read %ld notify %ld detach %ld",
        getsym("aux_conbind"),getsym("aux_conout"),getsym("aux_conin"),
        getsym("aux_conread"),getsym("aux_connotify"),getsym("aux_condetach"));
    t_info("coff_console_diag","open %ld openerr %ld stage %ld error %ld ioc %ld cmd %lx len %ld reply %ld",
        getsym("aux_conopen"),getsym("aux_conopenerr"),getsym("aux_constage"),getsym("aux_conerror"),
        getsym("aux_conioc"),getsym("aux_concmd"),getsym("aux_conlen"),getsym("aux_conreply"));
    if((fd=open("/dev/kmem",O_RDONLY))>=0) {
        char *symbols[5];int k,words;
        symbols[0]="uin_coffstartup";symbols[1]="uin_coffalert";symbols[2]="uin_coffqdfirst";symbols[3]="uin_coff47";symbols[4]="uin_coffsyserr";
        for(k=0;k<5;k++) {at=0;if(getksym(symbols[k],&at,&info)==0) {
            words=k==4?320:k==3?16:k==2?8:k?136:24;
            for(i=0;i<words;i+=8)if(lseek(fd,(off_t)(at+i*4),0)!=-1 &&
                read(fd,(char *)record,sizeof record)==sizeof record)
                t_info(symbols[k],"%d %lx %lx %lx %lx %lx %lx %lx %lx",i,
                    record[0],record[1],record[2],record[3],record[4],record[5],record[6],record[7]);
        }
        }
        close(fd);
    }
    at=0;
    count=getsym("uin_cofftracecount");
    if(count>0 && getksym("uin_cofftrace",&at,&info)==0 &&
        (fd=open("/dev/kmem",O_RDONLY))>=0) {
        for(i=count>64?count-64:0;i<count;i++) {
            if(lseek(fd,(off_t)(at+(i&63)*sizeof record),0)!=-1 &&
                read(fd,(char *)record,sizeof record)==sizeof record)
                t_info("coff_native_trap","%lx %lx %lx %lx %lx %lx %lx %lx",
                    record[0],record[1],record[2],record[3],record[4],record[5],record[6],record[7]);
        }
        close(fd);
    }
    if((fd=open("/tmp/coffchild.out",O_RDONLY))>=0) {
        n=read(fd,buf,sizeof buf-1);close(fd);
        if(n>0) {buf[n]=0;for(i=0;i<n;i+=128)
            t_info("coff_child_log","%.*s",n-i<128?n-i:128,buf+i);}
    }
}

#ifndef SYS76
/* a ROM for UI_ROM: the image file, or the host's at ROMBase */
static int
haverom()
{
	struct stat sb;
	long v;

	if (stat("/etc/aux/rom", &sb) == 0)
		return 1;
	return getsym("mac_rombase") == 0x40800000L &&
	    kmemrw(0x52800008L, &v, 0) == 0 && (v >> 16 & 0xffff) >= 0x67c;
}
#endif

/* the kernel trace ring (aux_tbuf), oldest first, into klog */
static long klen;

static int
readtrace()
{
	unsigned long a = 0, info;
	long pos, n, from;
	int fd;

	if (getksym("aux_tbuf", &a, &info) < 0 || (pos = getsym("aux_tpos")) < 0 ||
	    (fd = open("/dev/kmem", O_RDONLY)) < 0)
		return -1;
	from = pos > TBUF ? pos % TBUF : 0;
	n = pos > TBUF ? TBUF - from : pos;
	klen = 0;
	if (lseek(fd, (off_t)(a + from), 0) != -1 && read(fd, klog, n) == n)
		klen = n;
	if (pos > TBUF && lseek(fd, (off_t)a, 0) != -1 && read(fd, klog + klen, from) == from)
		klen += from;
	close(fd);
	klog[klen] = 0;
	return klen > 0 ? 0 : -1;
}

static void
native()
{
	struct modstatus st;
	int fd, id = 1, refs = -1, major = -1;

	fd = open("/dev/uinter0", O_RDWR);
	if (!t_check("native_open", fd >= 0, "%s", T_ERR))
		return;
	t_check("native_version", ioctl(fd, 0x20005100, 0) == 5, "%s", T_ERR);
	t_check("native_refused", ioctl(fd, 0x20005115, 0) == -1 && errno == EINVAL,
	    "CREATELAYER from a native process: %s", T_ERR);
	while (modstat(id, &st, 1) == 0) {
		if (strcmp(st.ms_name, "uinter") == 0) {
			refs = st.ms_refcnt;
			if (st.ms_msinfo[0].mss_type == MOD_TY_CDEV)
				major = st.ms_msinfo[0].mss_p1[0];
		}
		id = st.ms_id + 1;
	}
	t_check("module", refs == 1 && major == UIMAJ,
	    "uinter refs %d, major %d", refs, major);
	close(fd);
}

/* out's "P name", "F name: why", "I name value" lines as results; 1 if it says done */
static int
results(pfx)
	char *pfx;
{
	char sub[100], *l, *e, *why;
	int done = 0;

	for (l = out; *l; l = *e ? e + 1 : e) {
		for (e = l; *e && *e != '\n'; e++)
			;
		if (e - l == 4 && strncmp(l, "done", 4) == 0)
			done = 1;
		if (e - l < 3 || e - l >= sizeof sub - 12 || l[1] != ' ' || l[0] == 'K')
			continue;
		sprintf(sub, "%s%.*s", pfx, (int)(e - l - 2), l + 2);
		why = strchr(sub, ':');
		if (l[0] == 'I') {
			if ((why = strchr(sub, ' ')) != 0)
				*why++ = 0;
			t_info(sub, "%s", why ? why : "");
			continue;
		}
		if (why) {
			*why++ = 0;
			while (*why == ' ')
				why++;
		} else
			why = "";
		if (l[0] == 'P')
			t_pass(sub);
		else
			t_fail(sub, "%s", why);
	}
	return done;
}

/* macjoin m, output to fd; vm: Mac memory past the default VM limit */
static pid_t
joinrun(m, fd, vm)
	char *m;
	int fd, vm;
{
	static char *av[] = { "/aux/bin/macjoin", 0, 0 };
	struct rlimit rl;
	pid_t pid;

	av[1] = m;
	if ((pid = fork()) == 0) {
		if (vm && getrlimit(RLIMIT_VMEM, &rl) == 0) {
			rl.rlim_cur = rl.rlim_max;
			setrlimit(RLIMIT_VMEM, &rl);
		}
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
		}
		execve(av[0], av, macenv);
		_exit(127);
	}
	return pid;
}

/*
 * The joiner starts first, from outside the session, with the default
 * VM limit; the session (macjoin c) a second later.  When the creator
 * says K pid, it is killed: that joiner must go with it and the layer
 * be free.
 */
static void
macjoin()
{
	int p[2], st, jst, n, got = 0, i;
	long t0 = t_now_ms(), w = 0;
	struct pollfd pf;
	pid_t jp, cp;
	char *k;

	t_rearm(90);
	if (pipe(p) < 0)
		return;
	jp = joinrun("j", p[1], 0);
	sleep(2);
	cp = joinrun("c", p[1], 1);
	close(p[1]);
	pf.fd = p[0];
	pf.events = POLLIN;
	while (got < sizeof out - 1 && t_now_ms() - t0 < 60000L) {
		out[got] = 0;
		if ((k = strstr(out, "\nK ")) != 0 && strchr(k + 1, '\n')) {
			w = atol(k + 3);
			break;
		}
		if (poll(&pf, 1L, 500) <= 0)
			continue;
		if ((n = read(p[0], out + got, sizeof out - 1 - got)) <= 0)
			break;
		got += n;
	}
	out[got] = 0;
	if (cp > 0)
		kill(cp, SIGKILL);
	if (cp < 0 || t_waitchild(cp, &st, 10) < 0)
		st = -1;
	if (jp > 0 && t_waitchild(jp, &jst, 10) < 0) {
		kill(jp, SIGKILL);
		jst = -1;
	}
	close(p[0]);
	t_check("macjoin", results("macjoin_") && w > 0, "creator status %#x, K %ld", st, w);
	t_check("macjoin_joiner_status", jp > 0 && jst == 0, "status %#x", jst);
	for (i = 0; w > 0 && i < 50 && (kill((pid_t)w, 0) == 0 || errno != ESRCH); i++)
		poll(0, 0L, 100);
	t_check("macjoin_joiner_goes_with_session", w > 0 && i < 50, "pid %ld still there", w);
	n = joinrun("t", -1, 0);
	t_check("macjoin_layer_free", n > 0 && t_waitchild(n, &st, 10) == n && st == 0,
	    "status %#x", st);
}

/* the macabi lines as results */
static void
macabi()
{
	static char *av[] = { "/aux/bin/macabi", 0, 0 };
	char pram[256], hw[16];
	unsigned long a = 0, info;
	int p[2], st, n, got = 0, done = 0, fd;
	struct pollfd pf;
	long t0 = t_now_ms();
	pid_t pid;

	t_rearm(90);
	/* where uinter keeps the host ROM's address, for macabi to compare */
	if (getksym("ui_hwrom", &a, &info) == 0) {
		sprintf(hw, "%lx", a);
		av[1] = hw;
	}
	/* a PRAM file for the session to load: marked, 24-bit addressing asked */
	memset(pram, 0, sizeof pram);
	memcpy(pram + 0xfc, "PRAM", 4);
	if ((fd = open(PRAMF, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
		write(fd, pram, sizeof pram);
		close(fd);
	}
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		execve(av[0], av, macenv);
		_exit(127);
	}
	close(p[1]);
	pf.fd = p[0];
	pf.events = POLLIN;
	/* a hung macabi still reports how far it got */
	while (pid > 0 && got < sizeof out - 1 && t_now_ms() - t0 < 60000L) {
		if (poll(&pf, 1L, 500) <= 0)
			continue;
		if ((n = read(p[0], out + got, sizeof out - 1 - got)) <= 0)
			break;
		got += n;
	}
	if (pid > 0 && t_now_ms() - t0 >= 60000L)
		kill(pid, SIGKILL);
	out[got] = 0;
	close(p[0]);
	if (pid < 0 || t_waitchild(pid, &st, 60) < 0)
		st = -1;
	done = results("macabi_");
	t_check("macabi", st == 0 && done, "status %#x, done %d", st, done);
	/* written back as the session ended; startmac then starts from its own */
	n = -1;
	if ((fd = open(PRAMF, O_RDONLY)) >= 0) {
		n = read(fd, pram, sizeof pram);
		close(fd);
	}
	t_check("pram_file", n == 256 && memcmp(pram + 0xfc, "Mac!", 4) == 0,
	    "read %d", n);
	unlink(PRAMF);
}

/* startmac's output to the console, one "mac| " line each */
static void
relay(fd, secs)
int fd, secs;
{
	char b[512], l[600];
	struct pollfd pf;
	int n, k = 0, i;
	long t0 = t_now_ms();

	pf.fd = fd;
	pf.events = POLLIN;
	while (t_now_ms() - t0 < secs * 1000L) {
		pf.revents = 0;
		if (macid == 0 && (macid = front()) > 0) {
			mact = t_now_ms();
		}
		else if (macid < 0)
			macid = 0;
		if (macid && t_now_ms() - mact > SETTLE * 1000L)
			break;
		if (poll(&pf, 1L, 100) <= 0)
			continue;
		if ((n = read(fd, b, sizeof b)) <= 0)
			break;
		for (i = 0; i < n; i++) {
			if (b[i] == '\n' || k == sizeof l - 8) {
				l[k] = 0;
				/* the run stops on this word; keep it out */
				if (strstr(l, "SysZone"))
					zones |= 1;
				if (strstr(l, "ApplZone"))
					zones |= 2;
				if (strstr(l, "No Suitable Video"))
					novideo = 1;
				if (strstr(l, "anic"))
					strcpy(l, "(line withheld)");
				printf("mac| %s\n", l);
				fflush(stdout);
				k = 0;
			}
			if (b[i] != '\n' && b[i] >= ' ' && b[i] < 0x7f)
				l[k++] = b[i];
		}
	}
}


/* startmac's output read and dropped after the relay, so its writes never block */
static pid_t
drain(fd)
int fd;
{
	char b[512];
	pid_t p;

	if ((p = fork()) == 0) {
		while (read(fd, b, sizeof b) > 0)
			;
		_exit(0);
	}
	return p;
}

static void
pause_ms(ms)
long ms;
{
	poll((struct pollfd *)0, 0L, ms);
}

#define	L32(a)	((long)lm[a] << 24 | (long)lm[(a) + 1] << 16 | lm[(a) + 2] << 8 | lm[(a) + 3])

/* the running Mac: Ticks, Time, mouse, cursor and keys from the host */
static void
input()
{
	unsigned char *lm;
	long t0, d, m0, m1, c0, e0, k0, t1;
	int id, n = -1, chk = 0;
	char *r;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!t_check("mac_lowmem", lm && lm != (unsigned char *)-1, "shmat: %s", T_ERR))
		return;
	t0 = L32(0x16a);
	pause_ms(1000L);
	d = L32(0x16a) - t0;
	t_check("mac_ticks_60hz", d >= 50 && d <= 70, "%ld ticks in a second", d);
	d = L32(0x20c) - ((long)time((time_t *)0) + 2082844800L);
	t_check("mac_time", d >= -2 && d <= 2, "Time off by %ld s", d);
	host("shot mac_cur1");
	m0 = getsym("uin_mouse");
	c0 = getsym("uin_ncur");
	host("move 40 30");
	pause_ms(300L);
	m1 = getsym("uin_mouse");
	t_check("mouse_moves", (short)m1 > (short)m0 && (m1 >> 16) > (m0 >> 16),
	    "%#lx -> %#lx", m0, m1);
	t_check("mouse_lowmem", L32(0x830) == m1, "Mouse %#lx, kernel %#lx", L32(0x830), m1);
	t_check("cursor_drawn", getsym("uin_ncur") > c0, "draws %ld", getsym("uin_ncur") - c0);
	host("shot mac_cur2");
	r = host("cmp mac_cur1 mac_cur2");
	if (r && sscanf(r, "diff %d", &n) != 1)
		n = 0;
	/* the old cursor erased, the new one drawn */
	t_check("cursor_on_screen", n > 0 && n <= 1000, "%s", r ? r : "no answer");
	host("down a");
	pause_ms(300L);
	t_check("keymap_down", lm[0x174] & 1, "KeyMap %#x", lm[0x174]);
	host("up a");
	pause_ms(300L);
	t_check("keymap_up", !(lm[0x174] & 1), "KeyMap %#x", lm[0x174]);
	e0 = getsym("uin_nev");
	k0 = getsym("uin_nkey");
	host("key b");
	for (d = 0; d < 30 && getsym("uin_nkey") == k0; d++)
		pause_ms(100L);
	t_check("key_event_posted", getsym("uin_nev") > e0, "posted %ld", getsym("uin_nev") - e0);
	t_check("key_event_taken", getsym("uin_nkey") > k0, "taken %ld", getsym("uin_nkey") - k0);
	/* a click beside the Finder's alert: taken, and the Mac stays */
	k0 = getsym("uin_nbtn");
	host("button 1");
	host("button 0");
	for (d = 0; d < 30 && getsym("uin_nbtn") - k0 < 2; d++)
		pause_ms(100L);
	t_check("click_taken", getsym("uin_nbtn") - k0 >= 2, "button events taken %ld",
	    getsym("uin_nbtn") - k0);
	pause_ms(3000L);
	t0 = L32(0x16a);
	pause_ms(500L);
	t1 = L32(0x16a);
	t_check("mac_alive", kill(macpid, 0) == 0 && t1 - t0 >= 20, "ticks %ld", t1 - t0);
	shmdt((char *)lm);
	host("shot mac_after");
	r = host("mac mac_after");
	if (r)
		sscanf(r, "checker %d", &chk);
	t_check("mac_desktop_stays", chk >= 500, "checker %d per mille", chk);
}

/* the mouse to h, v on the Mac's screen, through the host; 1 when there */
static int
moveto(h, v)
int h, v;
{
	char r[40];
	long m;
	int i, dh, dv;

	for (i = 0; i < 30; i++) {
		m = getsym("uin_mouse");
		dh = h - (short)m;
		dv = v - (short)(m >> 16);
		if (dh >= -2 && dh <= 2 && dv >= -2 && dv <= 2)
			return 1;
		/* half way each time: accelerated steps still close in */
		dh = dh / 2 ? dh / 2 : dh;
		dv = dv / 2 ? dv / 2 : dv;
		dh = dh > 40 ? 40 : dh < -40 ? -40 : dh;
		dv = dv > 40 ? 40 : dv < -40 ? -40 : dv;
		sprintf(r, "move %d %d", dh, dv);
		host(r);
		pause_ms(100L);
	}
	return 0;
}

/* until the kernel has had c button changes from the device, at most 2 s */
static void
btnwait(c)
long c;
{
	int i;

	for (i = 0; i < 400 && getsym("uin_nbtnin") < c; i++)
		pause_ms(5L);
}

/*
 * n clicks held hold ms, gap ms apart: mouseDowns the Mac took.  The
 * event queue keeps 32 events and drops the oldest, as the Mac's, so
 * the clicks go CHUNK at a time, each chunk taken before the next.
 * seen: each change waits until the kernel has it, as the mouse is
 * sampled in the host's time, which a busy host stretches past hold.
 */
#define	CHUNK	10

static long
clicks(n, hold, gap, seen)
int n, hold, gap, seen;
{
	char r[40];
	long d0 = getsym("uin_ndown"), e0, g0, b;
	int i, j, k, m;

	for (k = 0; k < n; k += m) {
		m = n - k < CHUNK ? n - k : CHUNK;
		e0 = getsym("uin_nev");
		g0 = getsym("uin_nget");
		sprintf(r, "clicks %d %d %d", m, hold, gap);
		for (j = 0; seen && j < m; j++) {
			b = getsym("uin_nbtnin");
			host("button 1 0");
			btnwait(b + 1);
			pause_ms((long)hold);
			host("button 0 0");
			btnwait(b + 2);
			pause_ms((long)gap);
		}
		if (!seen)
			host(r);
		/* until all are down or, after 1 s, all that came are taken */
		for (i = 0; i < 200 && getsym("uin_ndown") - d0 < k + m &&
		    (i < 10 || getsym("uin_nget") - g0 < getsym("uin_nev") - e0); i++)
			pause_ms(100L);
	}
	return getsym("uin_ndown") - d0;
}

/*
 * The Apple menu, held: it drops down only when MBState says the button
 * is down as the Mac takes the mouseDown.
 */
static void
menus()
{
	char *r;
	int i, ok = 0, bad = -1, diff;
	long d0 = getsym("uin_ndown");

	if (!t_check("menu_reached", moveto(20, 9), "mouse %#lx", getsym("uin_mouse")))
		return;
	for (i = 0; i < 20; i++) {
		host("button 1");
		pause_ms(i & 1 ? 100L : 500L);
		host("shot menu");
		host("button 0");
		pause_ms(i & 2 ? 1500L : 300L);
		host("shot nomenu");
		diff = 0;
		if ((r = host("cmp menu nomenu")) != 0)
			sscanf(r, "diff %d", &diff);
		if (diff > 1000)
			ok++;
		else if (bad < 0)
			bad = i;
	}
	t_check("menu_opens", ok == 20, "%d of 20, first miss %d, mouseDowns %ld", ok, bad,
	    getsym("uin_ndown") - d0);
}

/* the desktop without an alert; the Mac setting the mouse; the disk opened */
static void
desk()
{
	unsigned char *lm;
	char *r;
	long m0, m1, want, n, dbl = -1, l0, b0;
	int id, w0 = 0, w1 = 0, chk = 0;

	r = host("mac mac_after 220 120 580 230");
	if (r)
		sscanf(r, "checker %d", &chk);
	t_check("no_alert", chk >= 900, "middle of the screen: checker %d per mille", chk);
	/* a fast move goes farther than it went, as the Mac's mouse speed says */
	moveto(scrw / 4, scrh / 2);
	m0 = getsym("uin_mouse");
	host("move 60 0");
	pause_ms(300L);
	m1 = getsym("uin_mouse");
	t_info("mouse_accel", "moved 60, cursor %d", (int)((short)m1 - (short)m0));
	/* RawMouse and MTemp written with CrsrNew set: the Mac moves the mouse */
	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, 0);
	if (lm && lm != (unsigned char *)-1) {
		want = 200L << 16 | 300;
		*(long *)(lm + 0x828) = want;
		*(long *)(lm + 0x82c) = want;
		lm[0x8ce] = 1;
		pause_ms(300L);
		t_check("mouse_set_by_mac", getsym("uin_mouse") == want && L32(0x830) == want,
		    "kernel %#lx, Mouse %#lx", getsym("uin_mouse"), L32(0x830));
		dbl = L32(0x2f0);
		shmdt((char *)lm);
	}
	/* the startup disk, top right: a double click opens its window */
	r = host("mac mac_after");
	if (r)
		sscanf(r, "checker %d", &w0);
	t_check("disk_icon_reached", moveto(scrw - 40, 48), "mouse %#lx", getsym("uin_mouse"));
	l0 = getsym("uin_nlost");
	b0 = getsym("uin_nbtnin");
	n = clicks(20, 30, 100, 1);
	t_check("clicks_fast", n == 20,
	    "20 clicks held 30 ms: %ld mouseDowns, %ld button changes, %ld events lost",
	    n, getsym("uin_nbtnin") - b0, getsym("uin_nlost") - l0);
	/* the mouse reports every 20 ms: shorter clicks may not reach the kernel */
	t_info("clicks_short", "20 clicks held 8 ms: %ld mouseDowns", clicks(20, 8, 60, 0));
	menus();
	m0 = getsym("uin_nbtn");
	host("click 2");
	t_info("dblclick", "DoubleTime %ld, button events taken %ld", dbl,
	    getsym("uin_nbtn") - m0);
	pause_ms(4000L);
	host("shot mac_window");
	r = host("mac mac_window");
	if (r)
		sscanf(r, "checker %d", &w1);
	/* a window covers a good part of the desktop pattern */
	t_check("disk_window", w0 - w1 >= 100, "checker %d -> %d per mille", w0, w1);
}

/* the Mac's screen, in front: dump, console by hotkey and back */
static void
screen()
{
	char *r;
	int chk = 0, white = 0, top = 0;

	hostopen();
	if (hfd < 0) {
		t_skip("mac_screen", "no host line");
		return;
	}
	r = host("shot mac_desktop");
	t_check("mac_shot", r && strcmp(r, "error") != 0, "%s", r ? r : "no answer");
	if (r)
		sscanf(r, "%d %d", &scrw, &scrh);
	r = host("mac mac_desktop");
	if (r)
		sscanf(r, "checker %d white %d top %d", &chk, &white, &top);
	t_info("mac_screen_stats", "%s", r ? r : "none");
	/* the grey desktop: most of the screen a 50% dither */
	t_check("mac_screen", chk >= 500, "checker %d per mille", chk);
	host("key ctrl+alt+meta_l+0");
	t_check("hotkey_console", waitfront(0L, 5), "front %ld", front());
	host("shot mac_console");
	host("key ctrl+alt+meta_l+1");
	t_check("hotkey_mac", waitfront(macid, 5), "front %ld", front());
	input();
	desk();
	close(hfd);
	hfd = -1;
}

#define	FIDD	"/etc/aux/fidd"
static pid_t fiddpg;
static int fiddkill();

/* the File ID daemon, started as A/UX does before the Mac environment */
static void
fidd()
{
	struct stat sb;
	pid_t pid;
	int st = -1;
	FILE *f;

	if (stat(FIDD, &sb) < 0) {
		t_skip("fidd", "no fidd on this root");
		return;
	}
	/* A/UX's mount table, which fidd reads: the root and its device */
	if (stat("/etc/mtab", &sb) < 0 && stat("/", &sb) == 0 &&
	    (f = fopen("/etc/mtab", "w")) != 0) {
		fprintf(f, "/dev/dsk/c0d0s0 / 4.2 rw,noquota,dev=%x 1 1\n",
		    (int)(major(sb.st_dev) << 8 | minor(sb.st_dev)));
		fclose(f);
	}
#ifdef SYS76
	/* the System Folders' volume, once */
	if (stat(MACVOL "/S81", &sb) == 0 || stat(MACVOL "/S761", &sb) == 0) {
		char l[120];
		int on = 0;

		if ((f = fopen("/etc/mtab", "r")) != 0) {
			while (fgets(l, sizeof l, f))
				on |= strstr(l, " " MACVOL " ") != 0;
			fclose(f);
		}
		if (!on && (f = fopen("/etc/mtab", "a")) != 0) {
			fprintf(f, MACDEV " " MACVOL " 4.2 rw,noquota,dev=%x 1 1\n",
			    (int)(major(sb.st_dev) << 8 | minor(sb.st_dev)));
			fclose(f);
		}
	}
#endif
	fiddkill();
	if ((pid = fork()) == 0) {
		setpgrp();
		execl(FIDD, "fidd", "-d", (char *)0);
		_exit(127);
	}
	fiddpg = pid;
	/* it forks and the parent returns */
	t_waitchild(pid, &st, 10);
	t_check("fidd", st == 0, "status %#x", st);
}

/*
 * The startup disk's Desktop Folder, a folder at the root as A/UX
 * installs it.  Without it the Mac side makes a symbolic link after
 * reading the root and the Finder cannot find it.
 */
static void
deskfolder()
{
	t_check("desktop_folder", mkdir("/Desktop Folder", 0777) == 0 || errno == EEXIST,
	    "%s", T_ERR);
}

/*
 * Kill every fidd: the daemon forks into its own process group, and one
 * left over serves the next run's fidop messages.  0 once none is left.
 */
static int
fiddkill()
{
	char path[32];
	prpsinfo_t ps;
	struct dirent *de;
	DIR *d;
	int fd, i, n = 0, left = 1;

	for (i = 0; i < 50 && left; i++) {
		if (i)
			pause_ms(100L);
		left = 0;
		if ((d = opendir("/proc")) == 0)
			return -1;
		while ((de = readdir(d)) != 0) {
			if (de->d_name[0] == '.')
				continue;
			sprintf(path, "/proc/%s", de->d_name);
			if ((fd = open(path, O_RDONLY)) < 0)
				continue;
			if (ioctl(fd, PIOCPSINFO, &ps) == 0 && ps.pr_zomb == 0 &&
			    strcmp(ps.pr_fname, "fidd") == 0) {
				kill(ps.pr_pid, SIGKILL);
				left++;
				n++;
			}
			close(fd);
		}
		closedir(d);
	}
	t_info("fidd_killed", "%d", n);
	return left;
}

static void
fiddlog()
{
	char b[160];
	FILE *f;
	int n = 0;

	if (fiddpg > 0)
		kill(-fiddpg, SIGTERM);
	t_check("fidd_stopped", fiddkill() == 0, "a fidd outlived SIGKILL");
	if ((f = fopen("/tmp/fid_log", "r")) == 0)
		return;
	while (fgets(b, sizeof b, f) && n++ < 40)
		printf("fidd| %s", b);
	fclose(f);
	fflush(stdout);
}

static void
startmac()
{
	static char *av[] = { "/mac/bin/startmac", 0 };
	char pat[80], *q1, *o, *s, *z;
	int p[2], st, id, fd;
	long npriv0, npriv, nal0;
	pid_t pid, dp;

	t_rearm(240);
	if ((nal0 = getsym("uinter_nalwatch")) < 0)
		nal0 = 0;
	/* opens and commands from here on; the ring keeps the first 8 KB */
	setsym("aux_tpos", 0L);
	setsym("uinter_trace", 1L);
	t_check("deferred_trace_armed",setsym("aux_defertrace",1L)==0,"");
#ifdef SYS81
        {
            long trace=access("/etc/aux/ASLMTraceProbe",F_OK)==0 ? 1L : 0L;
            t_check("aslm_trace_configuration",setsym("uinter_aslmtrace",trace)==0 &&
                getsym("uinter_aslmtrace")==trace,"requested %ld",trace);
        }
	t_check("native_filemanager_groups",getsym("uinter_fmgroup")==0,
	    "suppression %ld",getsym("uinter_fmgroup"));
#endif
#ifdef SYS6
	setsym("aux_trace", 4L | 8L | 16L);
#else
	setsym("aux_trace", 4L | 16L);
#endif
	npriv0 = getsym("guest_npriv");
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		setpgrp();
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		for (fd = 3; fd < 20; fd++)
			close(fd);
#ifdef SYS6
		if (chroot(ROOT6) < 0 || chdir("/") < 0)
			_exit(126);
#endif
		execve(av[0], av, macenv);
		_exit(127);
	}
	close(p[1]);
	macpid = pid;
	printf("INFO mac.startmac_pid %d\n", (int)pid);
	fflush(stdout);
	relay(p[0], 90);
	dp = drain(p[0]);
#ifdef SYS6
	if (macid)
		screen6();
#else
#ifdef SYS76
	if (macid)
		screen76();
#else
	if (macid)
		screen();
#endif
#endif
#ifdef SYS6
	/* a hangup, as a closed login line sends; then the kill */
	kill(-pid, SIGHUP);
	st = 0;
	t_info("hup_status", "%d %#x", t_waitchild(pid, &st, 20), st);
#endif
	kill(-pid, SIGKILL);
	close(p[0]);
	st = 0;
	if (t_waitchild(pid, &st, 20) < 0)
		st = -1;
	if (dp > 0) {
		kill(dp, SIGKILL);
		waitpid(dp, (int *)0, 0);
	}
	setsym("aux_trace", 0L);
	setsym("uinter_trace", 0L);
	npriv = getsym("guest_npriv");
	t_info("startmac_status", "%#x", st);
	if (novideo)
		t_skip("past_dovideo", "no video device (uinter without screens)");
	else {
		t_check("past_dovideo", zones == 3, "no Memory Manager report");
		t_check("mac_front", macid > 0, "the Mac's session never came to front");
	}
	if (macid > 0)
		t_check("console_after_exit", waitfront(0L, 5), "front %ld", front());
	t_info("privileged_emulated", "%ld", npriv - npriv0);
	t_info("aline_watched", "%ld", getsym("uinter_nalwatch") - nal0);
	/* killed: Mac RAM goes with it, or it blocks the next startmac */
	t_check("mac_ram_freed", shmget(TLOW, 0, 0) < 0, "'tLOW' left after SIGKILL");
	if ((id = shmget(TLOW, 0, 0)) >= 0)
		shmctl(id, IPC_RMID, (struct shmid_ds *)0);
	if (!t_check("trace", readtrace() == 0, "no kernel trace ring"))
		return;
	/* the trace into the log: the Mac task's opens and uinter commands */
	for (s = klog; *s; s = z) {
		for (z = s; *z && *z != '\n'; z++)
			;
		if (atoi(s) == pid)
			printf("ktrace| %.*s\n", (int)(z - s), s);
		if (*z)
			z++;
	}
	fflush(stdout);
	sprintf(pat, "%d Q01 0\n", (int)pid);
	q1 = strstr(klog, pat);
	t_check("ui_set", q1 != 0, "no UI_SET by pid %d in the trace", (int)pid);
	t_check("past_dodispatch", zones == 3, "no Memory Manager report (SysZone, ApplZone)");
	o = 0;
	sprintf(pat, "%d open ", (int)pid);
	for (s = q1 ? q1 : klog + klen; (s = strstr(s, pat)) != 0; s++) {
		z = strchr(s + strlen(pat), ' ');
		if (z && z - 6 >= s && strncmp(z - 6, "System", 6) == 0 &&
		    (z[-7] == '/' || (z[-7] == '%' && z[-8] == '/')) && atoi(z + 1) >= 0) {
			o = s;
			break;
		}
	}
	t_check("system_opened", o != 0, "no open of the System file after UI_SET");
	if (o)
		t_info("system_open", "%.*s", (int)(strchr(o, '\n') - o), o);
}

/* pid holding a write lock on path, 0 if none, -1 on error */
static long
holder(path)
	char *path;
{
	struct flock fl;
	int fd = open(path, O_RDWR);

	if (fd < 0)
		return -1;
	memset((char *)&fl, 0, sizeof fl);
	fl.l_type = F_WRLCK;
	if (fcntl(fd, F_GETLK, &fl) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return fl.l_type == F_UNLCK ? 0 : (long)fl.l_pid;
}

#define	ENVSH	"/tests/startmac.sh"	/* the startmac script, with -e */
#define	ENVF	"/tmp/Mac/r1/.env"
#define	ENVSTAMP "/mac/lib/System Folder/.stamp"

/* environment r1 for the script: its stamp current, the System in TBSYSTEM */
static void
envmake()
{
	system("/usr/bin/mkdir -p '/mac/lib/System Folder' '/tmp/Mac/r1/System Folder'");
	system("[ -f '" ENVSTAMP "' ] || echo test > '" ENVSTAMP "'");
	system("/usr/bin/cp '" ENVSTAMP "' '/tmp/Mac/r1/System Folder/.stamp'");
	system("echo > '/tmp/Mac/r1/System Folder/System'");
}

/* startmac -e r1 to completion; its exit status */
static int
envsecond()
{
	static char *av[] = { "/sbin/sh", ENVSH, "-e", "r1", 0 };
	int fd, st;
	pid_t p;

	if ((p = fork()) == 0) {
		fd = open("/tmp/envmac.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
		dup2(fd, 1);
		dup2(fd, 2);
		execve(av[0], av, macenv);
		_exit(127);
	}
	if (p < 0 || t_waitchild(p, &st, 30) < 0 || !WIFEXITED(st))
		return -1;
	return WEXITSTATUS(st);
}

/*
 * PRAM kept for the next session; startmac again, as another user
 * (group display) after the killed one: the desktop, no alert.
 */
static void
restart()
{
	static char *av[] = { "/mac/bin/startmac", 0 };
	struct stat sb;
	char *r;
	int p[2], st, fd, chk = 0;
	pid_t pid;

	t_check("pram_saved", stat("/etc/aux/pram", &sb) == 0 && sb.st_size == 256,
	    "/etc/aux/pram: %s", T_ERR);
	t_rearm(240);
	/* the user's System Folder and console, as after a login */
	system("/usr/bin/chown -R " OTHERUID " " SYSDIR " /dev/console");
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		setpgid(0, 0);	/* its own group, the console still its terminal */
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		for (fd = 3; fd < 20; fd++)
			close(fd);
		if (setgid(25) == 0 && setuid(atoi(OTHERUID)) == 0)
			execve(av[0], av, macenv);
		_exit(127);
	}
	close(p[1]);
	macid = 0;
	relay(p[0], 90);
	t_check("restart_front", macid > 0, "the Mac's session never came to front");
	if (macid > 0) {
		hostopen();
		r = host("shot mac_restart");
		r = r ? host("mac mac_restart 220 120 580 230") : 0;
		if (r)
			sscanf(r, "checker %d", &chk);
		t_check("restart_no_alert", chk >= 900, "middle of the screen: checker %d per mille",
		    chk);
		if (hfd >= 0)
			close(hfd);
		hfd = -1;
	}
	/* quit: a hangup, as a closed login line sends */
	kill(-pid, SIGHUP);
	st = 0;
	if (t_waitchild(pid, &st, 20) < 0)
		st = -1;
	t_info("restart_quit_status", "%#x", st);
	close(p[0]);
	system("/usr/bin/chown -R 0 " SYSDIR " /dev/console");
	t_check("restart_console", waitfront(0L, 5), "front %ld", front());
}

/* the startmac script with -e: the session holds the lock, a second is refused */
static void
envrun()
{
	static char *av[] = { "/sbin/sh", ENVSH, "-e", "r1", 0 };
	char b[256];
	int p[2], st, fd, n;
	pid_t pid;

	t_rearm(240);
	envmake();
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		setpgrp();
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		for (fd = 3; fd < 20; fd++)
			close(fd);
		execve(av[0], av, macenv);
		_exit(127);
	}
	close(p[1]);
	macid = 0;
	relay(p[0], 90);
	t_check("env_front", macid > 0, "the Mac's session never came to front");
	t_check("env_held", holder(ENVF) > 0, "holder %ld", holder(ENVF));
	st = envsecond();
	b[0] = 0;
	if ((fd = open("/tmp/envmac.log", O_RDONLY)) >= 0) {
		n = read(fd, b, sizeof b - 1);
		b[n > 0 ? n : 0] = 0;
		close(fd);
	}
	t_check("env_second", st == 1 && strstr(b, "in use") != 0, "status %d: %s", st, b);
	kill(-pid, SIGHUP);
	if (t_waitchild(pid, &st, 20) < 0)
		st = -1;
	close(p[0]);
	for (n = 0; n < 50 && holder(ENVF) > 0; n++)
		pause_ms(200L);
	t_check("env_released", holder(ENVF) == 0, "holder %ld", holder(ENVF));
	t_check("env_console", waitfront(0L, 5), "front %ld", front());
	if ((n = shmget(TLOW, 0, 0)) >= 0)
		shmctl(n, IPC_RMID, (struct shmid_ds *)0);
	system("/usr/bin/rm -rf /tmp/Mac");
}

#ifdef SYS76
/* the model Patch.067C keeps for Gestalt 'mach' (its own + 6), read from the task */
static long
patchbox()
{
	char path[32];
	long v = -1;
	int fd;

	sprintf(path, "/proc/%05d", (int)macpid);
	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	if (lseek(fd, 0x87cdcL, 0) == -1 || read(fd, (char *)&v, 4) != 4)
		v = -1;
	close(fd);
	return v;
}
#endif

#if defined(SYS76) || defined(SYS6)
/* pixels changed between two dumps, -1 without an answer */
static int
shotdiff(a, b)
char *a, *b;
{
	char req[128], *r;
	int n = -1;
	unsigned long na = strlen(a), nb = strlen(b);

	if (na > sizeof req - 6 || nb > sizeof req - 6 - na) return -1;
	sprintf(req, "cmp %s %s", a, b);
	if ((r = host(req)) != 0 && sscanf(r, "diff %d", &n) != 1)
		n = strcmp(r, "same") == 0 ? 0 : -1;
	return n;
}

/* the Mac still running: its ticks advance */
static int
alive76()
{
	unsigned char *lm;
	long t0, t1;
	int id;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!lm || lm == (unsigned char *)-1)
		return 0;
	t0 = L32(0x16a);
	pause_ms(500L);
	t1 = L32(0x16a);
	shmdt((char *)lm);
	return kill(macpid, 0) == 0 && t1 - t0 >= 20;
}

/*
 * A dump name taken until it differs from ref by lo to hi pixels or secs
 * pass, then again once the screen has settled: the pixels changed.
 */
static int
waitshot(name, ref, lo, hi, secs)
char *name, *ref;
int lo, hi, secs;
{
	char req[40];
	int i, n;

	sprintf(req, "shot %s", name);
	for (i = 0; i < secs; i += 2) {
		host(req);
		n = shotdiff(ref, name);
		if (n >= lo && n <= hi)
			break;
		pause_ms(2000L);
	}
	pause_ms(2000L);
	host(req);
	return shotdiff(ref, name);
}

/* a menu item chosen: the menu held open at (h, v), released over (ih, iv) */
static void
menuitem(h, v, ih, iv, shot)
int h, v, ih, iv;
char *shot;
{
	char req[40];

	/* a halt recorded, the session killed in its place */
	setsym("uinter_adcall", 0L);
	setsym("uinter_adtest", 1L);
	moveto(h, v);
	host("button 1");
	pause_ms(800L);
	moveto(ih, iv);
	pause_ms(500L);
	sprintf(req, "shot %s", shot);
	host(req);
	host("button 0");
}

#ifdef SYS81
static long
mac_cpu()
{
 char path[40];
 prpsinfo_t ps;
 prstatus_t st;
 struct dirent *de;
 DIR *d;
 long ms = 0,ut,sy;
 char name[17];
 int fd, found = 0;

 if ((d = opendir("/proc")) == 0) return -1;
 while ((de = readdir(d)) != 0) {
  if (de->d_name[0] == '.') continue;
  sprintf(path, "/proc/%s", de->d_name);
  if ((fd = open(path, O_RDONLY)) < 0) continue;
  if (ioctl(fd, PIOCPSINFO, &ps) == 0 && !ps.pr_zomb &&
      (ps.pr_pid == macpid || ps.pr_ppid == macpid)) {
   if (found>=32) {close(fd);closedir(d);return -1;}
   if (ioctl(fd, PIOCSTATUS, &st) != 0) { close(fd); closedir(d); return -1; }
   ut=st.pr_utime.tv_sec*1000L+st.pr_utime.tv_nsec/1000000;
   sy=st.pr_stime.tv_sec*1000L+st.pr_stime.tv_nsec/1000000;
   memcpy(name,ps.pr_fname,16);name[16]=0;
   t_info("finder_idle_process","pid %ld parent %ld name %s state %c user %ld system %ld signal %d pending %lx held %lx",
       (long)ps.pr_pid,(long)ps.pr_ppid,name,ps.pr_sname,ut,sy,
       st.pr_cursig,st.pr_sigpend.sigbits[0],st.pr_sighold.sigbits[0]);
   ms += (st.pr_utime.tv_sec + st.pr_stime.tv_sec) * 1000L +
       (st.pr_utime.tv_nsec + st.pr_stime.tv_nsec) / 1000000;
   found++;
  }
  close(fd);
 }
 closedir(d);
 return found ? ms : -1;
}
#endif

#ifdef SYS81
static void
logoutstate()
{
 char path[40], hex[129];
 unsigned char b[64];
 prpsinfo_t ps;
 prstatus_t st;
 struct dirent *de;
 DIR *d;
 int fd, i, n, j;
 long a;

 if ((d = opendir("/proc")) == 0) return;
 while ((de = readdir(d)) != 0) {
  if (de->d_name[0] == '.') continue;
  sprintf(path, "/proc/%s", de->d_name);
  if ((fd = open(path, O_RDONLY)) < 0) continue;
  if (ioctl(fd, PIOCPSINFO, &ps) == 0 && !ps.pr_zomb &&
      (ps.pr_pid == macpid || ps.pr_ppid == macpid) &&
      ioctl(fd, PIOCSTATUS, &st) == 0) {
   t_info("logout_state", "pid %d %s %c wchan %lx pc %lx instr %lx d0 %lx sp %lx",
    (int)ps.pr_pid, ps.pr_fname, ps.pr_sname, (long)ps.pr_wchan,
    (long)st.pr_reg[16], st.pr_instr, (long)st.pr_reg[0], (long)st.pr_reg[15]);
   for (j = 0; j < 2; j++) {
    a = (long)st.pr_reg[j ? 15 : 16];
    if (lseek(fd, (off_t)a, 0) != -1 && (n = read(fd, (char *)b, sizeof b)) > 0) {
     for (i = 0; i < n; i++) sprintf(hex + 2*i, "%02x", b[i]);
     t_info("logout_memory", "%lx %s", a, hex);
    }
   }
  }
  close(fd);
 }
 closedir(d);
}
#endif

/* the session ended from the Mac: startmac exits by itself, nothing halts */
static void
loggedout(pfx)
char *pfx;
{
	char n[40];
	long t0;
	int st = 0;
	pid_t r = 0;

	t_rearm(120);
	for (t0 = t_now_ms(); t_now_ms() - t0 < 60000L; pause_ms(500L))
		if ((r = waitpid(macpid, &st, WNOHANG)) != 0)
			break;
	#ifdef SYS81
 if (r != macpid) { host("shot mac81_logout_stall"); logoutstate(); }
#endif
 sprintf(n, "%s_exits", pfx);
	t_check(n, r == macpid, "startmac still running after %ld ms", t_now_ms() - t0);
	sprintf(n, "%s_clean", pfx);
	/* doLogout's _exit(0); a fault or any other exit is not a Log Out */
	t_check(n, r == macpid && WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %#x", st);
	if (r == macpid) {
		sprintf(n, "%s_status", pfx);
		t_info(n, "%#x after %ld ms", st, t_now_ms() - t0);
		macpid = 0;
	}
	sprintf(n, "%s_no_halt", pfx);
	t_check(n, getsym("uinter_adcall") == 0, "uadmin %#lx", getsym("uinter_adcall"));
	setsym("uinter_adtest", 0L);
	sprintf(n, "%s_console", pfx);
	t_check(n, waitfront(0L, 10), "front %ld", front());
}
#endif

#ifdef SYS76

/* n bytes of Mac memory at a, as an INFO line */
static void
macdump(name, a, n)
char *name;
long a;
int n;
{
	unsigned char *lm;
	char b[200];
	int id, i;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!lm || lm == (unsigned char *)-1)
		return;
	for (i = 0; i < n && i < 64; i++)
		sprintf(b + 2 * i, "%02x", lm[a + i]);
	t_info(name, "%lx %s", a, b);
	shmdt((char *)lm);
}

#define	BE32(p)	((long)(p)[0] << 24 | (long)(p)[1] << 16 | (p)[2] << 8 | (p)[3])


/* the shutdown queue (SDHeader at $5AF40): each procedure, its flags and first bytes */
static void
sdqueue()
{
	struct shmid_ds ds;
	unsigned char *lm, *e;
	long q, pr[8];
	char b[40];
	int id, i, n;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!lm || lm == (unsigned char *)-1)
		return;
	shmctl(id, IPC_STAT, &ds);
	for (q = BE32(lm + 0x5af42), n = 0; q > 0 && q + 10 < ds.shm_segsz && n < 8;
	    q = BE32(e), n++) {
		e = lm + q;
		pr[n] = BE32(e + 6);
		sprintf(b, "sdqueue_%d_flags", n);
		t_info(b, "%d", e[4] << 8 | e[5]);
	}
	shmdt((char *)lm);
	for (i = 0; i < n; i++) {
		sprintf(b, "sdqueue_%d", i);
		macdump(b, pr[i], 32);
	}
}

/* Patch.067C's LAP Manager call at $38244 goes through its checked dispatcher */
static int
lapredirect()
{
	static unsigned char want[10] = {
		0x43, 0xfa, 0xe1, 0xea, 0x70, 0x19, 0x4e, 0xba, 0xe2, 0x0e
	};
	unsigned char *lm;
	int id, ok;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!lm || lm == (unsigned char *)-1)
		return 0;
	ok = memcmp(lm + 0x38244, want, sizeof want) == 0;
	shmdt((char *)lm);
	return ok;
}

/* ShutDwnPower's and ShutDwnStart's shutDownDialog calls: 1 both skipped, 0 both kept */
static int
sdskipped()
{
	unsigned char *lm;
	int id, n = 0;

	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (!lm || lm == (unsigned char *)-1)
		return -1;
	n += memcmp(lm + 0xd16a, "\x70\x00\x4e\x71", 4) == 0;
	n += memcmp(lm + 0xd1d2, "\x70\x00\x4e\x71", 4) == 0;
	shmdt((char *)lm);
	return n == 2 ? 1 : n == 0 ? 0 : -1;
}

/* A/UX's resources, under the name with a space Shut Down looks for */
#define	AUXRES		"/mac/lib/Resources/%AUXResources"
#define	AUXRESSP	"/mac/lib/Resources/%AUX Resources"

/* the Special menu's title, its Shut Down item and the Apple menu's Log Out */
#ifdef SYS81
#define	SPECIALH	181
#define	SHUTH		200
#define	SHUTV		109
#define	LOGOUTV		123
#else
#define	SPECIALH	232
#define	SHUTH		250
#define	SHUTV		139
#define	LOGOUTV		168
#endif

/* Special > Shut Down as root: the Mac ends by itself, with no panic */
static void
shutdown76()
{
	long t0;
	int st = 0;
	pid_t r = 0;

	t_rearm(120);
	/* Shut Down's dialog, without which it logs out */
	link(AUXRES, AUXRESSP);
	/* the halt recorded, the session killed in its place */
	setsym("uinter_adcall", 0L);
	setsym("uinter_adtest", 1L);
#ifdef SYS81
	macdump("shutdown_error", 0xaf0L, 2);
	macdump("shutdown_power_entry", 0xd14cL, 12);
	macdump("shutdown_dialog_call", 0xd166L, 12);
#endif
	moveto(SPECIALH, 9);
	host("button 1");
	pause_ms(800L);
	moveto(SHUTH, SHUTV);
	pause_ms(500L);
	host("shot " SHOT "shutdown");
	host("button 0");
	for (t0 = t_now_ms(); t_now_ms() - t0 < 60000L; pause_ms(500L))
		if ((r = waitpid(macpid, &st, WNOHANG)) != 0)
			break;
	#ifdef SYS81
 if (r != macpid) { host("shot mac81_shutdown_stall"); logoutstate(); }
#endif
 t_check("shutdown_exits", r == macpid, "startmac still running after %ld ms",
	    t_now_ms() - t0);
	if (r == macpid) {
		t_info("shutdown_status", "%#x after %ld ms", st, t_now_ms() - t0);
		macpid = 0;
	}
	/* a fault (PC $17) ends it with SIGILL before the halt */
	t_check("shutdown_uadmin", getsym("uinter_adcall") == 0x200 &&
	    WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "uadmin %#lx, status %#x",
	    getsym("uinter_adcall"), st);
	setsym("uinter_adtest", 0L);
	unlink(AUXRESSP);
}

/*
 * Root's Special > Shut Down, for real: the console's halt message must
 * follow the HALT line, with no panic between.  Only the run's last
 * stage may do this.
 */
static void
halt76()
{
	static char line[] = "HALT mac76.shutdown_halts\n";
	long t0;
	int st = 0;
	pid_t r = 0;

	t_rearm(120);
	link(AUXRES, AUXRESSP);
	setsym("uinter_adtest", 0L);
	sync();
	write(1, line, sizeof line - 1);
	moveto(232, 9);
	host("button 1");
	pause_ms(800L);
	moveto(250, 139);
	pause_ms(500L);
	host("button 0");
	for (t0 = t_now_ms(); t_now_ms() - t0 < 60000L; pause_ms(500L))
		if ((r = waitpid(macpid, &st, WNOHANG)) != 0)
			break;
	t_fail("shutdown_halts", "startmac %s, status %#x", r == macpid ? "exited" : "still running", st);
	if (r == macpid)
		macpid = 0;
	unlink(AUXRESSP);
}

/* path and everything under it owned by uid */
static void
chownr(path, uid)
char *path;
int uid;
{
	char sub[256];
	struct dirent *de;
	DIR *d;

	chown(path, uid, -1);
	if ((d = opendir(path)) == 0)
		return;
	while ((de = readdir(d)) != 0)
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..") &&
		    strlen(path) + strlen(de->d_name) + 2 < sizeof sub) {
			sprintf(sub, "%s/%s", path, de->d_name);
			chownr(sub, uid);
		}
	closedir(d);
}

/*
 * $HOME/.mac/<host> (HOME=/tmp), the Trash and Temporary Items: mode
 * 0700, which uid other cannot open.  A file moved to the Trash and
 * back keeps its mode.
 */
#define	DOTMAC	"/tmp/.mac/unix"
static void
dotmac(n, uid, other)
char *n;
int uid, other;
{
	struct stat sb;
	char m[40];
	pid_t pid;
	int st, fd, k;

	sb.st_mode = 0;
	t_check(n, stat(DOTMAC, &sb) == 0, "no " DOTMAC " from 'boot' 3's _AUXDispatch(36, 0)");
	sprintf(m, "%s_mode", n);
	t_check(m, (sb.st_mode & 07777) == 0700 && sb.st_uid == uid, "mode %o, uid %d",
	    (int)sb.st_mode & 07777, (int)sb.st_uid);
	/* other cannot open it; uid moves a file to the Trash and back */
	for (k = 0; k < 2; k++) {
		st = -1;
		if ((pid = fork()) == 0) {
			if (setuid(k ? uid : other) < 0)
				_exit(2);
			if (k == 0)
				_exit(open(DOTMAC, O_RDONLY) < 0 && errno == EACCES ? 0 : 1);
			umask(0);
			unlink("/tmp/dmfile");
			if ((fd = open("/tmp/dmfile", O_WRONLY | O_CREAT, 0640)) < 0)
				_exit(3);
			close(fd);
			mkdir(DOTMAC "/Trash", 0700);
			if (rename("/tmp/dmfile", DOTMAC "/Trash/dmfile") < 0 ||
			    rename(DOTMAC "/Trash/dmfile", "/tmp/dmfile") < 0)
				_exit(4);
			_exit(stat("/tmp/dmfile", &sb) == 0 && (sb.st_mode & 0777) == 0640 &&
			    sb.st_uid == uid && unlink("/tmp/dmfile") == 0 ? 0 : 5);
		}
		if (pid > 0)
			t_waitchild(pid, &st, 10);
		sprintf(m, k ? "%s_roundtrip" : "%s_private", n);
		t_check(m, st == 0, "status %x", st);
	}
}

/*
 * A session as uid with the System Folder sys: its desktop, for a user
 * the Special menu, then the Apple menu's Log Out ends it.
 */
static void
session76(uid, sys, pfx)
int uid;
char *sys, *pfx;
{
	static char *av[] = { "/mac/bin/startmac", 0 };
	char *env[10], tb[80], n[40], shot[40], req[60], *r;
	int p[2], fd, i, st, chk = 0, white = 0, top = 0;
    int tilex = 0, tiley = 0, dominant = 1000;
	pid_t pid, dp;

	t_rearm(240);
	for (i = 0; macenv[i] && i < 9; i++)
		env[i] = strncmp(macenv[i], "TBSYSTEM=", 9) ? macenv[i] : tb;
	env[i] = 0;
	sprintf(tb, "TBSYSTEM=%s", sys);
	/* the user's System Folder and console, as after a login */
	chownr(sys, uid);
	chown("/dev/console", uid, -1);
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		setpgid(0, 0);	/* its own group, the console still its terminal */
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		for (fd = 3; fd < 20; fd++)
			close(fd);
		if (uid == 0 || (setgid(25) == 0 && setuid(uid) == 0))
			execve(av[0], av, env);
		_exit(127);
	}
	close(p[1]);
	macpid = pid;
	macid = 0;
	relay(p[0], 90);
	dp = drain(p[0]);
	sprintf(n, "%s_front", pfx);
	t_check(n, macid > 0, "the Mac's session never came to front");
	hostopen();
	if (macid > 0 && hfd >= 0) {
		sprintf(shot, "%s%s_desktop", SHOT, pfx);
		sprintf(req, "shot %s", shot);
		for (i = 0; i < 8; i++) {
			t_rearm(120);
			host(req);
			sprintf(req, "mac %s", shot);
			tilex = tiley = 0; dominant = 1000;
			if ((r = host(req)) != 0)
				sscanf(r, "checker %d white %d top %d tilex %d tiley %d dominant %d",
                    &chk, &white, &top, &tilex, &tiley, &dominant);
			sprintf(req, "shot %s", shot);
			if (DESKOK(chk, top, tilex, tiley, dominant))
				break;
			pause_ms(10000L);
		}
		sprintf(n, "%s_desktop", pfx);
		t_check(n, DESKOK(chk, top, tilex, tiley, dominant), "checker %d, menu %d, tile %d/%d, dominant %d",
            chk, top, tilex, tiley, dominant);
#ifdef SYS81
		/* the layout repeats: the next session watches what this one lost */
		sprintf(req, "/proc/%05d", (int)pid);
		macwat = heap81(req, 0L);
		sprintf(n, "%s_heap", pfx);
		t_check(n, macwat == 0, "master pointer %lx lost", macwat);
#endif
		if (strcmp(pfx, "halt") == 0) {
			halt76();
			goto out;
		}
		if (uid) {
			/* Log Out in place of Restart and Shut Down */
			moveto(SPECIALH, 9);
			host("button 1");
			pause_ms(800L);
			sprintf(req, "shot %s%s_special", SHOT, pfx);
			host(req);
			host("button 0");
			pause_ms(1000L);
			sprintf(req, "%s%s_special", SHOT, pfx);
			sprintf(n, "%s_special_menu", pfx);
			i = shotdiff(shot, req);
			t_check(n, i > 1000, "%d pixels changed", i);
		}
		/* AtalkHk2: -1, no LAP Manager */
		macdump("atalkhk2", 0xb18L, 4);
		sdqueue();
		sprintf(n, "%s_lap_redirect", pfx);
		t_check(n, lapredirect(), "");
		/* only root's Shut Down and Restart show A/UX's dialog */
		sprintf(n, "%s_sd_dialog", pfx);
		i = sdskipped();
		t_check(n, i == (uid != 0), "skipped %d", i);
		t_rearm(120);
		/* a user leaves through Special, root through the Apple menu */
		if (uid) {
			/* with the dialog's resources Special > Log Out still logs out */
			link(AUXRES, AUXRESSP);
			sprintf(req, "%s%s_speciallogout", SHOT, pfx);
			menuitem(SPECIALH, 9, SHUTH, SHUTV - 16, req);
		} else {
			sprintf(req, "%s%s_apple", SHOT, pfx);
			menuitem(20, 9, 60, LOGOUTV, req);
		}
		loggedout(pfx);
		unlink(AUXRESSP);
	}
out:
	if (hfd >= 0)
		close(hfd);
	hfd = -1;
	if (macpid) {
		kill(-pid, SIGKILL);
		t_waitchild(pid, &st, 20);
		macpid = 0;
	}
	close(p[0]);
	if (dp > 0) {
		kill(dp, SIGKILL);
		waitpid(dp, (int *)0, 0);
	}
	setsym("uinter_adtest", 0L);
	chownr(sys, 0);
	chown("/dev/console", 0, -1);
	sprintf(n, "%s_ram_freed", pfx);
	t_check(n, shmget(TLOW, 0, 0) < 0, "'tLOW' left");
	if ((i = shmget(TLOW, 0, 0)) >= 0)
		shmctl(i, IPC_RMID, (struct shmid_ds *)0);
	macid = 0;
}

/* a file's copy */
static void
copyf(a, b)
char *a, *b;
{
	char buf[4096];
	int f, g, n;

	if ((f = open(a, O_RDONLY)) < 0)
		return;
	if ((g = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
		while ((n = read(f, buf, sizeof buf)) > 0)
			write(g, buf, n);
		close(g);
	}
	close(f);
}

/* the System Folders' volume mounted (once a boot), 0 if none */
static int
macvol()
{
	struct stat sb;

	if (stat(MACDEV, &sb) < 0)
		return 0;
	if (stat(SYSDIR, &sb) == 0)
		return 1;
	return t_check("macvol_mount", system("/sbin/mount -F ufs " MACDEV " " MACVOL) == 0,
	    "mount " MACDEV " failed");
}

#ifdef SYS81
#ifdef OT81
#include "ot81-run.h"
#endif
static unsigned long
probe81(pfx)
char *pfx;
{
	char b[8192], n[96], *q, *end;
	unsigned long hi = 0, lo = 0;
	long stop = t_now_ms() + 60000L;
	int f, k, count = 0, done = 0, finderok = 0;

	b[0] = 0;

	unlink("/tmp/mac81probe.out");
	host("key m"); host("key a"); host("key c"); host("key 8");
	host("key 1"); host("key p"); host("key r"); host("key o");
	host("key b"); host("key e"); host("key meta_l+o");
	while (t_now_ms() < stop) {
		f = open("/tmp/mac81probe.out", O_RDONLY);
		k = f < 0 ? -1 : read(f, b, sizeof b - 1);
		if (f >= 0) close(f);
		if (k > 0) {
			b[k] = 0;
			if (strstr(b, "mac81probe done")) { done = 1; break; }
		}
		pause_ms(500L);
	}
	sprintf(n, "%s_complete", pfx);
	t_check(n, done, "helper did not finish");
	for (q = b; q && *q; q = end ? end + 1 : 0) {
		end = strchr(q, '\n');
		if (!end) break;
		*end = 0;
		if (!strncmp(q, "mac81probe I ", 13))
			t_info(pfx, "%s", q + 13);
        if (!strncmp(q,"mac81probe I video_main ",24)) {
            unsigned char lm[4]; char path[32]; long observed=atol(q+24),current=-1;
            sprintf(path,"/proc/%05d",(int)macpid);
            if ((f=open(path,O_RDONLY))>=0) {
                if(rdmac(f,0x8a4L,(char *)lm,4)) current=g32(lm);
                close(f);
            }
            t_info("video_main_identity","public %ld lowmem %ld equal %d",observed,current,observed==current);
        }
		if (!strcmp(q, "mac81probe P finder_process")) finderok = 1;
		if (!strncmp(q, "mac81probe I finder_psn ", 24))
			sscanf(q + 24, "%lu %lu", &hi, &lo);
		if (!strncmp(q, "mac81probe P ", 13) || !strncmp(q, "mac81probe F ", 13)) {
			sprintf(n, "%s_%.60s", pfx, q + 13);
			t_check(n, q[11] == 'P', "%s", q);
			count++;
		}
	}
	sprintf(n, "%s_contracts", pfx);
	t_check(n, count == 82, "%d of 82 contracts", count);
	return done && finderok && !hi ? lo : 0;
}

static int
windowtitle81(fd, title)
int fd;
char *title;
{
    unsigned char b[148], p[256];
    long w, h, text; int i, n;
    if (!rdmac(fd, 0x9d6L, (char *)b, 4)) return 0;
    w=g32(b);
    for (i=0; w && i<64; i++) {
        if ((w&1) || !rdmac(fd,w,(char *)b,sizeof(b))) return 0;
        if (b[110]) {
            h=g32(b+134);
            if (!h || (h&1) || !rdmac(fd,h,(char *)p,4)) return 0;
            text=g32(p);
            if (!text || !rdmac(fd,text,(char *)p,1)) return 0;
            n=p[0];
            if (n && !rdmac(fd,text+1,(char *)p,n)) return 0;
            p[n]=0;
            t_info("panel_window_title","%s",(char *)p);
            return !strcmp((char *)p,title);
        }
        h=g32(b+144);
        if (h==w) return 0;
        w=h;
    }
    return 0;
}

static long idledefaultvalues[5];
static int idledefaultseen;

static int
idlestub81(phase)
int phase;
{
    char *opt=getenv("MACPMSTUB"),*wait=getenv("MACPMIDLE");
    char *names[5]={"uin_pmidlestate","uin_pmidlerequests","uin_pmidleallocations","uin_pmidlerestores","uin_pmidlewatchedcalls"};
    unsigned long base,info;long value[5],zero=0;int i,ok=1;
    if(!opt && !wait) {
        if(getsym("uin_pmidle")!=1 || getsym("uin_pmidlestub")!=1)return 0;
        for(i=0;i<5;i++) {
            base=0;info=0;value[i]=-1;
            if(getksym(names[i],&base,&info)<0 || !base || kmemrw(base,&value[i],0)<0)ok=0;
        }
        t_info("finder_idle_stub","default phase %d state %ld requests %ld allocations %ld restores %ld watched %ld",
            phase,value[0],value[1],value[2],value[3],value[4]);
        if(phase==1) {
            idledefaultseen=0;
            if(!ok || value[0]!=2 || value[1]<1 || value[2]<1)return 0;
            for(i=0;i<5;i++)idledefaultvalues[i]=value[i];
            idledefaultseen=1;return 1;
        }
        return ok && idledefaultseen && value[0]==(phase==2 ? 0 : 2) &&
            value[1]==idledefaultvalues[1] && value[2]==idledefaultvalues[2] &&
            value[3]==idledefaultvalues[3]+(phase==2 ? 1 : 0) &&
            value[4]>=idledefaultvalues[4];
    }
    if(phase==1 && setsym("uin_pmidlestub",0L)<0)ok=0;
    if(!opt || strcmp(opt,"1"))return ok;
    if(!wait || strcmp(wait,"1"))return 0;
    for(i=0;i<5;i++) {
        base=0;info=0;value[i]=-1;
        if(getksym(names[i],&base,&info)<0 || !base || kmemrw(base,&value[i],0)<0)ok=0;
        else if(phase==1 && !i && value[0]!=0)return 0;
        else if(phase==1 && i && kmemrw(base,&zero,1)<0)ok=0;
    }
    t_info("finder_idle_stub","phase %d state %ld requests %ld allocations %ld restores %ld watched %ld",
        phase,value[0],value[1],value[2],value[3],value[4]);
    if(phase==1)return ok && value[0]==0 && setsym("uin_pmidlestub",1L)==0;
    if(phase==2 && setsym("uin_pmidlestub",0L)<0)ok=0;
    return ok && value[0]==(phase==2 ? 0 : 2) && value[1]==1 && value[2]==1 && value[3]==(phase==2 ? 1 : 0);
}

static int
idlepm81(arm)
int arm;
{
    char *opt=getenv("MACPMIDLE");
    char *names[6]={"uin_pmidlecalls","uin_pmidlerefused","uin_pmidlesleeps","uin_pmidleinterrupts","uin_pmidleticks","uin_pmidlezero"};
    unsigned long base,info;long value[6],zero=0,words[12],count;int i,j,row,ok;
    if(!opt && !getenv("MACPMSTUB"))return idlestub81(arm ? 1 : 0);
    ok=setsym("uin_pmidle",0L)==0;
    if(arm && !idlestub81(1))ok=0;
    if(!opt || strcmp(opt,"1"))return ok;
    for(i=0;i<6;i++) {
        base=0;info=0;value[i]=-1;
        if(getksym(names[i],&base,&info)<0 || !base ||
            kmemrw(base,&value[i],0)<0)ok=0;
        else if(arm && kmemrw(base,&zero,1)<0)ok=0;
    }
    t_info("finder_idle_pm","%s calls %ld refused %ld sleeps %ld interrupts %ld",
        arm ? "before" : "after",value[0],value[1],value[2],value[3]);
    t_info("finder_idle_pm_ticks","%s ticks %ld zero %ld",arm ? "before" : "after",value[4],value[5]);
    {long skipped=-1;base=0;info=0;
     if(getksym("uin_watchskipped",&base,&info)>=0 && base)
         if(kmemrw(base,&skipped,0)<0)skipped=-1;
     t_info("finder_idle_filter","%s skipped %ld",arm ? "before" : "after",skipped);
    }
    if(arm)return ok && setsym("uin_pmidletracecount",0L)==0 && setsym("uin_pmidle",1L)==0;
    count=getsym("uin_pmidletracecount");base=0;info=0;
    if(count<0 || count>8 || getksym("uin_pmidletrace",&base,&info)<0 || !base)return 0;
    for(row=0;row<count;row++) {
        for(j=0;j<12;j++) {words[j]=-1;if(kmemrw(base+row*48+j*4,&words[j],0)<0)ok=0;}
        t_info("finder_idle_pm_signal","row %d tick %lx/%lx result %ld signal %ld/%ld pending %lx/%lx held %lx/%lx pid %ld pc %lx queue %ld",
            row,words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7],words[8],words[9],words[10],words[11]);
    }
    return ok && idlestub81(0);
}

static int
idlewne81(arm)
int arm;
{
    unsigned long base[2],info;long values[10],zero=0;int i,j,at=0,ok=1;
    char *names[2]={"uin_wnestats","uin_wnelast"};
    if(setsym("uin_frontdiag",0L)<0)ok=0;
    for(i=0;i<2;i++) {
        base[i]=0;info=0;
        if(getksym(names[i],&base[i],&info)<0 || !base[i]) {ok=0;continue;}
        for(j=0;j<(i ? 6 : 4);j++) {
            values[at]=-1;
            if(kmemrw(base[i]+j*4,arm ? &zero : &values[at],arm)<0)ok=0;
            at++;
        }
    }
    if(arm)return ok && setsym("uin_frontdiag",1L)==0;
    if(ok) {
        t_info("finder_idle_wne","calls %ld zero %ld positive %ld unlimited %ld",
            values[0],values[1],values[2],values[3]);
        t_info("finder_idle_wne_last","caller %lx mask %lx sleep %lx region %lx event %lx sp %lx",
            values[4],values[5],values[6],values[7],values[8],values[9]);
    }
    return ok;
}

static int
publicwindowtitle81(fd,title,baseline)
int fd;
char *title;
long baseline;
{
    char name[256];unsigned long at=0,info;long serial,count,until=t_now_ms()+10000;
    int f,ok=0;
    if(getksym("uin_fronttitle",&at,&info)<0 || !at)return 0;
    while(t_now_ms()<until) {
        count=getsym("uin_frontcount");serial=getsym("uin_frontserial");
        if(count>baseline && serial>=0 && !(serial&1) && getsym("uin_frontpid")==macpid) {
            f=open("/dev/kmem",O_RDONLY);
            if(f>=0) {
                ok=lseek(f,(off_t)at,SEEK_SET)!=-1 && read(f,name,sizeof name)==sizeof name;
                close(f);
            }
            if(ok && getsym("uin_frontserial")==serial && name[255]==0) {
                t_info("panel_window_title","%s",name);
                if(!strcmp(name,title))return 1;
            }
        }
        poll((struct pollfd *)0,0L,100);
    }
    return 0;
}

static int
monitorstate81(fd, state)
int fd;
unsigned char *state;
{
    unsigned char b[46], p[34]; long h,g,pm;
    if (!rdmac(fd,0x8a4L,(char *)b,4) || !(h=g32(b)) || (h&1) ||
        !rdmac(fd,h,(char *)b,4) || !(g=g32(b)) || (g&1) ||
        !rdmac(fd,g,(char *)b,sizeof(b)) || !(h=g32(b+22)) || (h&1) ||
        !rdmac(fd,h,(char *)p,4) || !(pm=g32(p)) || (pm&1) ||
        !rdmac(fd,pm,(char *)p,sizeof(p))) return 0;
    memcpy((char *)state,(char *)b,sizeof(b));
    state[46]=p[32]; state[47]=p[33];
    return 1;
}

static int
monitorcontrols81(fd,w,b,x,y)
int fd;
long w;
unsigned char *b;
int *x,*y;
{
    unsigned char hp[4],rb[10],c[105]; long h,region,a,next;
    int i,n,k,found=0,dx,dy,left,top,right,bottom;
    h=g32(b+118);
    if(!h || (h&1) || !rdmac(fd,h,(char *)hp,4) || !(region=g32(hp)) ||
        (region&1) || !rdmac(fd,region,(char *)rb,sizeof(rb)) ||
        ((rb[0]<<8)|rb[1])<10) return 0;
    dx=(short)((rb[4]<<8)|rb[5])-(short)((b[18]<<8)|b[19]);
    dy=(short)((rb[2]<<8)|rb[3])-(short)((b[16]<<8)|b[17]);
    t_info("monitors_origin","content %d,%d port %d,%d translation %d,%d",
        (short)((rb[4]<<8)|rb[5]),(short)((rb[2]<<8)|rb[3]),
        (short)((b[18]<<8)|b[19]),(short)((b[16]<<8)|b[17]),dx,dy);
    h=g32(b+140);
    for(i=0;h && i<64;i++,h=next) {
        if((h&1) || !rdmac(fd,h,(char *)hp,4) || !(a=g32(hp)) || (a&1) ||
            !rdmac(fd,a,(char *)c,41))return 0;
        next=g32(c); n=c[40];
        if(n>63) {if(next==h)return 0;continue;}
        if(n && !rdmac(fd,a+41,(char *)c+41,n))return 0;
        c[41+n]=0;
        k=!strcmp((char *)c+41,"Colors:")?0:!strcmp((char *)c+41,"Grays:")?1:-1;
        if(k>=0) {
            left=(short)((c[10]<<8)|c[11]);top=(short)((c[8]<<8)|c[9]);
            right=(short)((c[14]<<8)|c[15]);bottom=(short)((c[12]<<8)|c[13]);
            if(g32(c+4)!=w || !c[16] || c[17]==255 || right<=left || bottom<=top || (found&(1<<k)))return 0;
            x[k]=dx+(left+right)/2;y[k]=dy+(top+bottom)/2;
            t_info("monitors_control","%s owner %lx rect %d,%d,%d,%d center %d,%d",
                (char *)c+41,w,left,top,right,bottom,x[k],y[k]);
            if(x[k]<(short)((rb[4]<<8)|rb[5]) || x[k]>=(short)((rb[8]<<8)|rb[9]) ||
                y[k]<(short)((rb[2]<<8)|rb[3]) || y[k]>=(short)((rb[6]<<8)|rb[7]))return 0;
            found|=1<<k;
        }
        if(next==h)return 0;
    }
    return !h && found==3;
}

static void
monitorcolor81(fd)
int fd;
{
    unsigned char initial[48],state[48],b[148];
    long w,stop,count; int i,x[2],y[2],ready,origin=0; char name[64],shot[96];
    if (!t_check("monitors_initial_device",monitorstate81(fd,initial),"unreadable main GDevice")) return;
    if (!t_check("monitors_device_shape",initial[0]==0xff && initial[1]==0xcd &&
        !initial[34] && !initial[35] && !initial[36] && !initial[37] &&
        ((initial[38]<<8)|initial[39])==600 && ((initial[40]<<8)|initial[41])==800 &&
        g32(initial+42)==128 && !initial[46] && initial[47]==8,
        "expected hosted800x6008-bit ref-51")) return;
    if (!t_check("monitors_front",windowtitle81(fd,"Monitors"),"Monitors must remain front")) return;
    if (rdmac(fd,0x9d6L,(char *)b,4)) {
        w=g32(b);
        for(i=0;w && i<64;i++) {
            if ((w&1) || !rdmac(fd,w,(char *)b,sizeof(b))) break;
            if(b[110]) {origin=monitorcontrols81(fd,w,b,x,y);break;}
            if(g32(b+144)==w)break;
            w=g32(b+144);
        }
    }
    if (!t_check("monitors_radio_geometry",origin && x[0]>=0 && x[0]<800 && y[0]>=0 && y[0]<600 &&
        x[1]>=0 && x[1]<800 && y[1]>=0 && y[1]<600,"radio controls unavailable or outside display")) return;
    for(i=0;i<2;i++) {
        if (!windowtitle81(fd,"Monitors")) {t_check("monitors_switch_front",0,"Monitors lost front");return;}
        count=getsym("uinter_video_graysets");
        if (count<0 || !moveto(x[i],y[i])) {t_check("monitors_switch_input",0,"driver diagnostic or pointer unavailable");return;}
        host("button 1");host("button 0");
        memcpy((char *)state,(char *)initial,sizeof(state));
        stop=t_now_ms()+10000L;ready=0;
        while(t_now_ms()<stop) {
            if(monitorstate81(fd,state) && ((state[21]&1)==!i) &&
                getsym("uinter_video_gray")==i && getsym("uinter_video_graysets")>count) {ready=1;break;}
            pause_ms(100L);
        }
        sprintf(name,"monitors_%s_state",i?"grays":"colors");
        t_check(name,ready,"GDevice flag %d gray %ld controls %ld",state[21]&1,
            getsym("uinter_video_gray"),getsym("uinter_video_graysets")-count);
        sprintf(shot,"shot " SHOT "monitors_%s",i?"grays":"colors");host(shot);
        sprintf(name,"monitors_%s_geometry",i?"grays":"colors");
        t_check(name,ready && !memcmp((char *)state+34,(char *)initial+34,14),"mode/rect/depth changed");
        if(!ready)return;
    }
}

static void
panel81(name, label)
char *name, *label;
{
    char cmd[64], shot[96], check[96], title[64], path[32];
    int n, fd, identified=0; char *q;long frontbase;
    t_rearm(120);
    frontbase=getsym("uin_frontcount");
    t_check("panel_public_window_armed",frontbase>=0 && setsym("uin_frontdiag",1L)==0,"");
    moveto(10, 450);
    host("button 1"); host("button 0");
    pause_ms(1500L);
    host("shot " SHOT "panel_desktop");
    for (q=name; *q; q++) {
        int c=*q;
        if (c>='A' && c<='Z') c+='a'-'A';
        sprintf(cmd,"key %c",c);
        host(cmd);
    }
    host("key meta_l+o");
    sprintf(shot, SHOT "%s_open",label);
    n=waitshot(shot,SHOT "panel_desktop",10000,1 << 30,40);
    sprintf(check,"%s_panel_open",label);
    if (!t_check(check,n>=10000,"%d pixels changed",n)) {setsym("uin_frontdiag",0L);return;}
    strncpy(title,name,sizeof(title)-1); title[sizeof(title)-1]=0;
    if (title[0]>='a' && title[0]<='z') title[0]+='A'-'a';
    sprintf(path,"/proc/%05d",(int)macpid);
    if ((fd=open(path,O_RDONLY))>=0) {
        identified=publicwindowtitle81(fd,title,frontbase);
        close(fd);
    }
    setsym("uin_frontdiag",0L);
    sprintf(check,"%s_panel_identity",label);
    t_check(check,identified,"expected front window %s",title);
    if (identified && !strcmp(title,"Monitors") && (fd=open(path,O_RDONLY))>=0) {
        monitorcolor81(fd); close(fd);
    }
    host("key meta_l+w");
    sprintf(shot,SHOT "%s_closed",label);
    n=waitshot(shot,SHOT "panel_desktop",0,1999,40);
    sprintf(check,"%s_panel_closed",label);
    t_check(check,n>=0 && n<2000,"%d pixels changed",n);
    sprintf(check,"%s_panel_alive",label);
    t_check(check,alive76(),"");
}

static int
namedksym81(name,addr,info)
char *name;
unsigned long *addr,*info;
{
    *addr=0;
    return getksym(name,addr,info);
}

static int
commandpid81()
{
    DIR *d; struct dirent *de; prpsinfo_t ps;
    char path[64]; int fd, pid=0;
    if (!(d=opendir("/proc"))) return 0;
    while ((de=readdir(d))) {
        if (de->d_name[0]=='.') continue;
        sprintf(path,"/proc/%s",de->d_name);
        if ((fd=open(path,O_RDONLY))<0) continue;
        if (ioctl(fd,PIOCPSINFO,&ps)==0 && !ps.pr_zomb &&
            !strcmp(ps.pr_fname,"CommandShell")) pid=ps.pr_pid;
        close(fd);
    }
    closedir(d); return pid;
}

static void
command81()
{
    int i, n, pid=0; char cmd[32], *q="commandshell";
    t_rearm(120);
    if (!t_check("coff_initial",!commandpid81(),"CommandShell already running")) return;
    host("shot " SHOT "coff_desktop");
    for (;*q;q++) { sprintf(cmd,"key %c",*q); host(cmd); }
    host("key meta_l+o");
    for (i=0;i<100 && !(pid=commandpid81());i++) pause_ms(100L);
    t_info("coff_pid","%d",pid);
    if (!t_check("coff_process",pid>0,"no CommandShell process")) {
        host("shot " SHOT "coff_rejected");
        host("key ret");
        n=waitshot(SHOT "coff_failure_closed",SHOT "coff_desktop",0,1999,40);
        t_check("coff_failure_restored",n>=0 && n<2000,"%d pixels changed",n);
        return;
    }
    n=waitshot(SHOT "coff_open",SHOT "coff_desktop",10000,1 << 30,40);
    if (!t_check("coff_window",n>=10000,"%d pixels changed",n)) return;
    host("key meta_l+q");
    for (i=0;i<100 && commandpid81();i++) pause_ms(100L);
    t_check("coff_quit",!commandpid81(),"CommandShell still running");
    n=waitshot(SHOT "coff_closed",SHOT "coff_desktop",0,1999,40);
    t_check("coff_desktop",n>=0 && n<2000,"%d pixels changed",n);
    t_check("coff_finder_alive",alive76(),"");
}

static void
coffpm81(fd)
int fd;
{
    unsigned char b[16];char hex[33];long a,base,pc;int i,j,n;
    static int selectors[]={16,17,55,59};
    if(lseek(fd,(off_t)0x904,0)!=-1 && read(fd,(char *)b,4)==4) {
        a=((long)b[0]<<24)|((long)b[1]<<16)|((long)b[2]<<8)|b[3];
        t_info("carrier_currenta5","%lx",a);
    }
    if(lseek(fd,(off_t)0xb7c,0)==-1 || read(fd,(char *)b,4)!=4)return;
    a=((long)b[0]<<24)|((long)b[1]<<16)|((long)b[2]<<8)|b[3];
    t_info("carrier_systema5","%lx",a);
    if(a<0x11000L || !cofframrange(fd,a-3308,3308+60*8))return;
    base=a-3308;
    for(i=0;i<4;i++) {
        a=base+8*selectors[i];
        if(lseek(fd,(off_t)a,0)==-1 || read(fd,(char *)b,8)!=8)continue;
        pc=((long)b[0]<<24)|((long)b[1]<<16)|((long)b[2]<<8)|b[3];
        t_info("carrier_pm_selector","%d address %lx target %lx input %d output %d",
            selectors[i],a,pc,((int)b[4]<<8)|b[5],((int)b[6]<<8)|b[7]);
        if(pc<0x10000L || !cofframrange(fd,pc,16) || (pc&1) ||
            lseek(fd,(off_t)pc,0)==-1 || (n=read(fd,(char *)b,16))!=16)continue;
        for(j=0;j<n;j++)sprintf(hex+2*j,"%02x",b[j]);
        t_info("carrier_pm_code","%d %lx %s",selectors[i],pc,hex);
    }
}

static void
commanddirect81once()
{
    int i,n,pid=0,fd,owned=0,quit=0,killed=0,done=0,io=0;long reaped=0,status=-1;prpsinfo_t ps;
    char path[48],cmd[32],buf[4097],*line,*q="commandshell";
    t_rearm(120);
    t_check("coff_diag_armed",setsym("uin_coffdiag",1L)==0 && setsym("aux_filediag",2L)==0,"");
    if (!t_check("coff_initial",!commandpid81(),"CommandShell already running")) {
        t_info("coff_adapter_state","launch %ld front %ld exit %ld refused %ld",getsym("uin_cofflaunch"),getsym("uin_cofffront"),getsym("uin_coffexit"),getsym("uin_coffrefused"));
    t_info("coff_diag_state","seen %ld closed %ld old55 %ld",getsym("uin_coffseen"),getsym("uin_coffclosed"),getsym("uin_coffcalls"));
    setsym("aux_trace",0L);
    setsym("uin_coffdiag",0L);setsym("aux_filediag",0L);return;
    }
    sprintf(path,"/proc/%d",(int)macpid);
    if((fd=open(path,O_RDONLY))>=0) {coffcommtarget(fd);coffpm81(fd);close(fd);}
    unlink("/tmp/coffprobe.out");unlink("/tmp/coffchild.out");
    host("shot " SHOT "coff_desktop");
    t_check("coff_unix_trace",setsym("aux_trace",7L | 16L)==0,"");
    for (;*q;q++) { sprintf(cmd,"key %c",*q); host(cmd); }
    host("key meta_l+o");
    for (i=0;i<100 && !(pid=commandpid81());i++) pause_ms(100L);
    t_info("coff_pid","%d",pid);
    if(pid>1) {
        sprintf(path,"/proc/%d",pid);
        if((fd=open(path,O_RDONLY))>=0) {
            if(ioctl(fd,PIOCPSINFO,&ps)==0 && ps.pr_pid==pid && ps.pr_ppid==macpid &&
                !strcmp(ps.pr_fname,"CommandShell") && !ps.pr_zomb) {
                owned=1;coffpm81(fd);
            }
            close(fd);
        }
    }
    if (!t_check("coff_process",owned,"pid %d owner %ld",pid,(long)macpid)) {
        coffsnapshot(0);
        setsym("aux_trace",16L);
        host("shot " SHOT "coff_rejected");
        host("key ret");
        n=waitshot(SHOT "coff_failure_closed",SHOT "coff_desktop",0,1999,40);
        t_check("coff_failure_restored",n>=0 && n<2000,"%d pixels changed",n);
        t_info("coff_adapter_state","launch %ld front %ld exit %ld refused %ld",getsym("uin_cofflaunch"),getsym("uin_cofffront"),getsym("uin_coffexit"),getsym("uin_coffrefused"));
    t_info("coff_diag_state","seen %ld closed %ld old55 %ld",getsym("uin_coffseen"),getsym("uin_coffclosed"),getsym("uin_coffcalls"));
    setsym("aux_trace",0L);
    setsym("uin_coffdiag",0L);setsym("aux_filediag",0L);
        return;
    }
    setsym("aux_trace",16L);
    n=waitshot(SHOT "coff_open",SHOT "coff_desktop",10000,1 << 30,40);
    t_info("coff_window_pixels","%d pixels changed",n);
    coffsnapshot(pid);
    coffordinary(pid);
    setsym("aux_trace",16L);
    unlink("/tmp/coffio");
    q="touch /tmp/coffio";
    for(;*q;q++) {
        if(*q==' ') sprintf(cmd,"key spc");
        else if(*q=='/') sprintf(cmd,"key slash");
        else sprintf(cmd,"key %c",*q);
        host(cmd);
    }
    host("key ret");
    for(i=0;i<100 && access("/tmp/coffio",F_OK)!=0;i++) pause_ms(100L);
    io=access("/tmp/coffio",F_OK)==0;
    t_check("coff_visible_output",coffvisible(pid),"terminal must render command output");
    t_check("coff_window",n>=10000 && io,"%d pixels changed; typed command %d",n,io);
    t_check("coff_original_io",io,
        "CommandShell must execute the typed command");
    unlink("/tmp/coffio");
    if(!io) {host("key ret");pause_ms(500L);}
    host("key meta_l+q");
    for (i=0;i<100 && commandpid81();i++) pause_ms(100L);
    quit=!commandpid81();
    t_check("coff_quit",io && quit,"CommandShell still running");
    if(!quit) {host("shot " SHOT "coff_quit_pending");coffsnapshot(pid);}
    if(!quit && (fd=open(path,O_RDONLY))>=0) {
        if(ioctl(fd,PIOCPSINFO,&ps)==0 && ps.pr_pid==pid && ps.pr_ppid==macpid &&
            !strcmp(ps.pr_fname,"CommandShell") && !ps.pr_zomb) killed=kill(pid,SIGKILL)==0;
        close(fd);pause_ms(1000L);
    }
    t_info("coff_owned_cleanup","fallback killed %d",killed);
    if((fd=open("/tmp/coffprobe.out",O_RDONLY))>=0) {
        n=read(fd,buf,sizeof buf-1);close(fd);
        if(n>0) {
            buf[n]=0;coffparentstack(buf);line=strstr(buf,"reaped ");if(line)sscanf(line,"reaped %ld",&reaped);
            line=strstr(buf,"status ");if(line)sscanf(line,"status %ld",&status);
            done=strstr(buf,"done\n")!=0;
            for(i=0;i<n;i+=128)t_info("coff_native_owner","%.*s",n-i<128?n-i:128,buf+i);
        }
    }
    t_check("coff_native_reaped",reaped==pid && done,"pid %d reaped %ld done %d",pid,reaped,done);
    t_check("coff_normal_status",io && quit && reaped==pid && done && status==0,"quit %d Unix status %ld",quit,status);
    n=waitshot(SHOT "coff_closed",SHOT "coff_desktop",0,1999,40);
    t_check("coff_desktop",n>=0 && n<2000,"%d pixels changed",n);
    t_check("coff_finder_alive",alive76(),"");
    t_info("coff_adapter_state","launch %ld front %ld exit %ld refused %ld",getsym("uin_cofflaunch"),getsym("uin_cofffront"),getsym("uin_coffexit"),getsym("uin_coffrefused"));
    t_info("coff_diag_state","seen %ld closed %ld old55 %ld",getsym("uin_coffseen"),getsym("uin_coffclosed"),getsym("uin_coffcalls"));
    setsym("aux_trace",0L);
    setsym("uin_coffdiag",0L);setsym("aux_filediag",0L);
}

static void
commanddirect81()
{
    char *opt=getenv("COFFNATIVE47");
    long saved=getsym("uinter_coffnative47");
    int i,native47=opt ? !strcmp(opt,"1") : saved;
    t_check("coff_native47_mode",setsym("uinter_coffnative47",native47)==0,"");
    for(i=0;i<2;i++) {
        t_info("coff_launch_iteration","%d",i+1);
        commanddirect81once();
    }
    setsym("uinter_coffnative47",saved);
}


static void
commandcarrier81()
{
    char cmd[32],buf[4097],path[40],*q="coffcarrier",*line,*end;
    long owner=0,child=0,reaped=0,stack=0; int i,fd,n,killed=0,live=0,io=0,done=0,j,k,off; prpsinfo_t ps; prstatus_t st;
    unsigned char memory[32];char hex[65];long address;
    t_check("carrier_staged",access("/etc/aux/CoffProbe",F_OK)==0,"");
    if(access("/etc/aux/CoffProbe",F_OK)<0) return;
    t_rearm(65);
    t_check("coff_diag_armed",setsym("uin_coffdiag",1L)==0 && setsym("aux_filediag",2L)==0,"");
    unlink("/tmp/coffprobe.out"); unlink("/tmp/coffchild.out"); unlink("/tmp/coffio");
    host("shot " SHOT "carrier_before");
    sprintf(cmd,"move %d %d",scrw-30,scrh-30); host(cmd);
    host("down left");pause_ms(150L);host("up left");pause_ms(1200L);
    for(;*q;q++) {sprintf(cmd,"key %c",*q);host(cmd);}
    host("key meta_l+o");
    for(i=0;i<150 && !child;i++) {
        if((fd=open("/tmp/coffprobe.out",O_RDONLY))>=0) {
            n=read(fd,buf,sizeof buf-1);close(fd);
            if(n>0) {buf[n]=0;line=strstr(buf,"owner ");if(line) sscanf(line,"owner %ld",&owner);
                line=strstr(buf,"child ");if(line) sscanf(line,"child %ld",&child);}
        }
        if(!child) pause_ms(100L);
    }
    t_info("coff_carrier","owner %ld child %ld",owner,child);
    if(child>1 && owner>1) {
        pause_ms(10000L);
        host("shot " SHOT "carrier_child");
        sprintf(path,"/proc/%ld",child);
        if((fd=open(path,O_RDONLY))>=0) {
            if(ioctl(fd,PIOCPSINFO,&ps)==0) {
                t_info("coff_carrier_child","pid %d parent %d command %s zombie %d",
                    (int)ps.pr_pid,(int)ps.pr_ppid,ps.pr_fname,ps.pr_zomb);
                if(ps.pr_pid==child && ps.pr_ppid==owner &&
                    !strcmp(ps.pr_fname,"CommandShell") && !ps.pr_zomb) {
                    live=1;
                    coffpm81(fd);
                    if(ioctl(fd,PIOCSTATUS,&st)==0) {
                        t_info("carrier_child_state","pid %d state %c wchan %lx PC %lx SP %lx instr %lx",
                            (int)ps.pr_pid,ps.pr_sname,(long)ps.pr_wchan,
                            (long)st.pr_reg[16],(long)st.pr_reg[15],st.pr_instr);
                        for(j=0;j<2;j++) {
                            address=(long)st.pr_reg[j?15:16];
                            if(lseek(fd,(off_t)address,0)!=-1 && (n=read(fd,(char *)memory,sizeof memory))>0) {
                                for(k=0;k<n;k++)sprintf(hex+2*k,"%02x",memory[k]);
                                t_info("carrier_child_memory","%lx %s",address,hex);
                                if(j==1) for(off=0;off+8<=n;off+=4) {
                                    if(memory[off]==0x20 && memory[off+6]==0 && memory[off+7]==0x28) {
                                        address=((long)memory[off+2]<<24)|((long)memory[off+3]<<16)|
                                            ((long)memory[off+4]<<8)|memory[off+5];
                                        if(address>0x10000L && cofframrange(fd,address,16) &&
                                            lseek(fd,(off_t)address,0)!=-1 && read(fd,(char *)memory,16)==16) {
                                            for(k=0;k<16;k++)sprintf(hex+2*k,"%02x",memory[k]);
                                            t_info("carrier_native_call","%lx %s",address,hex);
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    q="touch /tmp/coffio";
                    for(;*q;q++) {
                        sprintf(cmd,"key %s",*q==' '?"spc":*q=='/'?"slash":"");
                        if(*q!=' ' && *q!='/') sprintf(cmd,"key %c",*q);
                        host(cmd);
                    }
                    host("key ret");
                    for(i=0;i<100 && !io;i++) {io=access("/tmp/coffio",F_OK)==0;if(!io)pause_ms(100L);}
                    killed=kill((pid_t)child,SIGKILL)==0;
                }
            }
            close(fd);
        }
    }
    t_check("carrier_child",live,"owner %ld child %ld",owner,child);
    t_check("carrier_original_io",io,"CommandShell must execute the typed command");
    t_check("carrier_owned_cleanup",killed,"");
    t_info("coff_carrier_cleanup","owned child killed %d",killed);
    pause_ms(3000L); host("shot " SHOT "carrier_after");
    if((fd=open("/tmp/coffprobe.out",O_RDONLY))>=0) {
        n=read(fd,buf,sizeof buf-1);close(fd);if(n>0) {
            buf[n]=0;coffparentstack(buf);
            for(line=buf;*line;line=end) {
                end=strchr(line,'\n');if(!end)break;*end++=0;
                t_info("coff_carrier_log","%s",line);
                if(!strncmp(line,"reaped ",7))sscanf(line+7,"%ld",&reaped);
                if(!strncmp(line,"fork_child_sp ",14))sscanf(line+14,"%ld",&stack);
                if(!strcmp(line,"done"))done=1;
            }

        }
    }
    if((fd=open("/tmp/coffchild.out",O_RDONLY))>=0) {
        n=read(fd,buf,sizeof buf-1);close(fd);if(n>0) {buf[n]=0;for(i=0;i<n;i+=128)t_info("coff_carrier_binary","%.*s",n-i<128 ? n-i : 128,buf+i);}
    }
    t_check("carrier_private_stack",stack==0x3fffff00L,"%ld",stack);
    t_check("carrier_reaped",child>1 && reaped==child && done,"child %ld reaped %ld done %d",child,reaped,done);
    t_check("carrier_finder",alive76(),"");
    t_info("coff_adapter_state","launch %ld front %ld exit %ld refused %ld",getsym("uin_cofflaunch"),getsym("uin_cofffront"),getsym("uin_coffexit"),getsym("uin_coffrefused"));
    t_info("coff_diag_state","seen %ld closed %ld old55 %ld",getsym("uin_coffseen"),getsym("uin_coffclosed"),getsym("uin_coffcalls"));
    setsym("uin_coffdiag",0L);setsym("aux_filediag",0L);
    unlink("/tmp/coffio");
}

static void
commandunix81()
{
    static char *av[] = { "/mac/bin/CommandShell", 0 };
    char buf[8193], path[32];
    unsigned char low[4], a5data[16];
    long a5;
    int fd, n, i, st=0, done=0;
    pid_t child, w;
    long trace=getsym("uinter_trace");

    if (access("/etc/aux/CoffUnixProbe",F_OK)<0) return;
    t_rearm(45);
    fd=open("/tmp/coffunix.out",O_WRONLY|O_CREAT|O_TRUNC,0600);
    if (fd<0) return;
    setsym("uinter_trace",1L);
    child=fork();
    if (child==0) {
        setpgrp(); dup2(fd,1); dup2(fd,2); close(fd);
        execve(av[0],av,macenv); _exit(127);
    }
    close(fd);
    if (child>0) {
        for (i=0;i<200;i++) {
            w=waitpid(child,&st,WNOHANG);
            if (w==child) { done=1; break; }
            if (w<0) { done=1; st=-1; break; }
            pause_ms(100L);
        }
        t_info("coff_unix","pid %d live %d exited %d status %#x",(int)child,
            commandpid81(),done,st);
        sprintf(path,"/proc/%d",(int)child);
        if ((fd=open(path,O_RDONLY))>=0) {
            if (rdmac(fd,0x904L,(char *)low,4)) {
                a5=g32(low);
                t_info("coff_unix_a5","%#lx readable %d",a5,
                    a5>0 && !(a5&1) && rdmac(fd,a5-4,(char *)a5data,sizeof a5data));
            }
            if (rdmac(fd,0x9d6L,(char *)low,4))
                t_info("coff_unix_windows","%#lx",g32(low));
            if (rdmac(fd,0xb7cL,(char *)low,4)) {
                a5=g32(low);
                if (a5>1106 && !(a5&1) && rdmac(fd,a5-1106,(char *)low,4))
                    t_info("coff_unix_native_current","%#lx",g32(low));
            }
            close(fd);
        }
        host("shot " SHOT "coff_unix");
        if (!done) {
            kill(-child,SIGKILL);
            w=t_waitchild(child,&st,10);
            t_info("coff_unix_cleanup","wait %d status %#x",(int)w,st);
        }
    }
    if (trace>=0) setsym("uinter_trace",trace);
    if ((fd=open("/tmp/coffunix.out",O_RDONLY))>=0) {
        n=read(fd,buf,sizeof buf-1);
        if (n>0) { buf[n]=0; for(i=0;i<n;i+=128)t_info("coff_unix_output","%.*s",n-i<128 ? n-i : 128,buf+i); }
        close(fd);
    }
    unlink("/tmp/coffunix.out");
}

static void
recover81()
{
	unsigned long before, after;
	int n;
	struct stat root, mount;

	commandunix81();

	t_check("finder_copy_cross_mount", stat("/", &root)==0 &&
	    stat("/macsys", &mount)==0 && root.st_dev!=mount.st_dev,
	    "source and destination require separate host filesystems");
	t_rearm(240);
	before = probe81("filemanager");
	if (!before) return;
	setsym("uin_polldiag",1L);
	setsym("uin_forcehitcount",0L); setsym("uin_forcehitpending",0L);
    {
        unsigned long sym,addr=0; long zero=0; int k;
        if(namedksym81("uin_forcehitcalls",&addr,&sym)==0)
            for(k=0;k<2;k++) kmemrw(addr+k*4,&zero,1);
        if(namedksym81("uin_pollnative",&addr,&sym)==0)
            for(k=0;k<4;k++) kmemrw(addr+k*4,&zero,1);
        if(namedksym81("uin_forcehitlast",&addr,&sym)==0)
            for(k=0;k<4;k++) kmemrw(addr+k*4,&zero,1);
    }
	pause_ms(1000L);
	host("shot " SHOT "before_forcequit");
	host("down alt"); host("down meta_l"); host("down esc");
	pause_ms(1500L);
	host("up esc"); host("up meta_l"); host("up alt");
	n = waitshot(SHOT "forcequit_dialog", SHOT "before_forcequit", 1000, 1 << 30, 20);
	t_info("forcequit_path", "admit %ld arm %ld WNE %ld dispatch %ld retry %ld expire %ld off %ld",
	    getsym("uin_forceadmit"), getsym("uin_forcearm"), getsym("uin_forcewne"),
	    getsym("uin_forcedispatch"), getsym("uin_forceretry"),
	    getsym("uin_forceexpire"), getsym("uin_forceoff"));
	t_info("forcequit_observe", "active %ld flags %lx A-lines %ld last %lx at %lx",
	    getsym("uin_forceactive"), getsym("uin_forceflags"), getsym("uin_forcealine"),
	    getsym("uin_forceop"), getsym("uin_forcepc"));
	t_info("forcequit_publish", "published %ld refused %ld",
	    getsym("uin_forcepublish"), getsym("uin_forcefailed"));
	if (!t_check("forcequit_dialog", n >= 1000, "%d pixels changed", n)) return;
	if (!t_check("forcequit_cancel_button", moveto(581, 212), "")) return;
	host("button 1"); host("button 0");
	n = waitshot(SHOT "forcequit_cancelled", SHOT "before_forcequit", 0, 1999, 20);
	t_info("forcequit_modal", "poll %ld restored %ld", getsym("uin_forcepoll"), getsym("uin_forceunpoll"));
	t_info("forcequit_input", "mouse %lx host %lx MB %lx cursor %lx word %ld mode %ld",
	    getsym("uin_pollmouse"),getsym("uin_pollhost"),getsym("uin_pollmb"),
	    getsym("uin_pollcursor"),getsym("uin_pollword"),getsym("uin_pollmode"));
	t_info("forcequit_button", "down %ld up %ld edges %ld faults %ld downmouse %lx downhost %lx downcursor %lx downMB %lx",
	    getsym("uin_polldown"),getsym("uin_pollup"),getsym("uin_polledges"),getsym("uin_pollfault"),
	    getsym("uin_polldownmouse"),getsym("uin_polldownhost"),getsym("uin_polldowncursor"),getsym("uin_polldownmb"));
	{
	    unsigned long addr=0,info; long words[8]; int block,j,ok,count;
	    if(namedksym81("uin_pollport",&addr,&info)==0) for(block=0;block<16;block++) {
	        ok=1;
	        for(j=0;j<4;j++) if(kmemrw(addr+block*16+j*4,&words[j],0)<0) ok=0;
	        if(ok) t_info("forcequit_port","%d %08lx %08lx %08lx %08lx",
	            block,words[0],words[1],words[2],words[3]);
	    }
        {
            long calls[2]={-1,-1}, last[4]={-1,-1,-1,-1}, native[4]={-1,-1,-1,-1}; int k;
            if(namedksym81("uin_pollnative",&addr,&info)==0)
                for(k=0;k<4;k++) kmemrw(addr+k*4,&native[k],0);
            t_info("forcequit_nativepoll","samples %ld MB %lx return %lx pressed %ld",
                native[0],native[1],native[2],native[3]);
            if(namedksym81("uin_pollcalls",&addr,&info)==0) {
                long modes[5]; for(k=0;k<5;k++) {modes[k]=-1;kmemrw(addr+k*4,&modes[k],0);}
                t_info("forcequit_pollcalls","%ld %ld %ld %ld %ld",modes[0],modes[1],modes[2],modes[3],modes[4]);
            }
            if(namedksym81("uin_pollstatelast",&addr,&info)==0) {
                long state[5];for(k=0;k<5;k++) {state[k]=-1;kmemrw(addr+k*4,&state[k],0);}
                t_info("forcequit_statepath","PC %lx expected %lx stage %ld SP %lx copyfaults %lx",state[0],state[1],state[2],state[3],state[4]);
            }
            t_info("forcequit_interrupts","input %ld restart prevented %ld",getsym("aux_pollintr"),getsym("aux_pollretry"));
            if(namedksym81("aux_polllast",&addr,&info)==0) {
                long state[4];for(k=0;k<4;k++) {state[k]=-1;kmemrw(addr+k*4,&state[k],0);}
                t_info("forcequit_restart","compat %lx signal %ld restartmask %lx PC %lx",state[0],state[1],state[2],state[3]);
            }
            if(namedksym81("uin_forcehitcalls",&addr,&info)==0)
                for(k=0;k<2;k++) kmemrw(addr+k*4,&calls[k],0);
            if(namedksym81("uin_forcehitlast",&addr,&info)==0)
                for(k=0;k<4;k++) kmemrw(addr+k*4,&last[k],0);
            t_info("forcequit_hitpath","point %ld result %ld PC %lx expected %lx mode %ld stage %ld count %ld pending %ld",
                calls[0],calls[1],last[0],last[1],last[2],last[3],getsym("uin_forcehitcount"),getsym("uin_forcehitpending"));
        }
	    count=getsym("uin_forcehitcount"); if(count<0 || count>32) count=0;
	    if(namedksym81("uin_forcehits",&addr,&info)==0) for(block=0;block<count;block++) {
	        ok=1;
	        for(j=0;j<8;j++) if(kmemrw(addr+block*32+j*4,&words[j],0)<0) ok=0;
	        if(ok) t_info("forcequit_hit","%d point %lx A3 %lx rect %lx %lx D3 %lx result %lx CCR %lx MB %lx",
	            block,words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7]);
	    }
	    setsym("uin_polldiag",0L);
        setsym("aux_polldiag",0L);
	}
	if (!t_check("forcequit_cancel_restored", n >= 0 && n < 2000,
	    "%d pixels changed", n)) return;
	if (!t_check("forcequit_cancel_alive", alive76(), "")) return;
	host("down alt"); host("down meta_l"); host("down esc");
	pause_ms(1500L);
	host("up esc"); host("up meta_l"); host("up alt");
	n = waitshot(SHOT "forcequit_reopened", SHOT "before_forcequit", 1000, 1 << 30, 20);
	if (!t_check("forcequit_reopened", n >= 1000, "%d pixels changed", n)) return;
	if (!t_check("forcequit_quit_button", moveto(505, 212), "")) return;
	host("button 1"); host("button 0");
	n = waitshot(SHOT "finder_relaunched", SHOT "before_forcequit", 0, 1999, 60);
	t_check("finder_desktop_restored", n >= 0 && n < 2000, "%d pixels changed", n);
	after = probe81("after_relaunch");
	t_check("finder_process_replaced", before && after && before != after,
	    "Finder serial %lu -> %lu", before, after);
	if (!t_check("relaunch_alive", alive76(), "") || !after || before == after) return;
	host("shot " SHOT "before_repeat_forcequit");
	host("down alt"); host("down meta_l"); host("down esc");
	pause_ms(1500L);
	host("up esc"); host("up meta_l"); host("up alt");
	n = waitshot(SHOT "repeat_forcequit_dialog", SHOT "before_repeat_forcequit", 1000, 1 << 30, 20);
	t_info("repeat_forcequit_publish", "published %ld refused %ld wrapper %lx previousword %ld",
	    getsym("uin_forcepublish"), getsym("uin_forcefailed"),
	    getsym("uin_forceentry"), getsym("uin_forceword"));
	if (!t_check("repeat_forcequit_dialog", n >= 1000, "%d pixels changed", n)) return;
	if (!t_check("repeat_forcequit_cancel_button", moveto(581, 212), "")) return;
	host("button 1"); host("button 0");
	n = waitshot(SHOT "repeat_forcequit_cancelled", SHOT "before_repeat_forcequit", 0, 1999, 20);
	t_check("repeat_forcequit_cancel_restored", n >= 0 && n < 2000, "%d pixels changed", n);
	t_check("repeat_forcequit_cancel_alive", alive76(), "");
}
#endif

#ifdef COFF76
static void
coffpm76(fd)
int fd;
{
    unsigned char b[16];char hex[33];long a,base,pc;int i,j,n;
    static int selectors[]={16,17,55,59};
    if(lseek(fd,(off_t)0x904,0)!=-1 && read(fd,(char *)b,4)==4) {
        a=((long)b[0]<<24)|((long)b[1]<<16)|((long)b[2]<<8)|b[3];
        t_info("carrier_currenta5","%lx",a);
    }
    if(lseek(fd,(off_t)0xb7c,0)==-1 || read(fd,(char *)b,4)!=4)return;
    a=((long)b[0]<<24)|((long)b[1]<<16)|((long)b[2]<<8)|b[3];
    t_info("carrier_systema5","%lx",a);
    if(a<0x11000L || !cofframrange(fd,a-3308,3308+60*8))return;
    base=a-3330;
    for(i=0;i<4;i++) {
        a=base+8*selectors[i];
        if(lseek(fd,(off_t)a,0)==-1 || read(fd,(char *)b,8)!=8)continue;
        pc=((long)b[0]<<24)|((long)b[1]<<16)|((long)b[2]<<8)|b[3];
        t_info("carrier_pm_selector","%d address %lx target %lx input %d output %d",
            selectors[i],a,pc,((int)b[4]<<8)|b[5],((int)b[6]<<8)|b[7]);
        if(pc<0x10000L || !cofframrange(fd,pc,16) || (pc&1) ||
            lseek(fd,(off_t)pc,0)==-1 || (n=read(fd,(char *)b,16))!=16)continue;
        for(j=0;j<n;j++)sprintf(hex+2*j,"%02x",b[j]);
        t_info("carrier_pm_code","%d %lx %s",selectors[i],pc,hex);
    }
}

static int
commandpid76()
{
    DIR *d; struct dirent *de; prpsinfo_t ps;
    char path[64]; int fd, pid=0;
    if (!(d=opendir("/proc"))) return 0;
    while ((de=readdir(d))) {
        if (de->d_name[0]=='.') continue;
        sprintf(path,"/proc/%s",de->d_name);
        if ((fd=open(path,O_RDONLY))<0) continue;
        if (ioctl(fd,PIOCPSINFO,&ps)==0 && !ps.pr_zomb &&
            !strcmp(ps.pr_fname,"CommandShell")) pid=ps.pr_pid;
        close(fd);
    }
    closedir(d); return pid;
}

static void
commandcarrier76()
{
    char cmd[32],buf[4097],path[40],*q="coffcarrier",*line,*end;
    char *pixels;
    long owner=0,child=0,reaped=0,stack=0; int i,fd,n,killed=0,live=0,io=0,done=0,quit=0,window=0,j,k,off; prpsinfo_t ps; prstatus_t st;
    unsigned char memory[32];char hex[65];long address;
    t_check("carrier_staged",access("/etc/aux/CoffProbe",F_OK)==0,"");
    if(access("/etc/aux/CoffProbe",F_OK)<0) return;
    t_rearm(90);
    t_check("coff_diag_armed",setsym("uin_coffdiag",1L)==0 && setsym("aux_filediag",2L)==0,"");
    unlink("/tmp/coffprobe.out"); unlink("/tmp/coffchild.out"); unlink("/tmp/coffio");
    host("shot " SHOT "carrier_desktop");
    host("shot " SHOT "carrier_before");
    sprintf(cmd,"move %d %d",scrw-30,scrh-30); host(cmd);
    host("down left");pause_ms(150L);host("up left");pause_ms(1200L);
    for(;*q;q++) {sprintf(cmd,"key %c",*q);host(cmd);}
    host("key meta_l+o");
    for(i=0;i<150 && !child;i++) {
        if((fd=open("/tmp/coffprobe.out",O_RDONLY))>=0) {
            n=read(fd,buf,sizeof buf-1);close(fd);
            if(n>0) {buf[n]=0;line=strstr(buf,"owner ");if(line) sscanf(line,"owner %ld",&owner);
                line=strstr(buf,"child ");if(line) sscanf(line,"child %ld",&child);}
        }
        if(!child) pause_ms(100L);
    }
    t_info("coff_carrier","owner %ld child %ld",owner,child);
    if(child>1 && owner>1) {
        pause_ms(10000L);
        host("shot " SHOT "carrier_child");
        n=0;pixels=host("cmp " SHOT "carrier_desktop " SHOT "carrier_child");
        if(pixels)sscanf(pixels,"diff %d",&n);
        window=n>10000;
        sprintf(path,"/proc/%ld",child);
        if((fd=open(path,O_RDONLY))>=0) {
            if(ioctl(fd,PIOCPSINFO,&ps)==0) {
                t_info("coff_carrier_child","pid %d parent %d command %s zombie %d",
                    (int)ps.pr_pid,(int)ps.pr_ppid,ps.pr_fname,ps.pr_zomb);
                if(ps.pr_pid==child && ps.pr_ppid==owner &&
                    !strcmp(ps.pr_fname,"CommandShell") && !ps.pr_zomb) {
                    live=1;
                    coffpm76(fd);
                    if(ioctl(fd,PIOCSTATUS,&st)==0) {
                        t_info("carrier_child_state","pid %d state %c wchan %lx PC %lx SP %lx instr %lx",
                            (int)ps.pr_pid,ps.pr_sname,(long)ps.pr_wchan,
                            (long)st.pr_reg[16],(long)st.pr_reg[15],st.pr_instr);
                        for(j=0;j<2;j++) {
                            address=(long)st.pr_reg[j?15:16];
                            if(lseek(fd,(off_t)address,0)!=-1 && (n=read(fd,(char *)memory,sizeof memory))>0) {
                                for(k=0;k<n;k++)sprintf(hex+2*k,"%02x",memory[k]);
                                t_info("carrier_child_memory","%lx %s",address,hex);
                                if(j==1) for(off=0;off+8<=n;off+=4) {
                                    if(memory[off]==0x20 && memory[off+6]==0 && memory[off+7]==0x28) {
                                        address=((long)memory[off+2]<<24)|((long)memory[off+3]<<16)|
                                            ((long)memory[off+4]<<8)|memory[off+5];
                                        if(address>0x10000L && cofframrange(fd,address,16) &&
                                            lseek(fd,(off_t)address,0)!=-1 && read(fd,(char *)memory,16)==16) {
                                            for(k=0;k<16;k++)sprintf(hex+2*k,"%02x",memory[k]);
                                            t_info("carrier_native_call","%lx %s",address,hex);
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    q="touch /tmp/coffio";
                    for(;*q;q++) {
                        sprintf(cmd,"key %s",*q==' '?"spc":*q=='/'?"slash":"");
                        if(*q!=' ' && *q!='/') sprintf(cmd,"key %c",*q);
                        host(cmd);
                    }
                    host("key ret");
                    for(i=0;i<100 && !io;i++) {io=access("/tmp/coffio",F_OK)==0;if(!io)pause_ms(100L);}
                    host("key meta_l+q");
                    for(i=0;i<100;i++) {
                        int qfd=open(path,O_RDONLY);prpsinfo_t ended;
                        if(qfd<0) {quit=1;break;}
                        n=ioctl(qfd,PIOCPSINFO,&ended);close(qfd);
                        if(n==0 && ended.pr_pid==child && ended.pr_ppid==owner &&
                            !strcmp(ended.pr_fname,"CommandShell")) {
                            if(ended.pr_zomb) {quit=1;break;}
                        } else break;
                        pause_ms(100L);
                    }
                    if(!quit) {
                        prpsinfo_t owned;
                        if(ioctl(fd,PIOCPSINFO,&owned)==0 && owned.pr_pid==child &&
                            owned.pr_ppid==owner && !strcmp(owned.pr_fname,"CommandShell") && !owned.pr_zomb)
                            killed=kill((pid_t)child,SIGKILL)==0;
                    }
                }
            }
            close(fd);
        }
    }
    t_check("carrier_window",live && window,"live child %d window change %d",live,window);
    t_check("carrier_child",live,"owner %ld child %ld",owner,child);
    t_check("carrier_original_io",io,"CommandShell must execute the typed command");
    t_check("carrier_normal_quit",quit,"Command-Q must exit the original Unix child");
    t_check("carrier_owned_cleanup",quit || killed,"");
    t_info("coff_carrier_cleanup","owned child killed %d",killed);
    pause_ms(3000L); host("shot " SHOT "carrier_after");
    if((fd=open("/tmp/coffprobe.out",O_RDONLY))>=0) {
        n=read(fd,buf,sizeof buf-1);close(fd);if(n>0) {
            buf[n]=0;coffparentstack(buf);
            for(line=buf;*line;line=end) {
                end=strchr(line,'\n');if(!end)break;*end++=0;
                t_info("coff_carrier_log","%s",line);
                if(!strncmp(line,"reaped ",7))sscanf(line+7,"%ld",&reaped);
                if(!strncmp(line,"fork_child_sp ",14))sscanf(line+14,"%ld",&stack);
                if(!strcmp(line,"done"))done=1;
            }

        }
    }
    if((fd=open("/tmp/coffchild.out",O_RDONLY))>=0) {
        n=read(fd,buf,sizeof buf-1);close(fd);if(n>0) {buf[n]=0;for(i=0;i<n;i+=128)t_info("coff_carrier_binary","%.*s",n-i<128 ? n-i : 128,buf+i);}
    }
    t_check("carrier_private_stack",stack==0x3fffff00L,"%ld",stack);
    t_check("carrier_reaped",child>1 && reaped==child && done,"child %ld reaped %ld done %d",child,reaped,done);
    t_check("carrier_finder",alive76(),"");
    t_info("coff_adapter_state","launch %ld front %ld exit %ld refused %ld",getsym("uin_cofflaunch"),getsym("uin_cofffront"),getsym("uin_coffexit"),getsym("uin_coffrefused"));
    t_info("coff_diag_state","seen %ld closed %ld old55 %ld",getsym("uin_coffseen"),getsym("uin_coffclosed"),getsym("uin_coffcalls"));
    setsym("uin_coffdiag",0L);setsym("aux_filediag",0L);
    unlink("/tmp/coffio");
}

static void
command76once()
{
    int i,n,pid=0,fd,owned=0,quit=0,killed=0,done=0,io=0;long reaped=0,status=-1;prpsinfo_t ps;
    char path[48],cmd[32],buf[4097],*line,*q="commandshell";
    t_rearm(120);
    t_check("coff_diag_armed",setsym("uin_coffdiag",1L)==0 && setsym("aux_filediag",2L)==0,"");
    if (!t_check("coff_initial",!commandpid76(),"CommandShell already running")) {
        t_info("coff_adapter_state","launch %ld front %ld exit %ld refused %ld",getsym("uin_cofflaunch"),getsym("uin_cofffront"),getsym("uin_coffexit"),getsym("uin_coffrefused"));
    t_info("coff_diag_state","seen %ld closed %ld old55 %ld",getsym("uin_coffseen"),getsym("uin_coffclosed"),getsym("uin_coffcalls"));
    setsym("aux_trace",0L);
    setsym("uin_coffdiag",0L);setsym("aux_filediag",0L);return;
    }
    sprintf(path,"/proc/%d",(int)macpid);
    if((fd=open(path,O_RDONLY))>=0) {coffcommtarget(fd);coffpm76(fd);close(fd);}
    unlink("/tmp/coffprobe.out");unlink("/tmp/coffchild.out");
    host("shot " SHOT "coff_desktop");
    t_check("coff_unix_trace",setsym("aux_trace",7L | 16L)==0,"");
    for (;*q;q++) { sprintf(cmd,"key %c",*q); host(cmd); }
    host("key meta_l+o");
    for (i=0;i<100 && !(pid=commandpid76());i++) pause_ms(100L);
    t_info("coff_pid","%d",pid);
    if(pid>1) {
        sprintf(path,"/proc/%d",pid);
        if((fd=open(path,O_RDONLY))>=0) {
            if(ioctl(fd,PIOCPSINFO,&ps)==0 && ps.pr_pid==pid && ps.pr_ppid==macpid &&
                !strcmp(ps.pr_fname,"CommandShell") && !ps.pr_zomb) {
                owned=1;coffpm76(fd);
            }
            close(fd);
        }
    }
    if (!t_check("coff_process",owned,"pid %d owner %ld",pid,(long)macpid)) {
        coffsnapshot(0);
        setsym("aux_trace",16L);
        host("shot " SHOT "coff_rejected");
        host("key ret");
        n=waitshot(SHOT "coff_failure_closed",SHOT "coff_desktop",0,1999,40);
        t_check("coff_failure_restored",n>=0 && n<2000,"%d pixels changed",n);
        t_info("coff_adapter_state","launch %ld front %ld exit %ld refused %ld",getsym("uin_cofflaunch"),getsym("uin_cofffront"),getsym("uin_coffexit"),getsym("uin_coffrefused"));
    t_info("coff_diag_state","seen %ld closed %ld old55 %ld",getsym("uin_coffseen"),getsym("uin_coffclosed"),getsym("uin_coffcalls"));
    setsym("aux_trace",0L);
    setsym("uin_coffdiag",0L);setsym("aux_filediag",0L);
        return;
    }
    setsym("aux_trace",16L);
    n=waitshot(SHOT "coff_open",SHOT "coff_desktop",10000,1 << 30,40);
    t_info("coff_window_pixels","%d pixels changed",n);
    coffsnapshot(pid);
    coffordinary(pid);
    setsym("aux_trace",16L);
    unlink("/tmp/coffio");
    q="touch /tmp/coffio";
    for(;*q;q++) {
        if(*q==' ') sprintf(cmd,"key spc");
        else if(*q=='/') sprintf(cmd,"key slash");
        else sprintf(cmd,"key %c",*q);
        host(cmd);
    }
    host("key ret");
    for(i=0;i<100 && access("/tmp/coffio",F_OK)!=0;i++) pause_ms(100L);
    io=access("/tmp/coffio",F_OK)==0;
    t_check("coff_visible_output",coffvisible(pid),"terminal must render command output");
    t_check("coff_window",n>=10000 && io,"%d pixels changed; typed command %d",n,io);
    t_check("coff_original_io",io,
        "CommandShell must execute the typed command");
    unlink("/tmp/coffio");
    if(!io) {host("key ret");pause_ms(500L);}
    host("key meta_l+q");
    for (i=0;i<100 && commandpid76();i++) pause_ms(100L);
    quit=!commandpid76();
    t_check("coff_quit",io && quit,"CommandShell still running");
    if(!quit) {host("shot " SHOT "coff_quit_pending");coffsnapshot(pid);}
    if(!quit && (fd=open(path,O_RDONLY))>=0) {
        if(ioctl(fd,PIOCPSINFO,&ps)==0 && ps.pr_pid==pid && ps.pr_ppid==macpid &&
            !strcmp(ps.pr_fname,"CommandShell") && !ps.pr_zomb) killed=kill(pid,SIGKILL)==0;
        close(fd);pause_ms(1000L);
    }
    t_info("coff_owned_cleanup","fallback killed %d",killed);
    if((fd=open("/tmp/coffprobe.out",O_RDONLY))>=0) {
        n=read(fd,buf,sizeof buf-1);close(fd);
        if(n>0) {
            buf[n]=0;coffparentstack(buf);line=strstr(buf,"reaped ");if(line)sscanf(line,"reaped %ld",&reaped);
            line=strstr(buf,"status ");if(line)sscanf(line,"status %ld",&status);
            done=strstr(buf,"done\n")!=0;
            for(i=0;i<n;i+=128)t_info("coff_native_owner","%.*s",n-i<128?n-i:128,buf+i);
        }
    }
    t_check("coff_native_reaped",reaped==pid && done,"pid %d reaped %ld done %d",pid,reaped,done);
    t_check("coff_normal_status",io && quit && reaped==pid && done && status==0,"quit %d Unix status %ld",quit,status);
    n=waitshot(SHOT "coff_closed",SHOT "coff_desktop",0,1999,40);
    t_check("coff_desktop",n>=0 && n<2000,"%d pixels changed",n);
    t_check("coff_finder_alive",alive76(),"");
    t_info("coff_adapter_state","launch %ld front %ld exit %ld refused %ld",getsym("uin_cofflaunch"),getsym("uin_cofffront"),getsym("uin_coffexit"),getsym("uin_coffrefused"));
    t_info("coff_diag_state","seen %ld closed %ld old55 %ld",getsym("uin_coffseen"),getsym("uin_coffclosed"),getsym("uin_coffcalls"));
    setsym("aux_trace",0L);
    setsym("uin_coffdiag",0L);setsym("aux_filediag",0L);
}

static void
command76()
{
    char *opt=getenv("COFFNATIVE47");
    long saved=getsym("uinter_coffnative47");
    int i,native47=opt ? !strcmp(opt,"1") : saved;
    t_check("coff_native47_mode",setsym("uinter_coffnative47",native47)==0,"");
    for(i=0;i<2;i++) {
        t_info("coff_launch_iteration","%d",i+1);
        command76once();
    }
    setsym("uinter_coffnative47",saved);
}

#endif

/* About closed, SimpleText opened from the desktop and quit, the disk window */
static void
app76()
{
	char *r;
	int n, chk = -1;

	t_rearm(120);
	host("key meta_l+w");
	n = waitshot(SHOT "closed", SHOT "desktop", 0, 1999, 20);
	t_check("about_closed", n >= 0 && n < 2000, "%d pixels from the desktop", n);
#ifdef COFF76
#ifdef COFF76C
    commandcarrier76();
#else
    command76();
#endif
    shutdown76();
    return;
#endif
#ifdef COFF81
#ifdef COFF81D
    commanddirect81();
#else
    commandcarrier81();
#endif
    shutdown76();
    return;
#endif
#ifdef OT81
	if (access("/mac/lib/OTFastProbe", R_OK) == 0) {
		ot81probe("network");
		t_check("finder_stays", alive76(), "");
		shutdown76();
		return;
	}
#endif
#if defined(SYS81) && !defined(OT81) && defined(MAC81FAST)
    if (access("/mac/lib/MacFastProbe", R_OK) == 0) {
        panel81("Appearance", "appearance");
        panel81("Monitors", "monitors");
        recover81();
        shutdown76();
        return;
    }
#endif
	/* typing selects the desktop icon by name; Command-O opens it */
	t_rearm(120);
	host("key s");
	pause_ms(1000L);
	host("key meta_l+o");
	n = waitshot(SHOT "simpletext", SHOT "closed", 100000, 1 << 30, 60);
	/* its untitled window, white over the middle of the desktop */
	if ((r = host("mac " SHOT "simpletext 200 150 600 450")) != 0)
		sscanf(r, "checker %d", &chk);
	t_check("simpletext_open", n >= 100000 && chk >= 0 && chk < 100,
	    "%d pixels changed, checker %d per mille", n, chk);
	t_check("simpletext_alive", alive76(), "");
#ifdef SYS81
	t_rearm(240);
	host("key meta_l+o");
	n = waitshot(SHOT "open_dialog", SHOT "simpletext", 10000, 1 << 30, 20);
	t_check("standard_open_dialog", n >= 10000, "%d pixels changed", n);
	if (n >= 10000) host("key esc");
	n = waitshot(SHOT "open_cancelled", SHOT "simpletext", 0, 1999, 20);
	t_check("standard_open_cancel", n >= 0 && n < 2000, "%d pixels changed", n);
	host("key m"); host("key a"); host("key c"); host("key 8"); host("key 1");
	pause_ms(400L); host("shot " SHOT "typed_text");
	host("key meta_l+s");
	n = waitshot(SHOT "save_dialog", SHOT "typed_text", 10000, 1 << 30, 20);
	t_check("standard_save_dialog", n >= 10000, "%d pixels changed", n);
	if (n >= 10000) host("key esc");
	n = waitshot(SHOT "save_cancelled", SHOT "typed_text", 0, 1999, 20);
	t_check("standard_save_cancel", n >= 0 && n < 2000, "%d pixels changed", n);
	unlink("/Desktop Folder/mac81dialog");
	unlink("/Desktop Folder/%mac81dialog");
	host("key meta_l+s");
	n = waitshot(SHOT "save_again", SHOT "typed_text", 10000, 1 << 30, 20);
	if (n >= 10000) {
		host("key meta_l+d"); pause_ms(400L);
		host("key meta_l+a");
		host("key m"); host("key a"); host("key c"); host("key 8"); host("key 1");
		host("key d"); host("key i"); host("key a"); host("key l"); host("key o"); host("key g");
		host("key ret");
	}
	n = waitshot(SHOT "saved_text", SHOT "typed_text", 0, 9999, 20);
	{
		char data[8]; int f, bytes = -1;
		f = open("/Desktop Folder/mac81dialog", O_RDONLY);
		if (f >= 0) { bytes = read(f, data, sizeof data); close(f); }
		t_check("standard_save_data", bytes == 5 && !memcmp(data, "mac81", 5),
		    "%d bytes", bytes);
	}

#endif
	t_rearm(120);
	host("key meta_l+q");
#ifdef SYS81
	n = waitshot(SHOT "text_closed", SHOT "simpletext", 100000, 1 << 30, 20);
	if (n < 100000) {
		host("key meta_l+d");
		n = waitshot(SHOT "text_discarded", SHOT "simpletext", 100000, 1 << 30, 20);
	}
	if (n >= 100000) {
		unlink("/Desktop Folder/mac81dialog");
		unlink("/Desktop Folder/%mac81dialog");
	}
#endif
	n = waitshot(SHOT "quit", SHOT "closed", 0, 1999, 40);
	t_check("simpletext_quit", n >= 0 && n < 2000, "%d pixels from the desktop", n);
	t_check("finder_alive", alive76(), "");
	/* the startup disk's window: the File Manager lists the root */
	t_rearm(120);
	moveto(scrw - 40, 48);
	host("click 2");
	n = waitshot(SHOT "disk", SHOT "quit", 20000, 1 << 30, 40);
	t_check("disk_window", n >= 20000, "%d pixels changed", n);
	t_rearm(120);
	host("key meta_l+w");
	n = waitshot(SHOT "diskclosed", SHOT "quit", 0, 1999, 20);
	t_check("disk_window_closed", n >= 0 && n < 2000, "%d pixels from the desktop", n);
#ifdef SYS81
#ifdef OT81
	ot81probe("network");
#else
	command81();
	panel81("appearance","appearance");
	panel81("monitors","monitors");
	recover81();
#endif
#endif
#ifdef SYS81
    t_check("finder_idle_button_armed",setsym("uin_evbuttoncount",0L)==0 &&
        setsym("uin_evdiag",1L)==0,"");
#endif
	/* the Special menu held open */
	moveto(SPECIALH, 9);
	host("button 1");
	pause_ms(800L);
	host("shot " SHOT "special");
	host("button 0");
	pause_ms(1000L);
	n = shotdiff(SHOT "diskclosed", SHOT "special");
	t_check("special_menu", n > 1000, "%d pixels changed", n);
#ifdef SYS81
    host("key esc");
    host("button 0");
    t_check("finder_idle_cursor_desktop",moveto(scrw/4,scrh/2),"");
    host("click 1");
    n=waitshot(SHOT "idle_ready",SHOT "diskclosed",0,1999,20);
    t_check("finder_idle_desktop_ready",n>=0 && n<2000,
        "%d pixels from the closed desktop",n);
    n=shotdiff(SHOT "special",SHOT "idle_ready");
    t_check("finder_idle_menu_dismissed",n>1000,"%d pixels from the open menu",n);
	/* two idle minutes: still running, the desktop unchanged */
	t_rearm(240);
	{
		long before, after, stats[8], value, zero=0; int j, armed=1, readok=1;
		unsigned long base=0, lastbase=0, info;
		if (getksym("uin_evstats",&base,&info)<0 || !base) armed=0;
		for (j=0;armed && j<8;j++)
			if (kmemrw(base+j*4,&zero,1)<0) armed=0;
		setsym("uin_evtracecount",0L);
		t_check("finder_idle_events_armed",armed && setsym("uin_evdiag",1L)==0,"");
		t_check("finder_idle_wne_armed",idlewne81(1),"");
        pause_ms(1000L);
        t_check("finder_idle_wne_read",idlewne81(0),"");
		t_check("finder_idle_phase_armed",setsym("uin_pmphaseon",0L)==0 &&
            setsym("uin_pmphasecount",0L)==0 && setsym("uin_pmidletracecount",0L)==0 &&
            setsym("uin_pmphaseon",1L)==0,"");
        pause_ms(1000L);
        {unsigned long addr=0,phaseinfo=0;long words[16],count;int row,k,ok;
         ok=setsym("uin_pmphaseon",0L)==0;count=getsym("uin_pmphasecount");
         if(count<0 || count>8 || getksym("uin_pmphase",&addr,&phaseinfo)<0 || !addr)ok=0;
         else for(row=0;row<count;row++) {
             for(k=0;k<16;k++){words[k]=-1;if(kmemrw(addr+row*64+k*4,words+k,0)<0)ok=0;}
             t_info("finder_idle_pm_phase","row %d previouswake %lx entry %lx wake %lx nativepc %lx fp %lx caller %lx sp %lx a5 %lx signal %ld/%ld pending %lx flags %lx",
                 row,words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7],words[8],words[9],words[10],words[11]);
             t_info("finder_idle_pm_clock","row %d nativeTicks %lx/%lx virtualSR %lx held %lx",row,words[12],words[13],words[14],words[15]);
         }
         addr=0;phaseinfo=0;count=getsym("uin_pmphasecount");
         if(count<0 || count>8 || getksym("uin_pmwatch",&addr,&phaseinfo)<0 || !addr)ok=0;
         else for(row=0;row<count;row++) {
             for(k=0;k<8;k++){words[k]=-1;if(kmemrw(addr+row*32+k*4,words+k,0)<0)ok=0;}
             t_info("finder_idle_pm_watch","row %d macflags %lx gpflags %lx missing %lx pending %ld forceflags %lx coffdiag %ld watched %ld skipped %ld",
                 row,words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7]);
         }
         addr=0;phaseinfo=0;count=getsym("uin_pmidletracecount");
         if(count<0 || count>8 || getksym("uin_pmidletrace",&addr,&phaseinfo)<0 || !addr)ok=0;
         else for(row=0;row<count;row++) {
             for(k=0;k<12;k++){words[k]=-1;if(kmemrw(addr+row*48+k*4,words+k,0)<0)ok=0;}
             t_info("finder_idle_pm_wait","row %d hosttick %lx/%lx result %ld signal %ld/%ld pending %lx/%lx held %lx/%lx pid %ld pc %lx queue %ld",
                 row,words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7],words[8],words[9],words[10],words[11]);
         }
         t_check("finder_idle_phase_read",ok,"");
        }
		t_check("finder_idle_pm_armed",idlepm81(1),"");
        t_info("finder_idle_pm_before","calls %ld sleeps %ld interrupts %ld ticks %ld zero %ld refused %ld",
            getsym("uin_pmidlecalls"),getsym("uin_pmidlesleeps"),getsym("uin_pmidleinterrupts"),
            getsym("uin_pmidleticks"),getsym("uin_pmidlezero"),getsym("uin_pmidlerefused"));
		before=mac_cpu();
		host("prof 10 mac81_idle");
		pause_ms(120000L);
		after = mac_cpu();
        t_info("finder_idle_pm_after","calls %ld sleeps %ld interrupts %ld ticks %ld zero %ld refused %ld",
            getsym("uin_pmidlecalls"),getsym("uin_pmidlesleeps"),getsym("uin_pmidleinterrupts"),
            getsym("uin_pmidleticks"),getsym("uin_pmidlezero"),getsym("uin_pmidlerefused"));
		t_check("finder_idle_pm_read",idlepm81(0),"");
		setsym("uin_evdiag",0L);
		for (j=0;j<8;j++) {
			stats[j]=-1;
			if (!base || kmemrw(base+j*4,&stats[j],0)<0) readok=0;
		}
		t_info("finder_idle_events","calls %ld force %ld event %ld nonblock %ld expired %ld mouse %ld sleeps %ld signal %ld",stats[0],stats[1],stats[2],stats[3],stats[4],stats[5],stats[6],stats[7]);
        {
            unsigned long buttons=0;long words[10],count=getsym("uin_evbuttoncount");int k,row;
            if(getksym("uin_evbuttons",&buttons,&info)<0 || !buttons || count<0 || count>8)readok=0;
            else for(row=0;row<count;row++) {
                for(k=0;k<10;k++) {
                    words[k]=-1;
                    if(kmemrw(buttons+row*40+k*4,&words[k],0)<0)readok=0;
                }
                t_info("finder_idle_button","row %d kind %ld what %ld flags %lx evmask %lx q %ld tick %lx pc %lx mask %lx btn %ld written %ld",
                    row,words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7],words[8],words[9]);
            }
        }
		if (getksym("uin_evlast",&lastbase,&info)<0 || !lastbase) readok=0;
		for (j=0;j<8;j++) {
			value=-1;
			if (!lastbase || kmemrw(lastbase+j*4,&value,0)<0) readok=0;
			t_info("finder_idle_event_last","field %d value %lx",j,value);
		}
		t_check("finder_idle_events_read",readok,"");
		t_check("finder_idle_cpu_read", before >= 0 && after >= before,
		    "%ld -> %ld ms", before, after);
		if (before >= 0 && after >= before) {
			t_info("finder_idle_cpu", "%ld ms over 120000 ms", after - before);
			t_check("finder_idle_cpu_bounded",after-before<=24000L,
			    "%ld ms over 120000 ms (maximum 20%%)",after-before);
		}
	}
	host("shot " SHOT "idle");
	n = shotdiff(SHOT "diskclosed", SHOT "idle");
	t_check("finder_idle", n >= 0 && n < 2000, "%d pixels from the desktop", n);
#endif
	t_check("finder_stays", alive76(), "");
	shutdown76();
#ifdef SYS81
    if((!getenv("MACPMSTUB") && !getenv("MACPMIDLE")) ||
        (getenv("MACPMSTUB") && !strcmp(getenv("MACPMSTUB"),"1")))
        t_check("finder_idle_stub_restored",idlestub81(2),"");
#endif
}

#ifdef SYS81
/* n bytes of the Mac task's memory at addr */
static int
rdmac(fd, addr, b, n)
int fd;
long addr;
char *b;
int n;
{
	return lseek(fd, addr, 0) != -1 && read(fd, b, n) == n;
}

static long
g32(c)
unsigned char *c;
{
	return (long)c[0] << 24 | (long)c[1] << 16 | c[2] << 8 | c[3];
}

/*
 * A heap zone (z, or SysZone and the low-memory layout if 0): its header,
 * then the first block whose size leaves the zone or whose master
 * pointer does not point back, with the two blocks before it.
 */
static long
heap81(path, z)
char *path;
long z;
{
	unsigned char c[64], lm[4];
	char buf[400];
	long b, lim, sz, prev[2], mp, tgt = 0;
	int fd, i, n = 0, nb = 0, nbad = 0;
	static long lmaddr[] = { 0x2a6, 0x2aa, 0x10c, 0x108, 0x130, 0x114, 0x118, 0x2a2, 0x1ef0 };

	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	if (z == 0) {
		for (i = 0; i < 9; i++)
			if (rdmac(fd, lmaddr[i], (char *)lm, 4))
				n += sprintf(buf + n, " %lx=%lx", lmaddr[i], g32(lm));
		t_info("mac_lowmem", "SysZone ApplZone BufPtr MemTop ApplLimit HeapEnd TheZone ROMBase -:%s", buf);
		if (!rdmac(fd, 0x2a6L, (char *)lm, 4))
			{ tgt = -1; goto out; }
		z = g32(lm);
	}
	if (!rdmac(fd, z, (char *)c, 52))
		{ tgt = -1; goto out; }
	for (n = 0, i = 0; i < 52; i++)
		n += sprintf(buf + n, "%02x", c[i]);
	t_info("mac_zone", "%lx: %s", z, buf);
	lim = g32(c);
	prev[0] = prev[1] = 0;
	for (b = z + 52; b < lim && nb < 200000; b += sz, nb++) {
		if (!rdmac(fd, b, (char *)c, 16)) {
			tgt = -1;
			goto out;
		}
		sz = g32(c + 4);
		mp = -1;
		if (c[0] & 0x80 && rdmac(fd, z + g32(c + 8), (char *)lm, 4))
			mp = g32(lm);
		if (tgt && b <= tgt && tgt < b + sz) {
			for (n = 0, i = 0; i < 16; i++)
				n += sprintf(buf + n, "%02x", c[i]);
			t_info("mac_mpblk", "%lx in #%d at %lx: %s", tgt, nb, b, buf);
			if (rdmac(fd, tgt - 32, (char *)c, 64)) {
				for (n = 0, i = 0; i < 64; i++)
					n += sprintf(buf + n, "%s%02x", i == 32 ? " | " : "", c[i]);
				t_info("mac_mparea", "%s", buf);
			}
			if (!rdmac(fd, b, (char *)c, 16))
				break;
		}
		if (sz >= 12 && !(sz & 3) && b + sz <= lim && (c[0] & 0x80) && mp != b + 12 &&
		    nbad++ < 4) {
			t_info("mac_badmp", "#%d at %lx: %08lx %08lx %08lx mp at %lx = %lx",
			    nb, b, g32(c), sz, g32(c + 8), z + g32(c + 8), mp);
			if (!tgt)
				tgt = z + g32(c + 8);
		}
		if (sz < 12 || (sz & 3) || b + sz > lim || (c[0] & 0xc0) == 0xc0) {
			t_info("mac_badblk", "#%d at %lx: %08lx %08lx %08lx mp %lx; before %lx %lx",
			    nb, b, g32(c), sz, g32(c + 8), mp, prev[0], prev[1]);
			for (i = 0; i < 2; i++)
				if (prev[i] && rdmac(fd, prev[i], (char *)c, 48)) {
					int k, m = 0;

					for (k = 0; k < 48; k++)
						m += sprintf(buf + m, "%02x", c[k]);
					t_info("mac_prevblk", "%lx: %s", prev[i], buf);
				}
			if (!tgt)
				tgt = b;
			goto out;
		}
		prev[1] = prev[0];
		prev[0] = b;
	}
	if (b != lim && !tgt)
		tgt = -1;
	t_info("mac_heapok", "%lx: %d blocks to %lx (limit %lx)", z, nb, b, lim);
out:
	close(fd);
	return tgt;
}

#endif

/* the 7.6.1 Finder: its desktop, the System version, About This Computer */
static void
screen76()
{
	unsigned char *lm;
	char *r;
	int id, n, chk = 0, white = 0, top = 0, diff = 0;
    int tilex = 0, tiley = 0, dominant = 1000;

	hostopen();
	if (hfd < 0) {
		t_skip("mac_screen", "no host line");
		return;
	}
	/* Wait for the Finder menu bar to appear. */
	for (n = 0; n < 8; n++) {
		t_rearm(120);
		host("shot " SHOT "desktop");
		r = host("mac " SHOT "desktop");
		tilex = tiley = 0; dominant = 1000;
		if (r)
			sscanf(r, "checker %d white %d top %d tilex %d tiley %d dominant %d",
                &chk, &white, &top, &tilex, &tiley, &dominant);
#if defined(SYS81) && !defined(COFF81)
		if (DESKOK(chk, top, tilex, tiley, dominant))
#elif defined(COFF81)
		if (top >= 20 && top < 300)
#elif defined(COFF76)
		if (top >= 900)
#else
		if (DESKOK(chk, top, tilex, tiley, dominant))
#endif
			break;
		pause_ms(10000L);
	}
	t_info("mac_screen_stats", "checker %d white %d top %d tile %d/%d dominant %d",
        chk, white, top, tilex, tiley, dominant);
#ifdef SYS81
    if(top < 20 || top >= 300) {
        char path[32],hex[65]; unsigned char bytes[32]; long pc=getsym("guest_f7pc"); int fd,k;
        t_info("startup_fault","count %ld PC %lx EA %lx",getsym("guest_nfault7"),pc,getsym("guest_f7ea"));
        sprintf(path,"/proc/%d",(int)macpid);
        if(pc>0 && (fd=open(path,O_RDONLY))>=0) {
            if(lseek(fd,(off_t)pc,0)!=-1 && read(fd,(char *)bytes,32)==32) {
                for(k=0;k<32;k++)sprintf(hex+2*k,"%02x",bytes[k]);
                t_info("startup_fault_code","%lx %s",pc,hex);
            }
            close(fd);
        }
    }
#endif

#if defined(SYS81) && !defined(COFF81)
	/* the Platinum menu bar is grey, with white only in its highlights */
	t_check("finder_desktop", DESKOK(chk, top, tilex, tiley, dominant),
	    "checker %d, menu %d, tile %d/%d, dominant %d",
        chk, top, tilex, tiley, dominant);
#elif defined(COFF81)
	t_check("finder_menu", top >= 20 && top < 300,
	    "menu bar white %d per mille", top);
#elif defined(COFF76)
	t_check("finder_menu", top >= 900, "menu bar %d per mille", top);
#else
	t_check("finder_desktop", DESKOK(chk, top, tilex, tiley, dominant),
	    "checker %d, menu %d, tile %d/%d, dominant %d",
        chk, top, tilex, tiley, dominant);
#endif
	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (lm && lm != (unsigned char *)-1) {
		t_check("sysversion", (lm[0x15a] << 8 | lm[0x15b]) == SYSVER, "SysVersion %#x",
		    lm[0x15a] << 8 | lm[0x15b]);
		t_check("gestalt_mach", patchbox() == 29, "Mac model %ld (BoxFlag %d)",
		    patchbox(), lm[0xcb3]);
		shmdt((char *)lm);
	}
	/* About This Computer, the Apple menu's first item */
	if (t_check("apple_menu", moveto(20, 9), "mouse %#lx", getsym("uin_mouse"))) {
		host("button 1");
		pause_ms(800L);
		moveto(60, 29);
		pause_ms(500L);
		host("button 0");
		pause_ms(5000L);
		host("shot " SHOT "about");
		if ((r = host("cmp " SHOT "desktop " SHOT "about")) != 0)
			sscanf(r, "diff %d", &diff);
		t_check("about_window", diff > 20000, "%d pixels changed", diff);
		app76();
	}
	close(hfd);
	hfd = -1;
}
#endif

#ifdef SYS6
#define	MAC6RAN	"/tmp/.t_mac6"	/* a run in this boot */

/* System 6's Finder: the desktop pattern under a white menu bar, SysVersion */
static void
screen6()
{
	struct stat sb;
	unsigned char *lm;
	char *r;
	int id, n, chk = 0, white = 0, top = 0;

	hostopen();
	if (hfd < 0) {
		t_skip("mac_screen", "no host line");
		return;
	}
	for (n = 0; n < 8; n++) {
		t_rearm(120);
		host("shot " SHOT "desktop");
		r = host("mac " SHOT "desktop");
		if (r)
			sscanf(r, "checker %d white %d top %d", &chk, &white, &top);
		if (chk >= 900 && top >= 900)
			break;
		pause_ms(10000L);
	}
	t_info("mac_screen_stats", "checker %d white %d top %d", chk, white, top);
	/* the Finder's menu bar over its desktop, no alert */
	if (!t_check("finder_desktop", chk >= 900 && top >= 900, "checker %d, menu bar %d per mille",
	    chk, top)) {
		close(hfd);
		hfd = -1;
		return;
	}
	id = shmget(TLOW, 0, 0);
	lm = id < 0 ? 0 : (unsigned char *)shmat(id, (char *)0, SHM_RDONLY);
	if (lm && lm != (unsigned char *)-1) {
		t_check("sysversion", (lm[0x15a] << 8 | lm[0x15b]) == SYSVER, "SysVersion %#x",
		    lm[0x15a] << 8 | lm[0x15b]);
		shmdt((char *)lm);
	} else
		t_check("sysversion", 0, "no low memory");
	/* the Apple and Special menus held open */
	if (t_check("mouse", moveto(20, 9), "mouse %#lx", getsym("uin_mouse"))) {
		host("button 1");
		pause_ms(800L);
		host("shot " SHOT "apple");
		host("button 0");
		n = shotdiff(SHOT "desktop", SHOT "apple");
		t_check("apple_menu", n > 1000, "%d pixels changed", n);
	}
	pause_ms(1000L);
	moveto(184, 9);
	host("button 1");
	pause_ms(800L);
	host("shot " SHOT "special");
	host("button 0");
	n = shotdiff(SHOT "desktop", SHOT "special");
	t_check("special_menu", n > 1000, "%d pixels changed", n);
	/* the startup disk's window, opened from the middle of its icon and closed */
	t_rearm(120);
	pause_ms(1000L);
	moveto(scrw - 40, 51);
	host("click 2");
	n = waitshot(SHOT "disk", SHOT "desktop", 20000, 1 << 30, 40);
	t_check("disk_window", n >= 20000, "%d pixels changed", n);
	host("key meta_l+w");
	n = waitshot(SHOT "closed", SHOT "desktop", 0, 1999, 20);
	t_check("disk_window_closed", n >= 0 && n < 2000, "%d pixels from the desktop", n);
	t_check("finder_alive", alive76(), "");
	/*
	 * Logout runs the shutdown procedures, which close the Desktop
	 * Manager's files; a killed session leaves them damaged for the next
	 */
	t_rearm(120);
	if (stat(MAC6RAN, &sb) < 0) {
		close(creat(MAC6RAN, 0644));
		menuitem(184, 9, 189, 155, SHOT "logout");
	} else
		menuitem(20, 9, 60, 218, SHOT "applelogout");
	loggedout("logout");
	close(hfd);
	hfd = -1;
}

/* the System Folder's names with spaces, which the test root has without */
static char *sys6sp[] = { "%AUX Resources", "DA Handler", "Key Layout", "Scrapbook File", 0 };

static void
sys6links(on)
int on;
{
	char a[80], b[80], *s, *d;
	int i;

	for (i = 0; sys6sp[i]; i++) {
		sprintf(b, "%s/%s", SYS6DIR, sys6sp[i]);
		for (s = sys6sp[i], d = a + sprintf(a, "%s/", SYS6DIR); *s; s++)
			if (*s != ' ')
				*d++ = *s;
		*d = 0;
		if (on)
			link(a, b);
		else
			unlink(b);
	}
}
#endif

int
main(argc, argv)
int argc;
char **argv;
{
	struct mod_mreg reg;
	struct mod_execreg er;
	struct stat sb;
	int e, mj = UIMAJ;

#if defined(SYS76) || defined(SYS6)
	t_init(TNAME, 600);
#else
	t_init("mac", 300);
#endif
	if (t_kmem("guest_loading") == -1) {
		t_skip("all", "kernel has no guest support");
		return t_done();
	}
	if (stat(MD "/uinter", &sb) < 0 || stat("/dev/uinter0", &sb) < 0) {
		t_skip("all", "no uinter module on this root");
		return t_done();
	}
#ifdef OT81
	if (access("/mac/lib/OTProvProbe", R_OK) == 0) {
		int bmj = 55;
		strcpy(reg.md_modname, "otbridge");
		reg.md_typedata = (caddr_t)&bmj;
		if (!t_check("provider_register", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0, "%s", T_ERR) ||
		    !t_check("provider_load", modload("/tests/otb/otbridge") >= 0, "%s", T_ERR))
			return t_done();
	}
#endif
#ifdef SYS81
    {
        struct rlimit rl;int ok=getrlimit(RLIMIT_VMEM,&rl)==0;
        if(ok && rl.rlim_cur<48L*1024*1024) {
            if(rl.rlim_max<48L*1024*1024)ok=0;
            else {rl.rlim_cur=48L*1024*1024;ok=setrlimit(RLIMIT_VMEM,&rl)==0;}
        }
        if(!t_check("session_memory_limit",ok,"32 MB Mac RAM plus 16 MB process headroom"))return t_done();
    }
#endif
	e = modpath(MD) < 0 ? errno : 0;
	strcpy(reg.md_modname, "auxexec");
	reg.md_typedata = (caddr_t)&er;
	er.er_magic = 0x150;
	er.er_flags = EXF_FIRST;
	if (!t_check("register_exec", e == 0 && modadm(MOD_TY_EXEC, MOD_C_MREG, &reg) == 0,
	    "modpath %d, modadm: %s", e, T_ERR))
		return t_done();
	strcpy(reg.md_modname, "uinter");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0,
	    "modadm: %s", T_ERR))
		return t_done();
#if !defined(SYS76) && !defined(SYS6)
	native();
	e = haverom();
	if (stat("/aux/bin/macabi", &sb) < 0)
		t_skip("macabi", "no macabi on this root");
	else if (!e)
		t_skip("macabi", "no Mac ROM: no /etc/aux/rom, none at ROMBase");
	else
		macabi();
	if (stat("/aux/bin/macjoin", &sb) < 0)
		t_skip("macjoin", "no macjoin on this root");
	else if (!e)
		t_skip("macjoin", "no Mac ROM: no /etc/aux/rom, none at ROMBase");
	else
		macjoin();
#endif
#ifdef SYS76
	/* mounted for the rest of the boot */
	macvol();
#endif
#ifdef SYS6
	if (stat(STARTMAC, &sb) < 0 || stat(ROOT6 "/etc/aux/rom", &sb) < 0 ||
	    stat(SYSDIR "/System", &sb) < 0)
#else
	if (stat(STARTMAC, &sb) < 0 || stat("/etc/aux/rom", &sb) < 0 ||
	    stat(SYSDIR "/System", &sb) < 0)
#endif
		t_skip("startmac", "no startmac, ROM or System file on this root");
	else {
#ifdef SYS81
  t_check("full_template", access(SYSDIR "/.stamp", R_OK) == 0 &&
      access(USERSYS "/.stamp", R_OK) == 0, "8.1 qualification requires full System Folders");
#endif
		fidd();
		deskfolder();
#ifdef SYS6
		/* its own ROM: the IIci's box flag loads it again */
		e = open("/dev/uinter0", O_RDWR);
		t_check("boxflag", setsym("uinter_boxflag", 5L) == 0, "no uinter_boxflag");
		t_check("sysfolder", rename(SYSDIR, SYS6DIR) == 0, "%s", T_ERR);
		sys6links(1);
		startmac();
		sys6links(0);
		rename(SYS6DIR, SYSDIR);
		setsym("uinter_boxflag", -1L);
		if (e >= 0)
			close(e);
#else
#ifdef SYS76
		/* the module loaded, then a Quadra 800 (no ProductInfo of its own) */
		e = open("/dev/uinter0", O_RDWR);
		t_check("boxflag", setsym("uinter_boxflag", 29L) == 0, "no uinter_boxflag");
#ifdef SYS81
        t_check("force_diag_armed", setsym("uin_polldiag",1L)==0 &&
            getsym("uin_polldiag")==1L && setsym("aux_polldiag",1L)==0,
            "Force diagnostics before startup");
#endif
		system("/usr/bin/rm -rf /tmp/.mac");
#ifndef SYS81
		/* the run's last stage: a root session that shuts down */
		if (argc > 1 && strcmp(argv[1], "halt") == 0) {
			session76(0, SYSDIR, "halt");
			return t_done();
		}
#endif
	#if !defined(COFF76) && !defined(COFF81D) && !defined(OT81)
	session76(atoi(OTHERUID), USERSYS, "user");
		dotmac("user_dotmac", atoi(OTHERUID), atoi(OTHERUID) + 1);
		session76(0, SYSDIR, "root");
#endif
		/* made by _AUXDispatch(36, 0); t_mac's run leaves one */
		system("/usr/bin/rm -rf /tmp/.mac");
		/* SimpleText on the desktop, for the Finder to open */
		copyf(SYSDIR "/SimpleText", "/Desktop Folder/SimpleText");
		copyf(SYSDIR "/%SimpleText", "/Desktop Folder/%SimpleText");
#ifdef SYS81
		/* this session's patches only */
		setsym("uinter_patched", 0L);
        setsym("uinter_soundnative", 0L);
#endif
#ifdef COFF76
        copyf("/mac/bin/CommandShell", "/Desktop Folder/CommandShell");
        copyf("/mac/bin/%CommandShell", "/Desktop Folder/%CommandShell");
        if(access("/etc/aux/CoffProbe",F_OK)==0)
            copyf("/mac/lib/COFFCarrier/COFFCarrier", "/Desktop Folder/COFFCarrier");
#endif
#ifdef SYS81
        copyf("/mac/bin/CommandShell", "/Desktop Folder/CommandShell");
        copyf("/mac/bin/%CommandShell", "/Desktop Folder/%CommandShell");
        copyf(SYSDIR "/Control Panels/Appearance", "/Desktop Folder/Appearance");
        copyf(SYSDIR "/Control Panels/%Appearance", "/Desktop Folder/%Appearance");
        copyf(SYSDIR "/Control Panels/Monitors", "/Desktop Folder/Monitors");
        copyf(SYSDIR "/Control Panels/%Monitors", "/Desktop Folder/%Monitors");
        if(access("/etc/aux/CoffProbe",F_OK)==0)
            copyf("/mac/lib/COFFCarrier/COFFCarrier", "/Desktop Folder/COFFCarrier");
		copyf("/mac/lib/Mac81Probe/Mac81Probe", "/Desktop Folder/Mac81Probe");
#ifdef OT81
		copyf("/mac/lib/OTProbe/OTProbe", "/Desktop Folder/OTProbe");
#endif
#endif
		startmac();
#ifdef COFF76
        unlink("/Desktop Folder/CommandShell"); unlink("/Desktop Folder/%CommandShell");
        unlink("/Desktop Folder/COFFCarrier");
#endif
#ifdef SYS81
        unlink("/Desktop Folder/CommandShell"); unlink("/Desktop Folder/%CommandShell");
        unlink("/Desktop Folder/Appearance"); unlink("/Desktop Folder/%Appearance");
        unlink("/Desktop Folder/Monitors"); unlink("/Desktop Folder/%Monitors");
        unlink("/Desktop Folder/COFFCarrier");
		unlink("/Desktop Folder/Mac81Probe");
#ifdef OT81
		unlink("/Desktop Folder/OTProbe");
#endif
#endif
		unlink("/Desktop Folder/SimpleText");
		unlink("/Desktop Folder/%SimpleText");
		dotmac("auxdispatch_36", 0, atoi(OTHERUID));
#ifdef SYS81
		/*
		 * the HFS Plus queue walk repaired, both _AUXDispatch(36) calls made,
		 * _SoundDispatch answered, the selected File Manager configuration,
		 * FSSpecs made for Unix volumes, _HFSDispatch's table entry taken
		 */
		t_check("sound_dispatch_initialization", (getsym("uinter_patched") & 0x20) != 0 ||
            getsym("uinter_soundnative") > 0,
            "neither bootstrap nor authenticated native initialization");
		t_check("live_patches", getsym("uinter_fmgroup") >= 0 &&
		    ((getsym("uinter_patched") |
            (getsym("uinter_soundnative") > 0 ? 0x20L : 0)) & ~0x2e00L) ==
		    (getsym("uinter_fmgroup") == 0 ? 0x113f : 0x117f),
		    "applied %#lx", getsym("uinter_patched"));
		/* SimpleText's exit ran the Process Manager's Time Manager cleanup */
		t_check("tm_cleanup", (getsym("uinter_patched") & 0x600) != 0,
		    "applied %#lx", getsym("uinter_patched"));
#endif
		setsym("uinter_boxflag", -1L);
		if (e >= 0)
			close(e);
#else
		startmac();
		if (macid > 0)
			restart();
		if (macid > 0 && stat(ENVSH, &sb) == 0)
			envrun();
#endif
#endif
		fiddlog();
	}
	return t_done();
}
