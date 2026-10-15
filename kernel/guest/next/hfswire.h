/*
 * The host filesystem's wire: the guest routines fill the mailbox, store
 * its address to the host-call doorbell, and wait for state to read HF_REPLY.
 * Words are big-endian 32 bits, as on the guest.
 */
#ifndef NEXT_HFSWIRE_H
#define NEXT_HFSWIRE_H
#define HF_BASE 0x08000000UL	/* the region mapped for these routines, outside the kernel's banks */
#define HF_SIZE 0x00020000UL
#define HF_CTX (HF_BASE + 0x4000UL)
#define HF_STUB (HF_BASE + 0x6000UL)
#define HF_FSNAME (HF_BASE + 0x6100UL)
#define HF_CODE (HF_BASE + 0x8000UL)
#define HF_MAGIC 0x48465331UL
#define HF_NAME 256
#define HF_DATA 8192

#define HF_IDLE 0
#define HF_REQUEST 1
#define HF_REPLY 2

#define HF_ROOT 1
#define HF_LOOKUP 2
#define HF_GETATTR 3
#define HF_SETATTR 4
#define HF_READ 5
#define HF_WRITE 6
#define HF_READDIR 7
#define HF_CREATE 8
#define HF_REMOVE 9
#define HF_RENAME 10
#define HF_MKDIR 11
#define HF_RMDIR 12
#define HF_SYMLINK 13
#define HF_READLINK 14
#define HF_LINK 15
#define HF_FSYNC 16
#define HF_INACTIVE 17
#define HF_STATFS 18

/* setattr: which fields to apply */
#define HF_SET_MODE 1
#define HF_SET_OWNER 2
#define HF_SET_SIZE 4
#define HF_SET_TIMES 8

struct hf_box {
	unsigned long magic;
	volatile unsigned long state;
	unsigned long op, node, node2, offset, length, mode, uid, gid, flags;
	long error, result;
	/* the attributes of node, on every reply that names one */
	unsigned long type, amode, auid, agid, nlink, size, blocks, rdev, ino, atime, mtime, ctime;
	unsigned long bsize, fblocks, ffree, files;
	char name[HF_NAME], name2[HF_NAME];
	unsigned char data[HF_DATA];
};

/* the kernel's types (vtype) the host reports */
#define HF_VREG 1
#define HF_VDIR 2
#define HF_VBLK 3
#define HF_VCHR 4
#define HF_VLNK 5
#define HF_VFIFO 8
#endif
