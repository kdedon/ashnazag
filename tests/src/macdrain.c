#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv)
{
    char b[64];
    int fd,n,i,k;
    struct timeval delay;
    if(argc!=2 || (fd=open(argv[1],O_RDONLY))<0)return 1;
    while((n=read(fd,b,sizeof b))>0) {
        for(i=0;i<n;i+=k) {
            k=write(1,b+i,n-i);
            if(k<0 && errno==EINTR) {k=0;continue;}
            if(k<=0) {close(fd);return 1;}
        }
        delay.tv_sec=0;delay.tv_usec=100000;
        select(0,0,0,0,&delay);
    }
    close(fd);
    return n<0;
}
