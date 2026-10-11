#ifndef MIG_FLOPPYSHARE_H
#define MIG_FLOPPYSHARE_H
/*
 * trackdisk.device's requests to the floppy helper.  The guest fills a
 * request, sets state, rings the doorbell with 2 and waits for the
 * reply; the helper keeps each unit's status current.
 */
#define MIG_FD_BASE 0x24000000UL
#define MIG_FD_MAP_SIZE 65536UL
#define MIG_FD_DATA (MIG_FD_BASE + 4096)
#define MIG_FD_DATA_SIZE 32768UL
#define MIG_FD_MAGIC 0x4d464431U
#define MIG_FD_VERSION 1U
#define MIG_FD_BELL 0x00f7fffcUL
#define MIG_FD_UNITS 4
#define MIG_FD_IDLE 0U
#define MIG_FD_REQUEST 1U
#define MIG_FD_REPLY 2U

/* unit kinds */
#define MIG_FD_NONE 0U
#define MIG_FD_DRIVE 1U		/* a host drive, raw tracks */
#define MIG_FD_FILE 2U		/* an ADF file */
/* unit flags */
#define MIG_FD_DISK 1U
#define MIG_FD_WPROT 2U
#define MIG_FD_HD 4U

/* ops: the data area holds a track's sectors or raw bytes */
#define MIG_FD_STATUS 1U	/* refresh the unit's status */
#define MIG_FD_READ 2U		/* track's sectors into data */
#define MIG_FD_WRITE 3U		/* track's sectors from data, kept until UPDATE */
#define MIG_FD_UPDATE 4U
#define MIG_FD_CLEAR 5U		/* forget the kept track */
#define MIG_FD_MOTOR 6U		/* len 1 on, 0 off (after UPDATE) */
#define MIG_FD_RAWREAD 7U	/* len bytes; flags MIG_FD_SYNC */
#define MIG_FD_RAWWRITE 8U	/* len bytes; flags MIG_FD_INDEX */
#define MIG_FD_SYNC 1U		/* start after the first $4489 */
#define MIG_FD_INDEX 2U		/* write from the index pulse */

struct mig_fd_unit {
    volatile unsigned int kind, flags, changes, sectors;
};
struct mig_fd {
    unsigned int magic, version;
    volatile unsigned int state;
    unsigned int op, unit, track, len, flags;
    volatile unsigned int error, actual;
    struct mig_fd_unit u[MIG_FD_UNITS];
};
#define MIG_FD_BARRIER() __asm__ __volatile__("" : : : "memory")

int mig_fd_helper(int, int, int, char **);
#endif
