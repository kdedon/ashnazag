/*
 * startmig -- validate local Kickstart media and enter an Amiga profile.
 * SYS: is ~/Amiga, or ~/Amiga/env with -e; one writable session each.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <pwd.h>
#include "amigaio.h"
#include "rtgshare.h"
#include "inputshare.h"
#include "hostfswire.h"
#include "sysroot.h"
#include "envroot.h"
#include "miglog.h"

#define ROMBASE 0xf80000UL
#define ROMSIZE 0x80000UL
#define CHIPSIZE 0x200000UL
#define FASTBASE 0x08000000UL
#define BOOTBASE 0x00f00000UL
#define BOOTSIZE 0x80000UL

extern int mprotect();
extern int migdisp(), migkick;

static unsigned char rombuf[ROMSIZE];
static int displaylife = -1;
static void fail(char *);
static int romok();
static unsigned long get32(unsigned char *);

static void
loadboot(char *path)
{
    unsigned char *p = (unsigned char *)BOOTBASE, extra;
    unsigned long n = 0;
    int fd, count;
    fd = open(path, O_RDONLY);
    if (fd < 0) fail(path);
    while (n < BOOTSIZE) {
        count = read(fd, (char *)p + n, BOOTSIZE - n);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) fail("read boot extension");
        if (!count) break;
        n += count;
    }
    do { count = read(fd, (char *)&extra, 1); } while (count < 0 && errno == EINTR);
    close(fd);
    if (n != BOOTSIZE || count != 0 || p[0] != 0x4a || p[1] != 0xfc ||
        get32(p + 2) != BOOTBASE || get32(p + 6) <= BOOTBASE + 26 ||
        get32(p + 6) > BOOTBASE + BOOTSIZE || !(p[10] & 1) ||
        get32(p + 22) < BOOTBASE + 26 || get32(p + 22) >= BOOTBASE + BOOTSIZE) {
        miglog(1, "invalid container boot extension %.500s", path);
        exit(1);
    }
    if (mprotect((caddr_t)BOOTBASE, BOOTSIZE, PROT_READ | PROT_EXEC) < 0)
        fail("boot extension protection");
}

static void
fail(message)
	char *message;
{
	miglog(1, "%.500s: %s", message, strerror(errno));
	exit(1);
}

static unsigned long
get32(p)
	unsigned char *p;
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
	    (unsigned long)p[2] << 8 | p[3];
}

static int
readrom(path)
	char *path;
{
	unsigned long n = 0;
	int fd, count;
	unsigned char extra;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		fail(path);
	while (n < ROMSIZE) {
		count = read(fd, (char *)rombuf + n, ROMSIZE - n);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			fail(path);
		if (count == 0)
			break;
		n += count;
	}
	do { count = read(fd, (char *)&extra, 1); } while (count < 0 && errno == EINTR);
	if (count < 0)
		fail(path);
	close(fd);
	if (n != ROMSIZE || count != 0) {
		miglog(1, "%.500s: ROM must be exactly 512 KiB", path);
		return 0;
	}
	return romok(rombuf, path);
}

/* the A4000 Kickstart 3.2 (47.96), by sum, CRC and header */
static int
romok(b, name)
	unsigned char *b;
	char *name;
{
	unsigned long sum = 0, prev, crc = 0xffffffffUL, i;
	int bit;

	for (i = 0; i < ROMSIZE; i += 4) {
		prev = sum;
		sum = (sum + get32(b + i)) & 0xffffffffUL;
		if (sum < prev)
			sum++;
	}
	for (i = 0; i < ROMSIZE; i++) {
		crc ^= b[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
	}
	if (sum != 0xffffffffUL || (crc ^ 0xffffffffUL) != 0x9bb8fc93UL ||
	    get32(b) != 0x11144ef9UL || get32(b + 4) != 0xf800d2UL) {
		miglog(1, "%.500s: expected the A4000 Kickstart 3.2 (47.96) ROM", name);
		return 0;
	}
	return 1;
}

static void
region(address, size, shared)
	unsigned long address, size;
	int shared;
{
	int fd = open("/dev/zero", O_RDWR);
	char what[48];
	if (fd < 0)
		fail("/dev/zero");
	sprintf(what, "mmap %#lx+%#lx", address, size);
	if (mmap((caddr_t)address, size, PROT_READ | PROT_WRITE | PROT_EXEC,
	    (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED, fd, 0) == (caddr_t)-1)
		fail(what);
	close(fd);
}

static void
nothing(sig)
    int sig;
{
    (void)sig;
}

static void
displaygone(sig)
    int sig;
{
    static char message[] = "startmig: helper process exited\n";
    (void)sig;
    write(2, message, sizeof message - 1);
    _exit(1);
}

static void
startdisplay()
{
    int ready[2], life[2], n;
    pid_t pid;
    char success;
    struct pollfd p;
    struct sigaction sa;
    memset((char *)&sa, 0, sizeof sa);
    sa.sa_handler = displaygone;
    sa.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, (struct sigaction *)0) < 0)
        fail("SIGCHLD");
    if (pipe(ready) < 0 || pipe(life) < 0)
        fail("display pipe");
    pid = fork();
    if (pid < 0)
        fail("display fork");
    if (pid == 0) {
        close(ready[0]); close(life[1]);
        _exit(migdisp(ready[1], life[0]));
    }
    close(ready[1]); close(life[0]);
    displaylife = life[1];
    /* Keeping the write end open ties the display to this process. */
    p.fd = ready[0]; p.events = POLLIN;
    do { n = poll(&p, 1, 5000); } while (n < 0 && errno == EINTR);
    if (n <= 0 || read(ready[0], &success, 1) != 1 || success != 1) {
        miglog(1, "display session did not become ready");
        kill(pid, SIGTERM);
        exit(1);
    }
    close(ready[0]);
}

