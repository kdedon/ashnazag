/*
 * top - a top users display for Unix
 *
 * SYNOPSIS:  Ash Nazag (SVR4 on 68k Mac, Amiga, Atari)
 *
 * DESCRIPTION:
 * Processes from /proc (PIOCPSINFO), system figures from /dev/kstat,
 * swap from swapctl().  No floating point: FPU-less CPUs run it.
 */
#include "top.h"
#include "machine.h"
#include "utils.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/procfs.h>
#include <sys/swap.h>
#include <sys/priocntl.h>
#include <sys/tspriocntl.h>
#include <sys/procset.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "kstat.h"

#define PROCFS		"/proc"
#define CPUSTATES	4
#define NSTATES		7

/* pr_state, pr_flag */
#define SRUN		2
#define SONPROC		6
#define SSYS		0x00000001

struct handle {
	struct prpsinfo **next_proc;
	int remaining;
};

static char header[] =
"  PID X        PRI NICE  SIZE   RES STATE   TIME    CPU COMMAND";
#define UNAME_START 6
#define Proc_format "%5d %-8.8s %3ld %4d %5s %5s %-5s %6s %3d.%d%% %.16s"

static char *state_abbrev[] =
{"", "sleep", "run", "zombie", "stop", "start", "cpu"};

static int process_states[NSTATES];
static char *procstatenames[] = {
	"", " sleeping, ", " running, ", " zombie, ", " stopped, ",
	" starting, ", " on cpu, ", NULL
};

static int cpu_states[CPUSTATES];
static char *cpustatenames[] = { "idle", "user", "kernel", "wait", NULL };
static char *cpunames[] = { "cpu_idle", "cpu_user", "cpu_kernel", "cpu_wait" };

static int memory_stats[4];
static char *memorynames[] =
{ "K real, ", "K free, ", "K swap, ", "K free swap", NULL };

static kstat_ctl_t *kc;
static long pagek;		/* page size in K */
static DIR *procdir;
static struct prpsinfo *pbase;
static struct prpsinfo **pref;
static int pmax, nproc;

/* CPU time by pid at the last update, sorted by pid; %CPU from the change */
struct ptime {
	pid_t pid;
	long ms;
};
static struct ptime *told, *tnew;
static int nold;
static int *pcpu;		/* tenths of a percent, parallel to pbase */
static long lastms;

extern char *myname;
extern void quit();

static long
nowms()
{
	static long t0;
	struct timeval tv;

	gettimeofday(&tv, (struct timezone *)0);
	if (t0 == 0)
		t0 = tv.tv_sec;
	return (tv.tv_sec - t0) * 1000 + tv.tv_usec / 1000;
}

int
machine_init(statics)
	struct statics *statics;
{
	statics->procstate_names = procstatenames;
	statics->cpustate_names = cpustatenames;
	statics->memory_names = memorynames;
	if ((kc = kstat_open()) == 0) {
		fprintf(stderr, "%s: /dev/kstat: cannot read system statistics\n", myname);
		return -1;
	}
	pagek = kstat_value(kc, "pagesize") / 1024;
	if (pagek == 0)
		pagek = 1;
	if ((procdir = opendir(PROCFS)) == 0 || chdir(PROCFS) < 0) {
		fprintf(stderr, "%s: cannot read %s\n", myname, PROCFS);
		return -1;
	}
	lastms = nowms();
	return 0;
}

char *
format_header(uname_field)
	char *uname_field;
{
	char *ptr = header + UNAME_START;

	while (*uname_field != '\0')
		*ptr++ = *uname_field++;
	return header;
}

static void
getswap(total, avail)
	long *total, *avail;
{
	static char paths[16][64];
	char buf[sizeof(swaptbl_t) + 16 * sizeof(swapent_t)];
	swaptbl_t *st = (swaptbl_t *)buf;
	int i, n;

	*total = *avail = 0;
	if ((n = swapctl(SC_GETNSWP, 0)) <= 0)
		return;
	if (n > 16)
		n = 16;
	st->swt_n = n;
	for (i = 0; i < n; i++)
		st->swt_ent[i].ste_path = paths[i];
	if ((n = swapctl(SC_LIST, st)) <= 0)
		return;
	for (i = 0; i < n; i++) {
		*total += st->swt_ent[i].ste_pages;
		*avail += st->swt_ent[i].ste_free;
	}
}

