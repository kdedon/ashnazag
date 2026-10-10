#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "config.h"
extern int socket(), kill();

static int routes(b,cap) char *b;int cap;
{
 int f[2],n=0,r,status,ok=0;pid_t pid;struct pollfd p;time_t end;
 if(pipe(f)<0)return 0;
 pid=fork();if(pid<0){close(f[0]);close(f[1]);return 0;}
 if(!pid){close(f[0]);dup2(f[1],1);close(f[1]);execl("/usr/bin/netstat","netstat","-rn",(char *)0);_exit(127);}
 close(f[1]);p.fd=f[0];p.events=POLLIN;end=time((time_t *)0)+6;
 while(time((time_t *)0)<end){
  r=poll(&p,1,500);if(r<0){if(errno==EINTR)continue;break;}if(!r)continue;
  if(n==cap-1)break;r=read(f[0],b+n,cap-1-n);if(r<0){if(errno==EINTR)continue;break;}
  if(!r){ok=1;break;}n+=r;
 }
 close(f[0]);b[n]=0;
 if(!ok)kill(pid,SIGKILL);
 for(r=0;r<20;r++){
  if(waitpid(pid,&status,WNOHANG)==pid)return ok && WIFEXITED(status)&&WEXITSTATUS(status)==0;
  poll((struct pollfd *)0,0,100);
 }
 kill(pid,SIGKILL);return 0;
}
static int resolver(b,cap) char *b;int cap;
{
 FILE *f=fopen("/etc/resolv.conf","r");int n;
 if(!f){if(errno==ENOENT){*b=0;return 1;}return 0;}
 n=fread(b,1,cap-1,f);if(ferror(f)||!feof(f)){fclose(f);return 0;}
 b[n]=0;fclose(f);return 1;
}
int main(argc,argv) int argc;char **argv;
{
 struct otb_identity i;struct ifreq list[32],r;struct ifconf c;
 char text[8192],name[16];int fd,j,n,selected=0,bridge;
 if(argc!=1){fprintf(stderr,"usage: otbconfig\n");return 1;}
 memset(&i,0,sizeof i);i.version[3]=1;
 if(!routes(text,sizeof text)||!otb_route(text,i.router,name)){
  fprintf(stderr,"otbconfig: cannot read numeric default route\n");return 1;
 }
 fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0){perror("otbconfig: socket");return 1;}
 memset(&c,0,sizeof c);c.ifc_len=sizeof list;c.ifc_req=list;
 if(ioctl(fd,SIOCGIFCONF,&c)<0 || c.ifc_len<=0 || c.ifc_len>=sizeof list || c.ifc_len%sizeof list[0])goto bad;
 n=c.ifc_len/sizeof list[0];
 for(j=0;j<n;j++){
  if(!memchr(list[j].ifr_name,0,sizeof list[j].ifr_name))goto bad;
  if(strcmp(list[j].ifr_name,name))continue;
  memset(&r,0,sizeof r);strcpy(r.ifr_name,name);
  if(ioctl(fd,SIOCGIFFLAGS,&r)<0 || !(r.ifr_flags&IFF_UP) || (r.ifr_flags&(IFF_LOOPBACK|IFF_POINTOPOINT)))goto bad;
  if(ioctl(fd,SIOCGIFADDR,&r)<0 || r.ifr_addr.sa_family!=AF_INET)goto bad;
  memcpy(i.address,&((struct sockaddr_in *)&r.ifr_addr)->sin_addr,4);
  if(ioctl(fd,SIOCGIFNETMASK,&r)<0 || r.ifr_addr.sa_family!=AF_INET)goto bad;
  memcpy(i.mask,&((struct sockaddr_in *)&r.ifr_addr)->sin_addr,4);selected=1;break;
 }
 close(fd);
 if(!selected || !resolver(text,sizeof text) || !otb_resolver(text,&i) || !otb_identity_valid(&i)){
  fprintf(stderr,"otbconfig: invalid target network identity\n");return 1;
 }
 bridge=open("/dev/otbridge",O_RDWR);
 if(bridge<0){perror("otbconfig: bridge");return 1;}
 if(ioctl(bridge,OTB_SETIDENT,&i)<0){perror("otbconfig: identity");close(bridge);return 1;}
 close(bridge);
 printf("otbconfig: %s %u.%u.%u.%u\n",name,i.address[0],i.address[1],i.address[2],i.address[3]);return 0;
bad:
 perror("otbconfig: interface");close(fd);return 1;
}