static void
startfilesystem(dev, root, readonly)
    int dev, readonly;
    char *root;
{
    int ready[2], life[2], n;
    pid_t pid;
    char success;
    struct pollfd p;
    if (pipe(ready) < 0 || pipe(life) < 0)
        fail("filesystem pipe");
    pid = fork();
    if (pid < 0) fail("filesystem fork");
    if (pid == 0) {
        close(ready[0]); close(life[1]); close(dev);
        if (displaylife >= 0) close(displaylife);
        _exit(mig_fs_broker(ready[1], life[0], root, readonly));
    }
    close(ready[1]); close(life[0]);
    p.fd = ready[0]; p.events = POLLIN;
    do { n = poll(&p, 1, 5000); } while (n < 0 && errno == EINTR);
    if (n <= 0 || read(ready[0], &success, 1) != 1 || success != 1) {
        miglog(1, "filesystem helper did not become ready");
        kill(pid, SIGTERM);
        exit(1);
    }
    close(ready[0]);
}

/*
 * The log goes in a writable SYS:, otherwise /tmp; a black screen
 * leaves the startup's progress there.
 */
static void
openlog(root, writable)
    char *root;
    int writable;
{
    char path[1100];
    int fd = -1;
    if (writable && strlen(root) < 1000) {
        sprintf(path, "%s/.startmig.log", root);
        unlink(path);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    }
    if (fd < 0) {
        sprintf(path, "/tmp/startmig.%ld.log", (long)getuid());
        unlink(path);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    }
    if (fd < 0) return;
    miglog_fd = fd;
    miglog(1, "log in %s", path);
}

