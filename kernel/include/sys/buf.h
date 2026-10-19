/* Block I/O requests, as a block driver's strategy routine sees them. */
#ifndef _SYS_BUF_H
#define _SYS_BUF_H

typedef struct buf {
	int	b_flags;
	struct buf *b_forw;
	struct buf *b_back;
	struct buf *av_forw;
	struct buf *av_back;
	o_dev_t	b_dev;
	int	b_umd;
	int	b_s1;
	unsigned b_bcount;
	union {
		caddr_t	b_addr;
		int	*b_words;
		daddr_t	*b_daddr;
	} b_un;
	daddr_t	b_blkno;
	char	b_oerror;
	unsigned int b_resid;
	clock_t	b_start;
	struct proc *b_proc;
	struct page *b_pages;	/* B_PAGEIO */
	clock_t	b_reltime;
	long	b_bufsize;
	int	(*b_iodone)();
	struct vnode *b_vp;
	struct buf *b_chain;
	int	b_reqcnt;
	int	b_error;
	dev_t	b_edev;
} buf_t;

#define	B_WRITE		0x0000
#define	B_READ		0x0001
#define	B_DONE		0x0002
#define	B_ERROR		0x0004
#define	B_BUSY		0x0008
#define	B_PAGEIO	0x10000

extern void biodone();
extern void bp_mapin();
extern void bp_mapout();

#endif
