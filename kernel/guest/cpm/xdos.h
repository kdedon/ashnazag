/* xdos.h -- the environment's shared memory and its processes */

#define	XPRIV	0x10000L	/* below: each process's own */
#define	XPART	0x100000L	/* the first partition */
#define	XPSIZE	0x100000L
#define	NPROC	32
#define	NCON	8
#define	NPART	64

#define	XP_SESSION	1	/* runs the CCP on its console */
#define	XP_IDLE		2	/* a session whose console nobody has joined */

extern int xme, xncon, xnpart, xmypart0;
extern int xinit(), xcon(), xjoined(), xconbusy();
extern void xsession(), xidle(), xname(), xexit(), xkillall(), xconwait(), xwarm(), xforked();
extern unsigned long xpbase();

/* startcpm */
extern int b3_conshared;
extern void b3_exit(), b3_conflush(), b3_child();
extern long b3_ccpsv();
extern int b3_setcon();
