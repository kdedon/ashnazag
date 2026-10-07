#ifndef AMIGA_HOSTFS_H
#define AMIGA_HOSTFS_H

#define MIG_FS_PATH 512
#define MIG_FS_DATA 32768
#define MIG_FS_VOLUMES 8
#define MIG_FS_HANDLES 256
#define MIG_FS_LOCK 1
#define MIG_FS_UNLOCK 2
#define MIG_FS_DUPLOCK 3
#define MIG_FS_PARENT 4
#define MIG_FS_OPEN 5
#define MIG_FS_CLOSE 6
#define MIG_FS_READ 7
#define MIG_FS_WRITE 8
#define MIG_FS_SEEK 9
#define MIG_FS_EXAMINE 10
#define MIG_FS_NEXT 11
#define MIG_FS_RENAME 12
#define MIG_FS_DELETE 13
#define MIG_FS_MKDIR 14
#define MIG_FS_INFO 15

/* Native big-endian 32-bit words; payloads contain no process pointers. */
struct mig_fs_request {
    unsigned int op, volume, handle, handle2, flags;
    int offset;
    unsigned int length;
    int result, error, type;
    unsigned int size, mtime, protect;
    char path[MIG_FS_PATH], path2[MIG_FS_PATH];
    unsigned char data[MIG_FS_DATA];
};

struct mig_hostfs;
struct mig_hostfs *mig_hostfs_create(void);
void mig_hostfs_destroy(struct mig_hostfs *);
int mig_hostfs_mount(struct mig_hostfs *, unsigned int, const char *, const char *, int);
void mig_hostfs_dispatch(struct mig_hostfs *, struct mig_fs_request *);
int mig_hostfs_overlay(const char *, const unsigned char *, const unsigned char *, unsigned long);
int mig_hostfs_overlay_when(const char *, const unsigned char *, const unsigned char *,
    unsigned long, const volatile unsigned int *, unsigned int);

#endif
