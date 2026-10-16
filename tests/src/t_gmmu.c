/*
 * t_gmmu: guestcore's 68040 MMU shadow, with no guest media.  A child
 * becomes a guest of the nextguest module with 1 MB of RAM, then, as the
 * guest's supervisor, builds 040 tables, enables translation and checks:
 * mapped pages read and write their physical pages and get U and M; a
 * write-protected page and an invalid one reach the guest's vector 2 as
 * format-7 faults with the fault address, are fixed there and rerun; a
 * transparent range is identity; a remap after pflush is seen; the user
 * space (URP) is separate and its supervisor-only page faults; 8 KB pages
 * translate; TC off is identity again.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "sys/mod.h"
#include "next/profile.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	RAMSZ	0x100000UL
#define	PH(o)	((volatile unsigned long *)(NX_PHYS + NX_RAM + (o)))
#define	PA(o)	(NX_RAM + (o))
#define	V	0x10000000UL
#define	ROOTS	0x0000UL
#define	ROOTU	0x0200UL
#define	PTRS	0x0400UL
#define	PTRU	0x0600UL
#define	PGS	0x0800UL
#define	PGU	0x0900UL
#define	PG8	0x0a00UL
#define	P1	0x10000UL
#define	P2	0x11000UL
#define	P3	0x12000UL
#define	P4	0x13000UL
#define	P5	0x14000UL
#define	P8	0x20000UL

#define	NRES	24
static struct res {
	char name[24];
	int ok;
	unsigned long got, want;
} res[NRES];
static int nres;

static unsigned long vectors[256] __attribute__((aligned(4)));
static volatile int nfault;
static volatile unsigned long lastfa, lastssw, lastfv;

static void
check(name, got, want)
	char *name;
	unsigned long got, want;
{
	if (nres < NRES) {
		strcpy(res[nres].name, name);
		res[nres].ok = got == want;
		res[nres].got = got;
		res[nres].want = want;
		nres++;
	}
}

static void
movec(cr, v)
	int cr;
	unsigned long v;
{
	/* movec d0,<cr>, one instruction per register */
	__asm__ __volatile__("mov.l %0,%%d0" : : "g" (v) : "d0");
	switch (cr) {
	case 0x003: __asm__ __volatile__(".short 0x4e7b,0x0003" : : : "memory"); break;
	case 0x006: __asm__ __volatile__(".short 0x4e7b,0x0006" : : : "memory"); break;
	case 0x801: __asm__ __volatile__(".short 0x4e7b,0x0801" : : : "memory"); break;
	case 0x806: __asm__ __volatile__(".short 0x4e7b,0x0806" : : : "memory"); break;
	case 0x807: __asm__ __volatile__(".short 0x4e7b,0x0807" : : : "memory"); break;
	}
}

static void
pflush(a)
	unsigned long a;
{
	__asm__ __volatile__("mov.l %0,%%a0\n\t.short 0xf508" : : "g" (a) : "a0", "memory");
}

static void
pflusha()
{
	__asm__ __volatile__(".short 0xf518" : : : "memory");
}

/* vector 2: the fault is fixed in the tables, then the access reruns */
void
onfault(f)
	unsigned char *f;
{
	unsigned long fa = (unsigned long)f[20] << 24 | f[21] << 16 | f[22] << 8 | f[23];

	nfault++;
	lastfa = fa;
	lastssw = f[12] << 8 | f[13];
	lastfv = f[6] << 8 | f[7];
	if (nfault > 8)
		_exit(3);
	if (fa == V + 0x1000)
		*PH(PGS + 4) &= ~4UL;		/* write-protect off */
	else if (fa == V + 0x2000)
		*PH(PGS + 8) = PA(P3) | 1;
	else if (fa == V + 0x3000)
		*PH(PGU + 12) = PA(P4) | 1;	/* user page instead of supervisor-only */
	pflush(fa);
}

__asm__("	.text\n"
"buserr:	movem.l %d0-%d1/%a0-%a1,-(%sp)\n"
"	pea 16(%sp)\n"
"	jsr onfault\n"
"	addq.l &4,%sp\n"
"	movem.l (%sp)+,%d0-%d1/%a0-%a1\n"
"	rte\n"
/* trap #1: back to supervisor mode after the trap */
"trap1:	mov.l 2(%sp),%a0\n"
"	addq.l &8,%sp\n"
"	jmp (%a0)\n");
extern char buserr[], trap1[];

