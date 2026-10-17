/* Scatter/gather I/O requests. */
#ifndef _SYS_UIO_H
#define _SYS_UIO_H

typedef struct iovec {
	caddr_t	iov_base;
	int	iov_len;
} iovec_t;

typedef struct uio {
	iovec_t	*uio_iov;
	int	uio_iovcnt;
	off_t	uio_offset;
	short	uio_segflg;	/* enum uio_seg */
	short	uio_fmode;	/* file flags */
	daddr_t	uio_limit;	/* file size limit */
	int	uio_resid;	/* bytes left */
} uio_t;

typedef enum uio_rw { UIO_READ, UIO_WRITE } uio_rw_t;
typedef enum uio_seg { UIO_USERSPACE, UIO_SYSSPACE, UIO_USERISPACE } uio_seg_t;

extern int uiomove();
extern int ureadc();
extern int uwritec();

#endif
