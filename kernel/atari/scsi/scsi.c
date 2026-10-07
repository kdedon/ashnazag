/*
 * SCSI core.  Each target has a FIFO of jobs; the run loop takes one job
 * per target in turn, so a busy disk cannot starve the network adapter.
 * A job runs to the end on the adapter (polled), then its completion is
 * called.  Completions may queue more work: the running loop picks it up,
 * so they never nest.  Jobs queued above SC_MAXIPL (an interrupt or a
 * timeout) wait for the next submission from below it.
 *
 * Per job: CHECK CONDITION fetches the sense; UNIT ATTENTION, BUSY and a
 * stalled bus (reset by the adapter) are retried sj_retries times.
 */
#include "scsi.h"

#define T_RESET		250000L	/* us from bus reset to the next selection */
#define T_BUSY		100000L	/* us before retrying a busy target */

struct scsi_hba	*scsi_hba;
long		scsi_resets;
long		scsi_deferred;

static struct scsi_job	*sq_head[SC_NTARGET], *sq_tail[SC_NTARGET];
static int		sc_running;
static int		sc_next;	/* target served next */

#ifdef FS_HOST
extern struct scsi_hba	scsi_falcon;
void	sim_us();
#define SC_DELAY(n)	sim_us(n)
#else
extern struct scsi_hba	scsi_falcon, scsi_tt;
extern unsigned long	ata_mch;
extern void		delayus();
#define SC_DELAY(n)	delayus(n)

int
scsi_sr()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s));
	return s & 0xffff;
}

int
scsi_splhi()
{
	int s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	return s;
}

void
scsi_splx(s)
int s;
{
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}
#endif

/*
 * The adapter for this machine: _MCH 2 is a TT, 3 a Falcon (or CT60).
 * Loaders pass the cookie's value or only its high word.
 */
int
scsi_attach()
{
	struct scsi_hba *h;
	unsigned long m;

	if (scsi_hba)
		return 0;
#ifdef FS_HOST
	h = &scsi_falcon;
#else
	m = ata_mch > 0xffff ? ata_mch >> 16 : ata_mch;
	switch (m) {
	case 2:	h = &scsi_tt; break;
	case 3:	h = &scsi_falcon; break;
	default: return -1;
	}
#endif
	if ((*h->h_probe)() < 0)
		return -1;
	scsi_hba = h;
	return 0;
}

char *
scsi_hbaname()
{
	return scsi_hba ? scsi_hba->h_name : "none";
}

static void
sc_sense(j)
struct scsi_job *j;
{
	struct scsi_job r;
	int i;

	for (i = 0; i < sizeof r; i++)
		((char *)&r)[i] = 0;
	r.sj_target = j->sj_target;
	r.sj_lun = j->sj_lun;
	r.sj_cdb[0] = 0x03;
	r.sj_cdb[1] = j->sj_lun << 5;
	r.sj_cdb[4] = sizeof j->sj_sense;
	r.sj_cdblen = 6;
	r.sj_data = j->sj_sense;
	r.sj_len = sizeof j->sj_sense;
	r.sj_dir = FS_IN;
	r.sj_timeout = j->sj_timeout;
	if ((*scsi_hba->h_cmd)(&r) == SS_GOOD && r.sj_done >= 3)
		j->sj_senselen = r.sj_done;
}

/* One job on the bus, with sense and retries. */
static void
sc_exec(j)
struct scsi_job *j;
{
	int r, tries;

	for (tries = 0;; tries++) {
		j->sj_done = 0;
		j->sj_senselen = 0;
		r = (*scsi_hba->h_cmd)(j);
		if (r == FS_BUSERR)
			scsi_resets++;
		else if (r == SS_CHECK && !(j->sj_flags & SJF_NOSENSE)) {
			sc_sense(j);
			if (j->sj_senselen == 0 ||
			    (j->sj_sense[2] & 0x0f) != SK_UNITATTN)
				break;
		} else if (r != SS_BUSY && r != FS_BUSY)
			break;
		if (tries >= j->sj_retries)
			break;
		if (r == FS_BUSERR)
			SC_DELAY(T_RESET);
		else if (r != SS_CHECK)
			SC_DELAY(T_BUSY);
	}
	j->sj_status = r;
}

/* Runs the queues until they are empty, when nothing else is running them. */
static void
sc_kick()
{
	struct scsi_job *j;
	int s, t, n;

	s = SC_SPLHI();
	if (sc_running || ((s >> 8) & 7) > SC_MAXIPL) {
		if (!sc_running)
			scsi_deferred++;
		SC_SPLX(s);
		return;
	}
	sc_running = 1;
	for (;;) {
		for (n = 0, j = 0; n < SC_NTARGET && j == 0; n++) {
			t = (sc_next + n) % SC_NTARGET;
			if ((j = sq_head[t]) != 0 && (sq_head[t] = j->sj_next) == 0)
				sq_tail[t] = 0;
		}
		if (j == 0)
			break;
		sc_next = (t + 1) % SC_NTARGET;
		SC_SPLX(s);
		if (scsi_hba)
			sc_exec(j);
		else
			j->sj_status = FS_NOHBA;
		j->sj_flags |= SJF_DONE;
		if (j->sj_intr)
			(*j->sj_intr)(j);
		s = SC_SPLHI();
	}
	sc_running = 0;
	SC_SPLX(s);
}

void
scsi_start(j)
struct scsi_job *j;
{
	int s, t = j->sj_target & (SC_NTARGET - 1);

	j->sj_flags &= ~SJF_DONE;
	j->sj_next = 0;
	s = SC_SPLHI();
	if (sq_head[t])
		sq_tail[t]->sj_next = j;
	else
		sq_head[t] = j;
	sq_tail[t] = j;
	SC_SPLX(s);
	sc_kick();
}

/*
 * Synchronous: the job and anything queued before it run now.  When the
 * queues are already being run (a call from a completion) or the IPL is
 * too high, the job is taken back and FS_DEFER returned.
 */
int
scsi_run(j)
struct scsi_job *j;
{
	struct scsi_job **pp, *p;
	int s, t = j->sj_target & (SC_NTARGET - 1);

	j->sj_intr = 0;
	scsi_start(j);
	if (j->sj_flags & SJF_DONE)
		return j->sj_status;
	s = SC_SPLHI();
	for (p = 0, pp = &sq_head[t]; *pp && *pp != j; pp = &(*pp)->sj_next)
		p = *pp;
	if (*pp == j) {
		*pp = j->sj_next;
		if (sq_tail[t] == j)
			sq_tail[t] = p;
	}
	SC_SPLX(s);
	return j->sj_status = FS_DEFER;
}