void
get_system_info(si)
	struct system_info *si;
{
	static long cp_old[CPUSTATES], cp_diff[CPUSTATES];
	long cp[CPUSTATES], sw, swfree, fs;
	int i;

	if (kstat_read(kc) < 0) {
		fprintf(stderr, "%s: /dev/kstat: read failed\n", myname);
		quit(2);
	}
	for (i = 0; i < CPUSTATES; i++)
		cp[i] = kstat_value(kc, cpunames[i]);
	percentages(CPUSTATES, cpu_states, cp, cp_old, cp_diff);

	/* hundredths, rounded */
	fs = kstat_value(kc, "fscale");
	if (fs == 0)
		fs = 1;
	si->load_avg[0] = (kstat_value(kc, "avenrun_1min") * 100 + fs / 2) / fs;
	si->load_avg[1] = (kstat_value(kc, "avenrun_5min") * 100 + fs / 2) / fs;
	si->load_avg[2] = (kstat_value(kc, "avenrun_15min") * 100 + fs / 2) / fs;

	memory_stats[0] = kstat_value(kc, "physmem") * pagek;
	memory_stats[1] = kstat_value(kc, "freemem") * pagek;
	getswap(&sw, &swfree);
	memory_stats[2] = sw * pagek;
	memory_stats[3] = swfree * pagek;

	si->cpustates = cpu_states;
	si->memory = memory_stats;
	si->last_pid = -1;
}

static int
ptcmp(a, b)
	struct ptime *a, *b;
{
	return a->pid < b->pid ? -1 : a->pid > b->pid;
}

/* every process's psinfo into pbase, %CPU since the last call into pcpu */
static void
getptable()
{
	struct dirent *d;
	struct ptime key, *o;
	long now, el, ms;
	int fd, i;

	nproc = 0;
	for (rewinddir(procdir); (d = readdir(procdir)) != 0;) {
		if (d->d_name[0] == '.')
			continue;
		if (nproc == pmax) {
			pmax = pmax ? pmax * 2 : 64;
			pbase = (struct prpsinfo *)realloc(pbase, pmax * sizeof *pbase);
			pref = (struct prpsinfo **)realloc(pref, pmax * sizeof *pref);
			pcpu = (int *)realloc(pcpu, pmax * sizeof *pcpu);
			tnew = (struct ptime *)realloc(tnew, pmax * sizeof *tnew);
			told = (struct ptime *)realloc(told, pmax * sizeof *told);
			if (!pbase || !pref || !pcpu || !tnew || !told) {
				fprintf(stderr, "%s: out of memory\n", myname);
				quit(12);
			}
		}
		if ((fd = open(d->d_name, O_RDONLY)) < 0)
			continue;
		if (ioctl(fd, PIOCPSINFO, &pbase[nproc]) == 0)
			nproc++;
		close(fd);
	}
	now = nowms();
	el = now - lastms;
	lastms = now;
	for (i = 0; i < nproc; i++) {
		ms = pbase[i].pr_time.tv_sec * 1000 + pbase[i].pr_time.tv_nsec / 1000000;
		key.pid = pbase[i].pr_pid;
		o = nold ? (struct ptime *)bsearch((char *)&key, (char *)told, nold,
		    sizeof key, ptcmp) : 0;
		pcpu[i] = 0;
		if (o && el > 0 && ms >= o->ms)
			pcpu[i] = (ms - o->ms) * 1000 / el;
		if (pcpu[i] > 1000)
			pcpu[i] = 1000;
		tnew[i].pid = key.pid;
		tnew[i].ms = ms;
	}
	qsort((char *)tnew, nproc, sizeof *tnew, ptcmp);
	o = told;
	told = tnew;
	tnew = o;
	nold = nproc;
}

static struct handle handle;