static int
checksystem(char *root)
{
    struct mig_hostfs *fs = mig_hostfs_create();
    struct mig_fs_request request;
    int valid;
    if (!fs) return 0;
    if (mig_hostfs_mount(fs, 0, "Amiga", root, 1) < 0) {
        mig_hostfs_destroy(fs);
        return 0;
    }
    memset(&request, 0, sizeof request);
    request.op = MIG_FS_OPEN;
    strcpy(request.path, "S/Startup-Sequence");
    mig_hostfs_dispatch(fs, &request);
    valid = !request.error;
    mig_hostfs_destroy(fs);
    return valid;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	char *rom = "/etc/amiga/kicka4000.rom", *boot = "/etc/amiga/container-boot.rom", *end;
	char sysroot[1024], *home, *env = 0;
	struct stat sb;
	struct passwd *pw;
	int readonly = 0, rootreadonly = 0, census = 0, kick = 1, romarg = 0, hostrom = 0;
	unsigned long fastmb = 8;
	struct amigaenter ae;
	struct amigainfo info;
	struct sigaction sa;
	struct rlimit rl;
	int i, check = 0, probe = 0, fd, lockfd = -1;

	miglog_start(-1);

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--check")) check = 1;
		else if (!strcmp(argv[i], "--probe")) probe = 1;
		else if (!strcmp(argv[i], "--readonly")) readonly = 1;
		else if (!strcmp(argv[i], "--census")) census = 1;
		else if (!strcmp(argv[i], "--nokick")) kick = 0;
		else if ((!strcmp(argv[i], "-r") || !strcmp(argv[i], "-rom")) && i + 1 < argc)
			rom = argv[++i], romarg = 1;
		else if (!strcmp(argv[i], "-e") && i + 1 < argc)
			env = argv[++i];
		else if (!strcmp(argv[i], "-boot") && i + 1 < argc)
			boot = argv[++i];
		else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
			char *value = argv[++i];
			errno = 0;
			fastmb = strtoul(value, &end, 10);
			if (errno || end == value || *end || fastmb > 128)
				goto usage;
		} else goto usage;
	}
	if (check + probe > 1)
		goto usage;
	if (readonly && (check || probe)) goto usage;
	if (!probe && !check) {
		home = getenv("HOME");
		if ((!home || !*home) && (pw = getpwuid(getuid())) != 0)
			home = pw->pw_dir;
		if (env && strcmp(env, "default") != 0) {
			if (envroot("amiga", env, home, sysroot, sizeof sysroot) < 0 ||
			    stat(sysroot, &sb) < 0 || (sb.st_mode & S_IFMT) != S_IFDIR) {
				fprintf(stderr, "startmig: no environment %s; run makeamiga -e %s\n", env, env);
				return 1;
			}
		} else if (mig_sysroot(home, "/amiga/sys", sysroot, sizeof sysroot, &rootreadonly) < 0)
			fail("Amiga system directory; install /amiga/sys and run makeamiga");
		if (rootreadonly)
			fprintf(stderr, "startmig: SYS: is /amiga/sys, read-only; run makeamiga for your own\n");
		readonly |= rootreadonly;
		if (!readonly && (lockfd = envlock("startmig", "amiga", sysroot)) == -1)
			return 1;
		openlog(sysroot, !readonly);
		miglog(0, "SYS: is %.500s%s", sysroot, readonly ? ", read-only" : "");
	}
	/* the file now if named; else after the machine's own Kickstart is tried */
	if (romarg || check) {
		if (!readrom(rom))
			return 1;
		miglog(0, "Kickstart %.500s verified", rom);
	}
	if (check) {
		printf("A4000 Kickstart 3.2 (47.96): checksum and CRC verified\n");
		return 0;
	}
	if (!probe && !checksystem(sysroot)) {
		miglog(1, "%.500s has no readable S/Startup-Sequence; run makeamiga -f", sysroot);
		return 1;
	}
	for (i = 3; i < 256; i++)
		if (i != lockfd && i != miglog_fd)
			close(i);
	fd = open("/dev/amiga", O_RDWR);
	if (fd < 0 && (errno == ENXIO || errno == ENODEV)) {
		miglog(1, "/dev/amiga: Amiga module not loaded; as root run /usr/sbin/amigareg /usr/aux/lib/mod.d");
		return 1;
	}
	if (fd < 0 && errno == EACCES) {
		miglog(1, "/dev/amiga: permission denied; the display group may use it");
		return 1;
	}
	if (fd < 0)
		fail("/dev/amiga");
	if (ioctl(fd, AMIGAIOC_INFO, &info) < 0)
		fail("AMIGAIOC_INFO");
	if (info.ai_version != AMIGA_ABI_VERSION) {
		miglog(1, "Amiga module ABI %lu, expected %d", info.ai_version, AMIGA_ABI_VERSION);
		return 1;
	}
	if (!probe && !(info.ai_features & (AMIGA_FEAT_BOOT | AMIGA_FEAT_EXPERIMENTAL))) {
		miglog(1, "Amiga environment needs a 68040");
		return 1;
	}
	if (!romarg) {
		if (ioctl(fd, AMIGAIOC_MAPROM, 0) > 0 && romok((unsigned char *)ROMBASE, "the machine's Kickstart")) {
			hostrom = 1;
			miglog(0, "Kickstart: the machine's own");
		} else {
			if (!readrom(rom))
				return 1;
			miglog(0, "Kickstart %.500s verified", rom);
		}
	}
	/* the guest's memory exceeds the default soft limit on mappings */
	if (getrlimit(RLIMIT_VMEM, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_VMEM, &rl);
	}
	/* helpers ring the guest on input; the guest rings the SYS: helper */
	if (kick && (info.ai_features & AMIGA_FEAT_KICK))
		migkick = mig_fs_bell = fd;
	region(0UL, CHIPSIZE, 1);
	/* helpers fork before the private regions, which each fork would reserve again */
	if (!probe) {
		region(MIG_RTG_BASE, MIG_RTG_MAP_SIZE, 1);
		region(MIG_INPUT_BASE, MIG_INPUT_MAP_SIZE, 1);
		region(MIG_FS_BASE, MIG_FS_MAP_SIZE, 1);
		startdisplay();
		startfilesystem(fd, sysroot, readonly);
		miglog(0, "display and SYS: helpers ready");
		/* the helpers run first when both wait for the same tick */
		nice(1);
	}
	if (fastmb)
		region(FASTBASE, fastmb << 20, 0);
	if (!hostrom) {
		region(ROMBASE, ROMSIZE, 0);
		memcpy((char *)ROMBASE, rombuf, ROMSIZE);
		if (mprotect((caddr_t)ROMBASE, ROMSIZE, PROT_READ | PROT_EXEC) < 0)
			fail("mprotect");
	}
	if (!probe) {
		region(BOOTBASE, BOOTSIZE, 0);
		loadboot(boot);
	}
	/* SIGUSR2 carries virtual interrupts and must be caught */
	memset((char *)&sa, 0, sizeof sa);
	sa.sa_handler = nothing;
	sa.sa_flags = SA_NODEFER;
	if (sigaction(SIGUSR2, &sa, (struct sigaction *)0) < 0)
		fail("SIGUSR2");
	if (!probe)
		miglog(1, "starting Kickstart; Workbench follows (the hot key returns here)");
	ae.ae_version = AMIGA_ABI_VERSION;
	ae.ae_chipsize = CHIPSIZE;
	ae.ae_fastsize = fastmb << 20;
	ae.ae_flags = AMIGAF_PAL | (census ? AMIGAF_CENSUS : 0);
	if (ioctl(fd, AMIGAIOC_ENTER, &ae) < 0)
		fail("Amiga session (AMIGAIOC_ENTER)");
	if (probe) {
		if (ioctl(fd, AMIGAIOC_LEAVE, 0) < 0)
			fail("AMIGAIOC_LEAVE");
		close(fd);
		printf("Amiga profile attached and detached; ROM execution skipped\n");
		return 0;
	}
#ifdef __m68k__
	__asm__ __volatile__("mov.l %0,%%sp\n\tjmp (%1)" : : "d" (0x400L), "a" (get32((unsigned char *)ROMBASE + 4)));
#else
	fprintf(stderr, "startmig: ROM execution requires m68k\n");
	return 1;
#endif
	return 0;
usage:
	fprintf(stderr, "usage: startmig [-rom file] [-boot file] [-e env] [-m fast-MB] [--readonly] [--census] [--nokick] [--check | --probe]\n");
	return 2;
}
