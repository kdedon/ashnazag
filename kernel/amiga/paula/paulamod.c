/*
 * /dev/paula as a loadable driver (major 64).  While loaded, the level 2
 * table's CIA-A slot runs paula.c's handler, which also serves timer B,
 * and /dev/audio's open is wrapped so the two devices exclude each other.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/errno.h"
#include "sys/conf.h"
#include "sys/stream.h"
#include "sys/moddefs.h"
#include "sys/inline.h"

#define MEMF_CLEAR	0x10000
#define LEA_A0		0x41f9		/* lea abs.l,%a0 */
#define LEA_A1		0x43f9		/* lea abs.l,%a1 */
#define NEXT		0x1c		/* a client's link field */

extern struct streamtab audioinfo;
extern void (*int2_tbl[])();
extern void aciaaintr(), paula_aciaaintr(), bzero();
extern int paulaopen(), paulaclose(), paulaioctl(), printf();
extern char *AllocMem();

int paula_busy;			/* /dev/audio refuses while set */
int audio_nopen;		/* paulaopen refuses while set */

static int pa_flag;
static int (*au_open)();
static char **au_list;		/* the audio driver's first client */
static int au_incall;

char *
paula_allocmem(n, f)
	unsigned long n, f;
{
	char *p = AllocMem(n, f);

	if (p && (f & MEMF_CLEAR))
		bzero(p, n);
	return p;
}

static int
pa_open(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	struct cred *cr;
{
	int e;

	if (paula_busy)
		return EBUSY;
	audio_nopen = *au_list != 0 || au_incall;
	/* before paulaopen can sleep, so an audio open meanwhile sees it */
	paula_busy = 1;
	if ((e = paulaopen(devp, flag, otyp, cr)) != 0)
		paula_busy = 0;
	return e;
}

/*
 * Every open of the one audio stream makes a new client, but only the
 * last close removes one: a second opener is refused, or its orphaned
 * client would keep /dev/paula busy.
 */
static int
au_wrap(q, devp, flag, sflag, cr)
	queue_t *q;
	dev_t *devp;
	int flag, sflag;
	struct cred *cr;
{
	int e;

	if (paula_busy || q->q_ptr)
		return EBUSY;
	au_incall++;
	e = (*au_open)(q, devp, flag, sflag, cr);
	au_incall--;
	return e;
}

/* the address loaded by the first lea of form op in the first n words of f */
static char *
leaabs(f, op, n)
	int (*f)();
	int op, n;
{
	unsigned short *p = (unsigned short *)f;

	for (; n > 0; n--, p++)
		if (*p == op)
			return (char *)((unsigned long)p[1] << 16 | p[2]);
	return 0;
}

struct mod_drv_data paula_drvdata[] = {
	{ { 0 }, 0, 0,
	  { pa_open, paulaclose, 0, 0, paulaioctl, 0, 0, 0, 0, 0, 0, 0, &pa_flag }, 64, 1 }
};

static int
paula_load()
{
	struct qinit *qi = audioinfo.st_rdinit;
	char *head;
	int s;

	/* audio's close walks its client list from the head; its open pushes there */
	head = leaabs(qi->qi_qclose, LEA_A1, 16);
	if (head == 0 || leaabs(qi->qi_qopen, LEA_A0, 72) != head + NEXT) {
		printf("paula: audio driver not recognised\n");
		return ENXIO;
	}
	if (int2_tbl[0] != aciaaintr) {
		printf("paula: level 2 CIA-A slot taken\n");
		return ENXIO;
	}
	s = splhi();
	au_list = (char **)(head + NEXT);
	au_open = qi->qi_qopen;
	qi->qi_qopen = au_wrap;
	int2_tbl[0] = paula_aciaaintr;
	splx(s);
	return 0;
}

static int
paula_unload()
{
	int s = splhi();

	if (paula_busy || au_incall) {
		splx(s);
		return EBUSY;
	}
	audioinfo.st_rdinit->qi_qopen = au_open;
	int2_tbl[0] = aciaaintr;
	splx(s);
	return 0;
}

MOD_DRV_WRAPPER(paula, paula_load, paula_unload, 0, "Paula channels");