static void
guest()
{
	unsigned long u0, u3, i;

	vectors[2] = (unsigned long)buserr;
	vectors[33] = (unsigned long)trap1;
	movec(0x801, (unsigned long)vectors);
	for (i = 0; i < 0x1000; i += 4) {
		*PH(P1 + i) = 0x11111111;
		*PH(P2 + i) = 0x22222222;
		*PH(P3 + i) = 0x33333333;
		*PH(P4 + i) = 0x44444444;
		*PH(P5 + i) = 0x55555555;
	}
	for (i = 0; i < 0x2000; i += 4)
		*PH(P8 + i) = 0x80000000UL + i;
	*PH(ROOTS + 4 * (V >> 25)) = PA(PTRS) | 2;
	*PH(ROOTU + 4 * (V >> 25)) = PA(PTRU) | 2;
	*PH(PTRS) = PA(PGS) | 2;
	*PH(PTRU) = PA(PGU) | 2;
	*PH(PGS + 0) = PA(P1) | 1;
	*PH(PGS + 4) = PA(P2) | 4 | 1;		/* write-protected */
	*PH(PGS + 12) = PA(P4) | 0x80 | 1;	/* supervisor only */
	*PH(PGU + 0) = PA(P5) | 1;
	*PH(PGU + 12) = PA(P4) | 0x80 | 1;
	movec(0x807, PA(ROOTS));
	movec(0x806, PA(ROOTU));
	movec(0x006, 0x0400c000UL);		/* DTT0: 0x04xxxxxx, either mode */
	movec(0x003, 0x8000UL);			/* TC: on, 4 KB pages */

	*(volatile unsigned long *)V = 0x12345678;
	check("write_mapped", *PH(P1), 0x12345678UL);
	check("um_written", *PH(PGS) & 0x18, 0x18UL);
	check("read_wp", *(volatile unsigned long *)(V + 0x1000), 0x22222222UL);
	check("u_read", *PH(PGS + 4) & 0x18, 0x08UL);
	*(volatile unsigned long *)(V + 0x1000) = 0x2222aaaa;
	check("wp_fault", nfault, 1UL);
	check("wp_fa", lastfa, V + 0x1000);
	check("wp_frame", lastfv >> 12, 7UL);
	check("wp_ssw_write_atc", lastssw & 0x500, 0x400UL);
	check("wp_rerun", *PH(P2), 0x2222aaaaUL);
	check("invalid_read", *(volatile unsigned long *)(V + 0x2000), 0x33333333UL);
	check("invalid_fault", nfault, 2UL);
	check("invalid_fa", lastfa, V + 0x2000);
	check("transparent", *(volatile unsigned long *)PA(P1 + 4), 0x11111111UL);
	*PH(PGS) = PA(P3) | 1;
	pflush(V);
	check("remap", *(volatile unsigned long *)V, 0x33333333UL);

	/* user mode: the URP's space; trap #1 comes back */
	__asm__ __volatile__("mov.l %%sp,%%a0\n\t.short 0x4e60\n\t"	/* move a0,usp */
	    "andi.w &0xdfff,%%sr\n\t"
	    "mov.l %2,%%a0\n\tmov.l (%%a0),%0\n\t"
	    "mov.l 0x3000(%%a0),%1\n\t"
	    "trap &1"
	    : "=d" (u0), "=d" (u3) : "g" (V) : "a0", "memory");
	check("user_space", u0, 0x55555555UL);
	check("user_super_fault_fa", lastfa, V + 0x3000);
	check("user_super_rerun", u3, 0x44444444UL);
	check("super_again", *(volatile unsigned long *)V, 0x33333333UL);

	/* 8 KB pages */
	*PH(PTRS) = PA(PG8) | 2;
	*PH(PG8) = PA(P8) | 1;
	movec(0x003, 0xc000UL);
	check("page8k_lo", *(volatile unsigned long *)(V + 4), 0x80000004UL);
	check("page8k_hi", *(volatile unsigned long *)(V + 0x1ff8), 0x80001ff8UL);
	pflusha();
	movec(0x003, 0UL);
	check("tc_off_identity", *(volatile unsigned long *)PA(P2), 0x2222aaaaUL);
	check("faults", nfault, 3UL);
}

static int
machine(fd)
	int fd;
{
	struct nx_enter ne;
	unsigned long size = RAMSZ;
	int dev = open("/dev/nextguest", O_RDWR);

	if (dev < 0 || ioctl(dev, NX_RAMNEW, &size) < 0)
		return 4;
	if (mmap((void *)NX_RAM, RAMSZ, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, dev, 0) == (void *)-1 ||
	    mmap((void *)(NX_PHYS + NX_RAM), RAMSZ, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, dev, 0) ==
	    (void *)-1)
		return 5;
	signal(SIGUSR2, SIG_IGN);
	memset(&ne, 0, sizeof ne);
	ne.ramsize = RAMSZ;
	if (ioctl(dev, NX_ENTER, &ne) < 0)
		return 6;
	guest();
	write(fd, res, sizeof res);
	return 0;
}

static int
modid(name)
	char *name;
{
	struct modstatus st;
	int id = 1;

	while (modstat(id, &st, 1) == 0) {
		if (strcmp(st.ms_name, name) == 0)
			return st.ms_id;
		id = st.ms_id + 1;
	}
	return -1;
}

int
main()
{
	struct mod_mreg reg;
	int mj = NX_MAJOR, p[2], st, i, n, id;
	pid_t pid;

	t_init("gmmu", 120);
	if (access(MD "/nextguest", 0) < 0 || access("/dev/nextguest", 0) < 0) {
		t_skip("module", "no nextguest module on this root");
		return t_done();
	}
	modpath(MD);
	memset(&reg, 0, sizeof reg);
	strcpy(reg.md_modname, "nextguest");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "%s", T_ERR) || pipe(p) < 0)
		return t_done();
	if ((pid = fork()) == 0) {
		close(p[0]);
		_exit(machine(p[1]));
	}
	close(p[1]);
	n = read(p[0], res, sizeof res);
	close(p[0]);
	t_check("machine", t_waitchild(pid, &st, 60) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
	    n == sizeof res, "status %x, %d bytes", st, n);
	for (i = 0; n == sizeof res && i < NRES && res[i].name[0]; i++)
		t_check(res[i].name, res[i].ok, "got %lx, want %lx", res[i].got, res[i].want);
	id = modid("nextguest");
	if (id > 0)
		moduload(id);
	return t_done();
}
