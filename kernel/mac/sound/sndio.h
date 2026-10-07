/*
 * sndio.h -- host sound: the chip device and the session protocol.
 *
 * /dev/asc (one open, root): mmap offset 0 is the sound chip's register
 * page; read() blocks for the next event (chip interrupt or a display
 * switch) and returns a struct sndev.  Only the sound service opens it.
 *
 * Sessions talk to the service through SNDPATH, a pipe with connld: each
 * open is a private byte stream of records, a struct sndrec followed by
 * r_len bytes.  A client is bound to the session in front when it opens
 * (root may rebind it); it is heard only while that session is in front.
 * Shared by the kernel (K&R, -traditional), the service and clients.
 */
#ifndef SNDIO_H
#define SNDIO_H

#define SND_MAJ		46
#define SNDPATH		"/dev/sound"

/* Apple Sound Chip, EASC flavour, as on the 68040 Quadras */
#define ASC_PHYS	0x50F14000
#define ASC_SIZE	0x1000
#define ASC_FIFOA	0x000	/* any write in 0x000-0x3FF pushes a byte */
#define ASC_FIFOB	0x400
#define ASC_FIFOLEN	0x400
#define ASC_VERSION	0x800
#define ASC_MODE	0x801	/* 0 off, 1 FIFO */
#define ASC_CONTROL	0x802	/* 0x02 stereo */
#define ASC_FIFOMODE	0x803	/* 0x80 clears both FIFOs */
#define ASC_FIFOIRQ	0x804	/* read clears; ASC_IRQ* per FIFO, B shifted by 2 */
#define ASC_VOLUME	0x806	/* level in bits 7-5 */
#define ASC_CLOCK	0x807	/* 0: 22254.5 Hz */
#define ASC_IRQMASKA	0xF09	/* EASC: 1 masks FIFO A's interrupt */
#define ASC_IRQMASKB	0xF29
#define ASC_IRQHALF	0x01	/* below half full */
#define ASC_IRQEMPTY	0x02	/* full or empty: empty while playing */
#define ASC_RATE	0x56EE8BA3	/* 22254.545 Hz, 16.16 */

struct sndev {
	unsigned long	se_irq;		/* ASC_FIFOIRQ bits since the last read */
	unsigned long	se_nirq;	/* chip interrupts so far */
	long		se_front;	/* display session in front, 0 console */
	long		se_fuid;	/* its owner */
	unsigned long	se_serial;	/* front changes so far */
	long		se_hold;	/* a passthrough guest in front has the hardware */
};

/*
 * /dev/dmasnd (major DMA_MAJ, one open, root): the Atari DMA sound.  The
 * driver plays a ring in ST-RAM; write() queues samples in the format set
 * by DMA_SETFMT, taking whole frames up to the limit (O_NONBLOCK: what
 * fits, EAGAIN if none; EBUSY while held).  read() and poll() give struct
 * sndev events as /dev/asc does; se_irq carries DMA_EVEMPTY when all
 * queued data has played.  POLLOUT: room for more.
 */
#define DMA_MAJ		46
#define DMA_EVEMPTY	1
#define DMA_IOC		('D' << 8)
#define DMA_SETFMT	(DMA_IOC | 1)	/* struct dmafmt *: nearest the hardware has, written back */
#define DMA_GETDELAY	(DMA_IOC | 2)	/* bytes queued, not yet played */
#define DMA_SETLIMIT	(DMA_IOC | 3)	/* most bytes queued at once */
#define DMA_SETVOL	(DMA_IOC | 4)	/* 0..7 */
#define DMA_FLUSH	(DMA_IOC | 5)	/* drop what is queued, stop */
#define DMA_STATS	(DMA_IOC | 6)	/* struct dmastat * */

struct dmafmt {
	long		d_rate;		/* Hz */
	short		d_bits;		/* 8 (signed) or 16 (signed, big-endian) */
	short		d_chans;	/* 1 or 2, left first */
};

