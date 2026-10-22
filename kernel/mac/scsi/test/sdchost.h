/*
 * Host build of sdclust.c for the simulator: buffers, uio and kernel
 * services come from ncrsim.c.
 */
#include "ncrhost.h"

#define	sleep		sdc_sleep
#define	bcopy		sdc_bcopy
#define	dev_t		sdc_dev_t

typedef unsigned long	paddr_t;
typedef int		dev_t;
struct proc;
struct cred;

struct buf {
	int		b_flags;
	struct buf	*av_forw;
	unsigned	b_bcount;
	union {
		caddr_t	b_addr;
	} b_un;
	long		b_blkno;
	unsigned	b_resid;
	struct proc	*b_proc;
	int		b_error;
	dev_t		b_edev;
};

struct uio {
	long	uio_offset;
	int	uio_resid;
	caddr_t	uio_base;
};

#define	B_WRITE		0
#define	B_READ		1
#define	B_DONE		2
#define	B_ERROR		4
#define	EIO		5
#define	PRIBIO		20
#define	KM_SLEEP	0
#define	KM_NOSLEEP	1

extern bool	sdvalid();
extern int	uiophysio(), sleep(), wakeup();
extern uint	sdblkno(), sddevsize();
extern void	iodone(), sdqueue(), bcopy(), dlm_cachepush();
extern char	*sdhardwarename(), *kmem_alloc();
extern paddr_t	vtop();
extern int	(*sd_strathook)(), (*sd_rawhook)();
