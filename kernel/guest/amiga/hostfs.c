#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "hostfs.h"

#ifndef S_ISLNK
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#endif

#define E_NOMEM 103
#define E_BADNUM 115
#define E_INUSE 202
#define E_EXISTS 203
#define E_NOTFOUND 205
#define E_BADLOCK 211
#define E_TYPE 212
#define E_RO 214
#define E_NOTEMPTY 216
#define E_PROTECT 223
#define E_NOMORE 232
#define E_UNSUPPORTED 209
#define HPATH 2048

struct volume { char root[HPATH], name[32]; int mounted, ro; };
struct handle { unsigned int id, volume; int fd, exclusive, writable; dev_t dev; ino_t ino; DIR *dir; char rel[MIG_FS_PATH]; };
struct mig_hostfs { struct volume volumes[MIG_FS_VOLUMES]; struct handle handles[MIG_FS_HANDLES]; unsigned int serial; };

static int doserr(int e)
{
    switch (e) {
    case ENOENT: return E_NOTFOUND;
    case ENOMEM: case EMFILE: case ENFILE: return E_NOMEM;
    case EEXIST: return E_EXISTS;
    case ENOTDIR: case EISDIR: return E_TYPE;
    case ENOTEMPTY: return E_NOTEMPTY;
    case EROFS: return E_RO;
    case ENOSPC: return 221;
    case EACCES: case EPERM: return E_PROTECT;
    default: return E_BADNUM;
    }
}
static int same(const char *a, const char *b)
{
    unsigned char x,y;
    while (*a && *b) {
        x=(unsigned char)*a++; y=(unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a'-'A';
        if (y >= 'A' && y <= 'Z') y += 'a'-'A';
        if (x != y) return 0;
    }
    return *a == *b;
}
static struct handle *lookup(struct mig_hostfs *fs, unsigned int id)
{
    unsigned int i;
    for (i=0;i<MIG_FS_HANDLES;i++) if (id && fs->handles[i].id == id) return &fs->handles[i];
    return 0;
}
static struct handle *alloc_handle(struct mig_hostfs *fs)
{
    unsigned int i;
    for (i=0;i<MIG_FS_HANDLES;i++) if (!fs->handles[i].id) {
        struct handle *h=&fs->handles[i];
        memset(h,0,sizeof(*h)); h->fd=-1;
        do { fs->serial++; if (fs->serial > 2147483647U) fs->serial=1; } while (!fs->serial || lookup(fs,fs->serial));
        h->id=fs->serial; return h;
    }
    return 0;
}
static void release(struct handle *h)
{
    if (h->fd >= 0) close(h->fd);
    if (h->dir) closedir(h->dir);
    h->id=0;
}
static int full(struct volume *v,const char *rel,char *out)
{
    if (strlen(v->root)+strlen(rel)+2 > HPATH) return E_BADNUM;
    strcpy(out,v->root); if (*rel) { strcat(out,"/"); strcat(out,rel); }
    return 0;
}
static void pop(char *s)
{
    char *p=strrchr(s,'/'); if (p) *p=0; else *s=0;
}
/* Reject symbolic links so directory mappings retain their host roots. */
static int resolve(struct mig_hostfs *fs,unsigned int vol,unsigned int id,const char *path,int create,char *rel,char *out)
{
    struct handle *h; struct volume *v; const char *p,*end; char part[MIG_FS_PATH],found[MIG_FS_PATH];
    struct stat st; DIR *d; struct dirent *de; unsigned int n; int matches;
    if (vol>=MIG_FS_VOLUMES || !fs->volumes[vol].mounted) return E_NOTFOUND;
    v=&fs->volumes[vol]; *rel=0;
    if (id) {
        h=lookup(fs,id); if (!h || h->volume!=vol || h->fd>=0) return E_BADLOCK;
        strcpy(rel,h->rel);
    }
    p=path; end=strchr(p,':');
    if (end) {
        n=(unsigned int)(end-p); if (n>=sizeof(part)) return E_BADNUM;
        memcpy(part,p,n); part[n]=0;
        if (*part && !same(part,v->name)) return E_NOTFOUND;
        *rel=0; p=end+1;
    }
    while (*p) {
        end=strchr(p,'/'); n=end?(unsigned int)(end-p):(unsigned int)strlen(p);
        if (n>=sizeof(part)) return E_BADNUM;
        memcpy(part,p,n); part[n]=0;
        if (!*part || !strcmp(part,"..")) { pop(rel); p=end?end+1:p+n; continue; }
        if (!strcmp(part,".")) { p=end?end+1:p+n; continue; }
        if (strchr(part,':')) return E_BADNUM;
        /* an environment's .env stays the session's */
        if (same(part,".env")) return create&&!end?E_PROTECT:E_NOTFOUND;
        if (full(v,rel,out)) return E_BADNUM;
        d=opendir(out); if (!d) return doserr(errno);
        matches=0; *found=0;
        while ((de=readdir(d))!=0) if (same(de->d_name,part)) {
            if (strlen(de->d_name)>=sizeof(found)) { closedir(d); return E_BADNUM; }
            strcpy(found,de->d_name); matches++;
        }
        closedir(d);
        if (matches>1) return E_INUSE;
        if (!matches) { if (!create || end) return E_NOTFOUND; strcpy(found,part); }
        if (strlen(rel)+strlen(found)+2> MIG_FS_PATH) return E_BADNUM;
        if (*rel) strcat(rel,"/");
        strcat(rel,found);
        if (full(v,rel,out)) return E_BADNUM;
        if (lstat(out,&st)<0) { if (!matches && errno==ENOENT) return 0; return doserr(errno); }
        if (S_ISLNK(st.st_mode)) return E_PROTECT;
        if (end && !S_ISDIR(st.st_mode)) return E_TYPE;
        p=end?end+1:p+n;
    }
    return full(v,rel,out);
}
static int conflicts(struct mig_hostfs *fs,unsigned int v,const char *rel,int exclusive)
{
    unsigned int i; char path[HPATH]; struct stat st; int exists;
    exists=!full(&fs->volumes[v],rel,path) && stat(path,&st)==0;
    for (i=0;i<MIG_FS_HANDLES;i++) {
        struct handle *h=&fs->handles[i];
        if (h->id && ((!strcmp(h->rel,rel) && h->volume==v) || (exists && h->dev==st.st_dev && h->ino==st.st_ino)) && (exclusive || h->exclusive)) return 1;
    }
    return 0;
}
static int busy_tree(struct mig_hostfs *fs,unsigned int v,const char *rel)
{
    unsigned int i; size_t n=strlen(rel); char path[HPATH]; struct stat st; int exists;
    exists=!full(&fs->volumes[v],rel,path) && stat(path,&st)==0;
    for(i=0;i<MIG_FS_HANDLES;i++) {
        struct handle *h=&fs->handles[i];
        if(h->id && ((h->volume==v && !strncmp(h->rel,rel,n) && (!h->rel[n] || h->rel[n]=='/')) || (exists && h->dev==st.st_dev && h->ino==st.st_ino))) return 1;
    }
    return 0;
}
static void info(struct mig_fs_request *r,const struct stat *s,const char *name)
{
    r->type=S_ISDIR(s->st_mode)?2:-3;
    r->size=(unsigned int)s->st_size; r->mtime=(unsigned int)s->st_mtime;
    r->protect=(s->st_mode&S_IWUSR)?0:5;
    strncpy(r->path,name,MIG_FS_PATH-1); r->path[MIG_FS_PATH-1]=0;
}
struct mig_hostfs *mig_hostfs_create(void) { return (struct mig_hostfs *)calloc(1,sizeof(struct mig_hostfs)); }
void mig_hostfs_destroy(struct mig_hostfs *fs)
{
    unsigned int i; if (!fs) return;
    for (i=0;i<MIG_FS_HANDLES;i++) if (fs->handles[i].id) release(&fs->handles[i]);
    free(fs);
}
int mig_hostfs_mount(struct mig_hostfs *fs,unsigned int n,const char *name,const char *root,int ro)
{
    struct stat st; struct volume *v;
    if (!fs || n>=MIG_FS_VOLUMES || !name || !root || root[0]!='/' || strlen(root)>=HPATH || !*name || strlen(name)>=32 || strchr(name,':') || strchr(name,'/')) return -1;
    v=&fs->volumes[n]; if (v->mounted || stat(root,&st)<0 || !S_ISDIR(st.st_mode)) return -1;
    strcpy(v->root,root); strcpy(v->name,name); v->ro=ro!=0; v->mounted=1; return 0;
}
void mig_hostfs_dispatch(struct mig_hostfs *fs,struct mig_fs_request *r)
{
    struct handle *h,*nh; struct volume *v; struct stat st; struct dirent *de;
    char rel[MIG_FS_PATH],rel2[MIG_FS_PATH],path[HPATH],path2[HPATH];
    int e,fd,mode; long count; off_t old,pos;
    struct statvfs sv; unsigned long total,used,unit;
    r->result=-1; r->error=0;
    if (!fs || !memchr(r->path,0,MIG_FS_PATH) || !memchr(r->path2,0,MIG_FS_PATH) || r->length>MIG_FS_DATA) { r->error=E_BADNUM; return; }
    if (r->volume>=MIG_FS_VOLUMES || !fs->volumes[r->volume].mounted) { r->error=E_NOTFOUND; return; }
    v=&fs->volumes[r->volume]; h=lookup(fs,r->handle);
    if (r->handle && (!h || h->volume!=r->volume)) { r->error=E_BADLOCK; return; }
    e=0;
    switch (r->op) {
    case MIG_FS_INFO:
        if(statvfs(v->root,&sv)<0) {e=doserr(errno);break;}
        total=sv.f_blocks;
        used=sv.f_bavail>sv.f_blocks?0:sv.f_blocks-sv.f_bavail;
        unit=sv.f_frsize?sv.f_frsize:sv.f_bsize;
        if(!unit || unit>2147483647UL) {e=E_BADNUM;break;}
        if(total>2147483647UL) total=2147483647UL;
        if(used>total) used=total;
        r->result=(int)total; r->size=(unsigned int)used;
        r->length=(unsigned int)unit;
        r->flags=v->ro || (sv.f_flag&ST_RDONLY);
        break;
    case MIG_FS_LOCK: case MIG_FS_OPEN: case MIG_FS_MKDIR:
        if ((r->op==MIG_FS_OPEN && r->flags>2) || (r->op==MIG_FS_LOCK && r->flags>1)) { e=E_BADNUM; break; }
        if (v->ro && (r->op==MIG_FS_MKDIR || (r->op==MIG_FS_OPEN && r->flags))) { e=E_RO; break; }
        e=resolve(fs,r->volume,r->handle,r->path,r->op==MIG_FS_MKDIR || (r->op==MIG_FS_OPEN && r->flags),rel,path); if(e) break;
        if (conflicts(fs,r->volume,rel,r->op==MIG_FS_OPEN?r->flags==2:r->flags!=0)) { e=E_INUSE; break; }
        nh=alloc_handle(fs); if (!nh) { e=E_NOMEM; break; }
        nh->volume=r->volume; strcpy(nh->rel,rel);
        if (r->op==MIG_FS_OPEN) {
            if (stat(path,&st)==0 && !S_ISREG(st.st_mode)) { release(nh); e=E_TYPE; break; }
            mode=r->flags==0?O_RDONLY:O_RDWR|O_CREAT; if(r->flags==2) mode|=O_TRUNC;
            mode|=O_NONBLOCK;
#ifdef O_NOFOLLOW
            mode|=O_NOFOLLOW;
#endif
            fd=open(path,mode,0666); if(fd<0) e=doserr(errno);
            else { nh->fd=fd; if(fstat(fd,&st)<0) e=doserr(errno); else if(!S_ISREG(st.st_mode)) e=E_TYPE; }
            nh->writable=r->flags!=0; nh->exclusive=r->flags==2;
        } else {
            if (r->op==MIG_FS_MKDIR && mkdir(path,0777)<0) e=doserr(errno);
            if (!e && stat(path,&st)<0) e=doserr(errno);
            if (!e && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) e=E_TYPE;
            nh->exclusive=r->op==MIG_FS_LOCK && r->flags!=0;
        }
        if(e) release(nh); else { nh->dev=st.st_dev; nh->ino=st.st_ino; r->result=(int)nh->id; }
        break;
    case MIG_FS_UNLOCK: case MIG_FS_CLOSE:
        if (!h || ((r->op==MIG_FS_CLOSE)!=(h->fd>=0))) { e=E_BADLOCK; break; }
        release(h); r->result=1; break;
    case MIG_FS_DUPLOCK: case MIG_FS_PARENT:
        if (!h) { e=E_BADLOCK; break; }
        if(r->op==MIG_FS_PARENT && !*h->rel) { r->result=0; break; }
        if(r->op==MIG_FS_DUPLOCK && h->exclusive) { e=E_INUSE; break; }
        nh=alloc_handle(fs); if(!nh) {e=E_NOMEM;break;}
        nh->volume=h->volume; strcpy(nh->rel,h->rel);
        if(r->op==MIG_FS_PARENT) pop(nh->rel);
        if(full(v,nh->rel,path) || stat(path,&st)<0) { release(nh); e=E_NOTFOUND; break; }
        if(conflicts(fs,nh->volume,nh->rel,0)) { release(nh); e=E_INUSE; break; }
        nh->dev=st.st_dev; nh->ino=st.st_ino;
        r->result=(int)nh->id; break;
    case MIG_FS_READ: case MIG_FS_WRITE: case MIG_FS_SEEK:
        if(!h || h->fd<0) {e=E_BADLOCK;break;}
        if(r->op==MIG_FS_WRITE && (v->ro || !h->writable)) { e=v->ro?E_RO:E_PROTECT;break; }
        if(r->op==MIG_FS_SEEK) {
            if(r->flags>2) {e=E_BADNUM;break;}
            old=lseek(h->fd,0,SEEK_CUR); pos=lseek(h->fd,(off_t)r->offset,(int)r->flags);
            if(old<0 || pos<0) e=doserr(errno); else if(old>2147483647L || pos>2147483647L) {lseek(h->fd,old,SEEK_SET);e=E_BADNUM;} else r->result=(int)old;
        } else {
            do { count=r->op==MIG_FS_READ?read(h->fd,r->data,r->length):write(h->fd,r->data,r->length); } while(count<0 && errno==EINTR);
            if(count<0) e=doserr(errno); else r->result=(int)count;
        }
        break;
    case MIG_FS_EXAMINE: case MIG_FS_NEXT:
        if(!h) {e=E_BADLOCK;break;}
        e=resolve(fs,h->volume,0,h->rel,0,rel,path); if(e)break;
        if(r->op==MIG_FS_EXAMINE) {
            if(stat(path,&st)<0) {e=doserr(errno);break;}
            if(h->dir) {closedir(h->dir);h->dir=0;}
            info(r,&st,*rel?(strrchr(rel,'/')?strrchr(rel,'/')+1:rel):v->name);r->result=1;
        } else {
            if(!h->dir) {h->dir=opendir(path);if(!h->dir){e=doserr(errno);break;}}
            for (;;) {
                errno=0;de=readdir(h->dir);if(!de){e=errno?doserr(errno):E_NOMORE;break;}
                if(!strcmp(de->d_name,".") || !strcmp(de->d_name,"..") || same(de->d_name,".env"))continue;
                if(strlen(path)+strlen(de->d_name)+2>sizeof(path2)) {e=E_BADNUM;break;}
                strcpy(path2,path);strcat(path2,"/");strcat(path2,de->d_name);
                if(lstat(path2,&st)<0){e=doserr(errno);break;}
                if(!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))continue;
                info(r,&st,de->d_name);r->result=1;break;
            }
        }
        break;
    case MIG_FS_RENAME: case MIG_FS_DELETE:
        if(v->ro) {e=E_RO;break;}
        e=resolve(fs,r->volume,r->handle,r->path,0,rel,path);if(e)break;
        if(!*rel || busy_tree(fs,r->volume,rel)) {e=E_INUSE;break;}
        if(r->op==MIG_FS_RENAME) {
            e=resolve(fs,r->volume,r->handle2,r->path2,1,rel2,path2);if(e)break;
            if(lstat(path2,&st)==0) {e=E_EXISTS;break;}
            if(errno!=ENOENT) {e=doserr(errno);break;}
            if(rename(path,path2)<0)e=doserr(errno);else r->result=1;
        } else {
            if(lstat(path,&st)<0)e=doserr(errno);
            else if((S_ISDIR(st.st_mode)?rmdir(path):unlink(path))<0)e=doserr(errno);else r->result=1;
        }
        break;
    default: e=E_UNSUPPORTED;
    }
    r->error=e;
}
