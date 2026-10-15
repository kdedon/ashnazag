#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ksym.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "sys/mod.h"
#include "t.h"
#include "../../kernel/guest/mod/tosguest/tosio.h"

#define CHECK(ok, name) (test_ok = (ok), t_check(name, test_ok, "errno=%d", errno))
static int test_ok;

static long count(name)
char *name;
{
	unsigned long value = 0, info;
	long n = -1;
	int fd;
	if (getksym(name, &value, &info) < 0 || (fd = open("/dev/kmem", O_RDONLY)) < 0)
		return -1;
	if (lseek(fd, value, 0) != -1)
		(void)read(fd, &n, sizeof n);
	close(fd);
	return n;
}

static long kread(addr)
unsigned long addr;
{
	long n = -1;
	int fd = open("/dev/kmem", O_RDONLY);
	if (fd < 0)
		return -1;
	if (lseek(fd, addr, 0) != -1)
		(void)read(fd, &n, sizeof n);
	close(fd);
	return n;
}

/* Hold count of the character-device snode a device mapping references. */
static long devholds(rdev)
dev_t rdev;
{
	unsigned long base = 0, info, sp;
	int i, k;
	if (getksym("stable", &base, &info) < 0)
		return -1;
	for (i = 0; i < 16; i++)
		for (sp = kread(base + 4 * i), k = 0; sp && k < 1000; sp = kread(sp), k++)
			if (kread(sp + 88) == (long)rdev && kread(sp + 28) == 4 &&
			    kread(sp + 80) != (long)(sp + 4))
				return kread(sp + 4) & 0xFFFF;
	return -1;
}

int main()
{
	int fd, round, b[2], pid, st, major = 56;
	long basepages, baseobjects, after;
	volatile unsigned char *p;
	char c;
	unsigned long n = 0x4000;
	struct mod_mreg reg;
	struct stat sb;

	t_init("envram", 60);
	modpath(stat("/tests/aux/mod.d", &sb) == 0 ?
	    "/tests/aux/mod.d" : "/etc/conf/mod.d");
	strcpy(reg.md_modname, "tosguest");
	reg.md_typedata = (caddr_t)&major;
	CHECK(modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0, "register RAM client");
	fd = open("/dev/tos", O_RDWR);
	if (fd < 0) { CHECK(0, "open RAM client"); return t_done(); }
	basepages = count("guest_ram_pages");
	baseobjects = count("guest_ram_objects");
	CHECK(basepages == 0 && baseobjects == 0 && count("guest_ram_reserved") == 0, "no guest RAM allocated at boot");
	close(fd);
	fd = open("/dev/tos", O_RDWR);
	if (fd >= 0 && fstat(fd, &sb) == 0 && ioctl(fd, TOSIOC_RAM, n) == 0) {
		long holds = devholds(sb.st_rdev);
		CHECK(holds > 0, "find device snode");
		for (round = 0; round < 8; round++) {
			p = (unsigned char *)mmap((caddr_t)0x10000000, n, PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_FIXED, fd, 0);
			if (p == (unsigned char *)-1)
				break;
			p[0] = 1;
			munmap((caddr_t)p + 0x1000, 0x1000);
			pid = fork();
			if (pid == 0)
				_exit(p[0] != 1);
			if (pid > 0)
				t_waitchild(pid, &st, 10);
			munmap((caddr_t)p, 0x1000);
			munmap((caddr_t)p + 0x2000, n - 0x2000);
		}
		after = devholds(sb.st_rdev);
		t_check("map, split, fork and unmap return device holds to baseline",
		    round == 8 && after == holds, "holds %ld -> %ld", holds, after);
	} else
		CHECK(0, "open RAM client for hold count");
	if (fd >= 0)
		close(fd);
	for (round = 0; round < 3; round++) {
		fd = open("/dev/tos", O_RDWR);
		CHECK(fd >= 0 && ioctl(fd, TOSIOC_RAM, n) == 0, "allocate scattered RAM");
		CHECK(count("guest_ram_pages") == 0 && count("guest_ram_objects") == 1 &&
		    count("guest_ram_reserved") == 0,
		    "allocation charges no RAM before pages fault in");
		p = (unsigned char *)mmap((caddr_t)0x10000000, n, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED, fd, 0);
		CHECK(p != (unsigned char *)-1, "map RAM at virtual address");
		if (p == (unsigned char *)-1) { close(fd); continue; }
		CHECK(p[0] == 0 && p[0x3000] == 0, "faults return zeroed pages");
		p[0] = 0x73; p[0x3000] = 0x31;
		CHECK(count("guest_ram_pages") == 2 && count("guest_ram_reserved") == 2,
		    "only touched pages are resident and charged");
		CHECK(munmap((caddr_t)p + 0x1000, 0x1000) == 0, "split mapping with middle unmap");
		if (pipe(b) < 0) { CHECK(0, "pipe"); munmap((caddr_t)p, n); close(fd); continue; }
		pid = fork();
		if (pid == 0) {
			close(b[1]);
			read(b[0], &c, 1);
			st = p[0] != 0x73 || p[0x3000] != 0x31;
			munmap((caddr_t)p, n);
			close(fd); close(b[0]);
			_exit(st);
		}
		if (pid < 0) { close(b[0]); close(b[1]); munmap((caddr_t)p, n); close(fd); CHECK(0, "fork shared RAM mappings"); continue; }
		close(b[0]);
		CHECK(pid > 0, "fork shared RAM mappings");
		munmap((caddr_t)p, n); close(fd);
		CHECK(count("guest_ram_pages") == 2 && count("guest_ram_reserved") == 2,
		    "child retains RAM and budget after parent unmap and close");
		write(b[1], "x", 1); close(b[1]);
		st = -1;
		CHECK(pid > 0 && t_waitchild(pid, &st, 10) == pid && st == 0, "child reads retained pages and releases them");
		CHECK(count("guest_ram_pages") == 0 && count("guest_ram_objects") == 0 &&
		    count("guest_ram_reserved") == 0,
		    "last mapping returns pages and RAM budget");
	}
	return t_done();
}