struct dmastat {
	unsigned long	d_intrs;	/* frame interrupts */
	unsigned long	d_skips;	/* ... that found the DMA past the expected block */
	unsigned long	d_repeats;	/* blocks played twice (a late interrupt) */
	unsigned long	d_fallbacks;	/* lost interrupts: back to one repeating frame */
	unsigned long	d_under;	/* queue ran dry */
	unsigned long	d_played;	/* bytes of data played */
	unsigned long	d_starts;	/* DMA starts */
	long		d_chained;
	long		d_playing;
	long		d_held;
};

/* records; a reply has the request's r_cmd */
struct sndrec {
	unsigned long	r_cmd;
	unsigned long	r_len;		/* bytes that follow */
};
#define SNDR_PCM	1	/* samples in the current format */
#define SNDR_FMT	2	/* struct sndfmt; applies to what follows */
#define SNDR_DRAIN	3	/* reply once everything before it was heard or dropped */
#define SNDR_FLUSH	4	/* drop what is queued */
#define SNDR_BIND	5	/* long: display session id (root only) */
#define SNDR_STAT	6	/* reply carries a struct sndstat */
#define SNDR_MAX	65536	/* largest r_len */

struct sndfmt {
	unsigned long	f_rate;		/* 16.16 Hz, 4000..65535 */
	unsigned short	f_enc;		/* SNDE_* */
	unsigned short	f_chans;	/* 1 or 2, interleaved */
};
#define SNDE_U8		1	/* offset binary, the Mac's 8-bit format */
#define SNDE_S8		2
#define SNDE_S16	3	/* big-endian */

/* defaults until the first SNDR_FMT: the chip's own */
#define SND_DEFRATE	ASC_RATE
#define SND_DEFENC	SNDE_U8

struct sndstat {
	long		ss_sess;	/* bound session, -1 none */
	long		ss_front;
	unsigned long	ss_heard;	/* this client's frames sent to the chip */
	unsigned long	ss_muted;	/* this client's frames dropped in the back */
	unsigned long	ss_queued;	/* frames waiting */
	unsigned long	ss_fed;		/* frames sent to the chip, all clients */
	unsigned long	ss_nirq;	/* chip interrupts */
	unsigned long	ss_under;	/* chip ran empty with data still to come */
};

/*
 * /dev/snd/note, wave1-4, samp, raw: the A/UX Sound Manager's devices (major SA_MAJ, minor = the
 * synth: SA_NSYN of them, plus SA_RESET).  The kernel only relays: each
 * open, close, write and ioctl becomes a request read from SA_SRV by the
 * service, which answers with a reply written to it.  Requests of one
 * channel go one at a time.
 */
#define SA_MAJ		47
#define SA_NSYN		7
#define SA_RESET	255
#define SA_SRV		128	/* the service's minor: root, one open */
#define SA_MAX		8192	/* write bytes per request */
#define SA_ARGMAX	64	/* ioctl argument bytes */

struct sareq {
	unsigned long	q_seq;
	long		q_ch;		/* minor */
	long		q_op;		/* SAQ_* */
	long		q_pid;		/* caller */
	long		q_uid;
	long		q_cmd;		/* ioctl command; SAQ_IOCTL's argument is q_len bytes */
	long		q_arg;		/* ioctl argument value */
	long		q_len;		/* bytes that follow */
};
#define SAQ_OPEN	1
#define SAQ_CLOSE	2
#define SAQ_WRITE	3	/* reply: bytes taken; fewer: the writer waits for SAP_SPACE */
#define SAQ_IOCTL	4	/* reply: result, then the argument's new bytes */

struct sarep {
	unsigned long	p_seq;		/* the request's; 0: a notice */
	long		p_ch;
	long		p_ret;		/* >= 0 result, < 0 -errno; for a notice SAP_* */
	long		p_len;		/* bytes that follow */
};
#define SAP_SPACE	1	/* notice: the channel takes data again */

#endif
