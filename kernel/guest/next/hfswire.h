/*
 * The host filesystem's wire: the guest routines fill the mailbox, make
 * the HC_HFS host call, and wait for state to read HF_REPLY.
 * Words are big-endian 32 bits, as on the guest.
 */
#ifndef NEXT_HFSWIRE_H
#define NEXT_HFSWIRE_H
#define HF_BASE 0x07fe0000UL	/* the region for these routines: past bank 0, where the kernel maps its own space one to one */
#define HF_SIZE 0x00020000UL
#define HF_CTX (HF_BASE + 0x4000UL)
#define HF_STUB (HF_BASE + 0x6000UL)
#define HF_FSNAME (HF_BASE + 0x6100UL)
#define HF_HCALL (HF_BASE + 0x6400UL)	/* move.l 4(sp),d0; illegal; rts: d0 names the service */
#define HF_HSTUB (HF_BASE + 0x6410UL)	/* 8 bytes per service: moveq #service,d0; jmp HF_HCALL+4; for 0, moveq #0,d0; rts */
#define HF_KEYS (HF_BASE + 0x7800UL)
#define HF_CODE (HF_BASE + 0x8000UL)
#define HF_MAGIC 0x48465331UL
#define HF_NAME 256
#define HF_DATA 8192

/*
 * Host services.  The illegal instruction at HF_HCALL + 4 traps to the
 * module; arguments are on the guest's stack as for a C call (4(sp) the
 * first), results in d0/d1.
 */
#define HC_HFS 1	/* the mailbox holds a request */
#define HC_MONITOR 2	/* d1: the kernel's command line; the machine ends */
#define HC_US 3		/* d1 = host microseconds, 20 bits */
#define HC_DELAY 4	/* (us) */
#define HC_TIMER 5	/* (which, ns high, ns low): the next timer interrupt */
#define HC_SOFT 6	/* d1: software interrupts 1 and 2 at bits 24 and 25 */
#define HC_TIME 7	/* d0 = host time in seconds, d1 = 0 */
#define HC_NVGET 8	/* (buf): 32 bytes of NVRAM */
#define HC_NVSET 9	/* (buf): 32 bytes, checksum put in */
#define HC_OFF 10	/* power off: the machine ends */
#define HC_STAT 11	/* d0 = interrupt status for the level 6 dispatcher */
#define HC_TACK 12	/* the timer's interrupt acknowledged; d1 as HC_US */
#define HC_N 13

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

/* typed characters for the console, polled by the guest on the kernel's callout */
struct hf_keys {
	long (*timeout)(), (*input)();
	unsigned long ticks;
	volatile unsigned long head, tail;	/* the host writes head, the guest tail */
	unsigned char buf[256];
};

/* the kernel's types (vtype) the host reports */
#define HF_VREG 1
#define HF_VDIR 2
#define HF_VBLK 3
#define HF_VCHR 4
#define HF_VLNK 5
#define HF_VSOCK 6
#define HF_VFIFO 8
#endif
