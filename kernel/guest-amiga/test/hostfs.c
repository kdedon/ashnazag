#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "hostfs.h"
static struct mig_hostfs *fs;
static struct mig_fs_request r;
static void req(unsigned int op,unsigned int h,const char *p)
{
    memset(&r,0,sizeof(r));r.op=op;r.handle=h;if(p)strcpy(r.path,p);
}
static void run(void) {mig_hostfs_dispatch(fs,&r);}
int main(int argc,char **argv)
{
    unsigned int root,f,lock; char p[1024]; FILE *out;
    assert(argc==2);fs=mig_hostfs_create();assert(fs);
    assert(mig_hostfs_mount(fs,0,"SYS",argv[1],0)==0);
    req(MIG_FS_LOCK,0,"");run();assert(!r.error);root=r.result;
    req(MIG_FS_MKDIR,root,"Folder");run();assert(!r.error);lock=r.result;
    req(MIG_FS_UNLOCK,lock,0);run();assert(!r.error);
    req(MIG_FS_OPEN,root,"folder/File");r.flags=2;run();assert(!r.error);f=r.result;
    req(MIG_FS_WRITE,f,0);memcpy(r.data,"hello",5);r.length=5;run();assert(r.result==5);
    req(MIG_FS_SEEK,f,0);run();assert(r.result==5);
    req(MIG_FS_READ,f,0);r.length=9;run();assert(r.result==5&&!memcmp(r.data,"hello",5));
    req(MIG_FS_DUPLOCK,f,0);run();assert(r.error==202);
    req(MIG_FS_RENAME,root,"folder");strcpy(r.path2,"Renamed");run();assert(r.error==202);
    req(MIG_FS_CLOSE,f,0);run();assert(!r.error);
    req(MIG_FS_READ,f,0);run();assert(r.error==211);
    req(MIG_FS_OPEN,root,"folder/file");run();assert(!r.error);f=r.result;
    req(MIG_FS_DUPLOCK,f,0);run();assert(!r.error);lock=r.result;
    req(MIG_FS_EXAMINE,lock,0);run();assert(!r.error&&r.type==-3&&r.size==5);
    req(MIG_FS_UNLOCK,lock,0);run();assert(!r.error);
    req(MIG_FS_PARENT,f,0);run();assert(!r.error);lock=r.result;
    req(MIG_FS_EXAMINE,lock,0);run();assert(!r.error&&r.type==2&&!strcmp(r.path,"Folder"));
    req(MIG_FS_UNLOCK,lock,0);run();assert(!r.error);
    req(MIG_FS_CLOSE,f,0);run();assert(!r.error);
    req(MIG_FS_LOCK,root,"folder/file");run();assert(!r.error);lock=r.result;
    req(MIG_FS_LOCK,root,"FOLDER/FILE");r.flags=1;run();assert(r.error==202);
    req(MIG_FS_EXAMINE,lock,0);run();assert(!r.error&&r.size==5&&r.type==-3&&!strcmp(r.path,"File"));
    req(MIG_FS_UNLOCK,lock,0);run();assert(!r.error);
    req(MIG_FS_RENAME,root,"folder/file");strcpy(r.path2,"folder/New");r.handle2=root;run();assert(!r.error);
    req(MIG_FS_LOCK,root,"folder");run();assert(!r.error);lock=r.result;
    req(MIG_FS_NEXT,lock,0);run();assert(!r.error&&!strcmp(r.path,"New"));
    req(MIG_FS_NEXT,lock,0);run();assert(r.error==232);
    req(MIG_FS_EXAMINE,lock,0);run();assert(!r.error&&r.type==2);
    req(MIG_FS_NEXT,lock,0);run();assert(!r.error);
    req(MIG_FS_PARENT,root,0);run();assert(!r.error&&r.result==0);
    req(MIG_FS_LOCK,root,"../../");run();assert(!r.error);
    req(MIG_FS_OPEN,root,"folder");run();assert(r.error==212);
    snprintf(p,sizeof(p),"%s/fifo",argv[1]);assert(mkfifo(p,0600)==0);
    req(MIG_FS_OPEN,root,"fifo");run();assert(r.error==212);
    snprintf(p,sizeof(p),"%s/link",argv[1]);assert(symlink("/",p)==0);
    req(MIG_FS_LOCK,root,"link/etc");run();assert(r.error==223);
    snprintf(p,sizeof(p),"%s/DUP",argv[1]);out=fopen(p,"w");assert(out);fclose(out);
    snprintf(p,sizeof(p),"%s/dup",argv[1]);out=fopen(p,"w");assert(out);fclose(out);
    req(MIG_FS_OPEN,root,"DuP");run();assert(r.error==202);
    req(MIG_FS_LOCK,root,0);memset(r.path,'x',sizeof(r.path));run();assert(r.error==115);
    req(MIG_FS_READ,0,0);r.length=MIG_FS_DATA+1;run();assert(r.error==115);
    assert(mig_hostfs_mount(fs,1,"RO",argv[1],1)==0);
    req(MIG_FS_OPEN,0,"new");r.volume=1;r.flags=2;run();assert(r.error==214);
    req(MIG_FS_INFO,0,0);run();assert(!r.error&&r.result>0&&r.size<=(unsigned int)r.result&&r.length>0&&!r.flags);
    req(MIG_FS_INFO,0,0);r.volume=1;run();assert(!r.error&&r.flags);
    req(999,root,0);run();assert(r.error==209);
    /* an overlay shows its view while the file holds the original, in any case */
    snprintf(p,sizeof(p),"%s/Prefs",argv[1]);out=fopen(p,"w");assert(out);fputs("abcdef",out);fclose(out);
    assert(mig_hostfs_overlay("PREFS",(const unsigned char *)"abcdef",(const unsigned char *)"abXYef",6)==0);
    req(MIG_FS_OPEN,root,"prefs");run();assert(!r.error);f=r.result;
    req(MIG_FS_READ,f,0);r.length=3;run();assert(r.result==3&&!memcmp(r.data,"abX",3));
    req(MIG_FS_READ,f,0);r.length=9;run();assert(r.result==3&&!memcmp(r.data,"Yef",3));
    req(MIG_FS_CLOSE,f,0);run();assert(!r.error);
    req(MIG_FS_OPEN,0,"prefs");r.volume=1;run();assert(!r.error);f=r.result;
    req(MIG_FS_READ,f,0);r.volume=1;r.length=9;run();assert(r.result==6&&!memcmp(r.data,"abcdef",6));
    req(MIG_FS_CLOSE,f,0);r.volume=1;run();assert(!r.error);
    out=fopen(p,"w");assert(out);fputs("abcdeg",out);fclose(out);
    req(MIG_FS_OPEN,root,"prefs");run();assert(!r.error);f=r.result;
    req(MIG_FS_READ,f,0);r.length=9;run();assert(r.result==6&&!memcmp(r.data,"abcdeg",6));
    req(MIG_FS_CLOSE,f,0);run();assert(!r.error);
    mig_hostfs_destroy(fs);puts("hostfs: paths, locks, IO, enumeration, readonly, bounds and overlays passed");return 0;
}
