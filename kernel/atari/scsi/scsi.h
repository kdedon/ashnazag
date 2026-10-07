/*
 * SCSI core: per-target job queues over one host adapter (the Falcon or
 * TT NCR 5380, polled).  Jobs run to the end in the submitter's context,
 * never above SC_MAXIPL, so the clock and keyboard keep running.
 */
#ifndef SCSI_H
#define SCSI_H

#define SC_NTARGET	8
#define SC_MYID		7
#define SC_MAXIPL	3	/* jobs submitted above this wait in the queue */

/* data direction */
#define FS_NONE		0
#define FS_IN		1
#define FS_OUT		2

/* sj_status besides a SCSI status byte */
#define FS_NOTARGET	(-1)	/* selection timed out */
#define FS_BUSERR	(-2)	/* target stalled or misbehaved; bus was reset */
#define FS_BUSY		(-3)	/* bus never free, or arbitration lost */
#define FS_NOHBA	(-4)	/* no adapter */
#define FS_DEFER	(-5)	/* queued: the bus is in use or the IPL too high */

#define SS_GOOD		0x00
#define SS_CHECK	0x02
#define SS_BUSY		0x08

#define SK_NOTREADY	0x02
#define SK_ILLEGAL	0x05
#define SK_UNITATTN	0x06

/* sj_flags */
#define SJF_NOSENSE	0x01	/* return CHECK CONDITION without fetching sense */
#define SJF_DONE	0x02	/* set by the core before sj_intr */

struct scsi_job {
	struct scsi_job	*sj_next;
	int		sj_target, sj_lun;
	unsigned char	sj_cdb[12];
	int		sj_cdblen;
	unsigned char	*sj_data;
	long		sj_len;
	int		sj_dir;
	long		sj_timeout;	/* us allowed between REQs; 0 = 1 s */
	int		sj_retries;	/* after bus reset, busy or unit attention */
	int		sj_flags;
	void		(*sj_intr)();	/* (job) on completion; may queue more */
	char		*sj_arg;
	/* results */
	int		sj_status;	/* SCSI status byte or FS_* */
	long		sj_done;	/* bytes moved */
	unsigned char	sj_sense[18];
	int		sj_senselen;	/* 0: no sense fetched */
};

/* a host adapter */
struct scsi_hba {
	char	*h_name;
	int	(*h_probe)();	/* () 0, or -1 when absent */
	int	(*h_cmd)();	/* (job) status byte or FS_*; fills sj_done */
	void	(*h_reset)();	/* () pulse RST */
};

int	scsi_attach();	/* () 0, or -1 without an adapter; probes until found */
void	scsi_start();	/* (job) queue; runs at once when it can */
int	scsi_run();	/* (job) run now; sj_status, or FS_DEFER (not queued) */
char	*scsi_hbaname();

extern struct scsi_hba	*scsi_hba;
extern long		scsi_resets;

#ifdef FS_HOST
#define SC_IPL()	0
#define SC_SPLHI()	0
#define SC_SPLX(s)	(void)(s)
#else
#define SC_IPL()	(scsi_sr() >> 8 & 7)
#define SC_SPLHI()	scsi_splhi()
#define SC_SPLX(s)	scsi_splx(s)
int	scsi_sr(), scsi_splhi();
void	scsi_splx();
#endif

#endif