caddr_t
get_process_info(si, sel, compare)
	struct system_info *si;
	struct process_select *sel;
	int (*compare)();
{
	struct prpsinfo *pp, **prefp;
	int i, total = 0, active = 0;

	getptable();
	si->procstates = process_states;
	memset((char *)process_states, 0, sizeof process_states);
	prefp = pref;
	for (pp = pbase, i = 0; i < nproc; pp++, i++) {
		if (pp->pr_state <= 0 || pp->pr_state >= NSTATES)
			continue;
		if (!sel->system && (pp->pr_flag & SSYS))
			continue;
		total++;
		process_states[pp->pr_state]++;
		if (!pp->pr_zomb &&
		    (sel->idle || pp->pr_state == SRUN || pp->pr_state == SONPROC || pcpu[i]) &&
		    (sel->uid == -1 || pp->pr_uid == (uid_t)sel->uid) &&
		    (sel->command == 0 || strcmp(pp->pr_fname, sel->command) == 0)) {
			*prefp++ = pp;
			active++;
		}
	}
	if (compare != 0)
		qsort((char *)pref, active, sizeof *pref, compare);
	si->p_total = total;
	si->P_ACTIVE = active;
	handle.next_proc = pref;
	handle.remaining = active;
	return (caddr_t)&handle;
}

static char fmt[MAX_COLS];

char *
format_next_process(h, get_userid)
	caddr_t h;
	char *(*get_userid)();
{
	struct handle *hp = (struct handle *)h;
	struct prpsinfo *pp;
	int c;

	pp = *hp->next_proc++;
	hp->remaining--;
	c = pcpu[pp - pbase];
	sprintf(fmt, Proc_format,
	    (int)pp->pr_pid,
	    (*get_userid)(pp->pr_uid),
	    pp->pr_pri,
	    pp->pr_nice - NZERO,
	    format_k(pp->pr_size * pagek),
	    format_k(pp->pr_rssize * pagek),
	    state_abbrev[pp->pr_state],
	    format_time(pp->pr_time.tv_sec),
	    c / 10, c % 10,
	    printable(pp->pr_fname));
	return fmt;
}

/* busiest first: recent CPU, then total CPU, state, size */
static unsigned char sorted_state[] = { 0, 3, 6, 2, 4, 5, 7 };

int
proc_compare(pp1, pp2)
	struct prpsinfo **pp1, **pp2;
{
	struct prpsinfo *p1 = *pp1, *p2 = *pp2;
	long r;

	if ((r = pcpu[p2 - pbase] - pcpu[p1 - pbase]) == 0 &&
	    (r = p2->pr_time.tv_sec - p1->pr_time.tv_sec) == 0 &&
	    (r = sorted_state[p2->pr_state] - sorted_state[p1->pr_state]) == 0 &&
	    (r = p2->pr_pri - p1->pr_pri) == 0 &&
	    (r = p2->pr_rssize - p1->pr_rssize) == 0)
		r = p2->pr_size - p1->pr_size;
	return r < 0 ? -1 : r > 0;
}

int
proc_owner(pid)
	int pid;
{
	int i;

	for (i = 0; i < nproc; i++)
		if (pbase[i].pr_pid == pid)
			return pbase[i].pr_uid;
	return -1;
}

/* renice through the time-sharing class */
int
setpriority(dummy, who, niceval)
	int dummy, who, niceval;
{
	pcinfo_t pcinfo;
	pcparms_t pcparms;
	tsparms_t *ts;
	int scale;

	strcpy(pcinfo.pc_clname, "TS");
	if (priocntl(0, 0, PC_GETCID, (caddr_t)&pcinfo) == -1)
		return -1;
	if (niceval > 20)
		niceval = 20;
	else if (niceval < -20)
		niceval = -20;
	ts = (tsparms_t *)pcparms.pc_clparms;
	scale = ((tsinfo_t *)pcinfo.pc_clinfo)->ts_maxupri;
	ts->ts_uprilim = ts->ts_upri = -(scale * niceval) / 20;
	pcparms.pc_cid = pcinfo.pc_cid;
	return priocntl(P_PID, who, PC_SETPARMS, (caddr_t)&pcparms) == -1 ? -1 : 0;
}
