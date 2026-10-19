/* Uniprocessor monitor locks: sleep and wakeup. */
#ifndef _VM_MP_H
#define _VM_MP_H

typedef struct mon_t {
	unchar	dummy;
} mon_t;

#define mon_enter(lk)
#define mon_exit(lk)
#define cv_wait(lk, cond)	((void) sleep(cond, PSWP+1))
#define cv_broadcast(lk, cond)	(wakeprocs(cond, PRMPT))
#define lock_init(lk)		(lk)->dummy = 0

#endif
