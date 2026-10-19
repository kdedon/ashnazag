/* Open file table entry. */
#ifndef _SYS_FILE_H
#define _SYS_FILE_H

typedef struct file {
	struct file	*f_next;
	char		_f0[4];
	ushort		f_flag;
	cnt_t		f_count;
	struct vnode	*f_vnode;
	off_t		f_offset;
	struct cred	*f_cred;
	char		_f1[12];
} file_t;

/* f_flag */
#define FREAD		0x01
#define FWRITE		0x02
#define FNDELAY		0x04
#define FAPPEND		0x08
#define FSYNC		0x10
#define FNONBLOCK	0x80
#define FMASK		0xff
#define FCREAT		0x0100
#define FTRUNC		0x0200
#define FEXCL		0x0400
#define FNOCTTY		0x0800

/* per-descriptor flags */
#define FCLOSEXEC	001

#ifdef __STDC__
extern int getf(int, file_t **);
extern int closef(file_t *);
extern int falloc(struct vnode *, int, file_t **, int *);
extern void setf(int, file_t *);
extern int ufalloc(int, int *);
extern char getpof(int);
extern void setpof(int, char);
#else
extern int getf();
extern int closef();
extern int falloc();
extern void setf();
extern int ufalloc();
extern char getpof();
extern void setpof();
#endif

#endif
