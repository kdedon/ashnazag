/*
 * next -- a NeXTstation Turbo from the user's boot ROM.
 *
 *	native [rom]
 *	native -k mach_kernel
 *
 * The ROM (128 KB) is $NATIVEHOST_MEDIA.  With -k the boot program's job
 * is done here instead: the m68k slice of the user's kernel is loaded into
 * RAM and entered with the MMU off and the boot arguments it expects; the
 * ROM is not needed.  RAM is cleared at every start;
 * VRAM is $NATIVEHOST_ROOT/vram and NVRAM $NATIVEHOST_ROOT/nvram.  This
 * process becomes the machine (nextguest); a child shows the 2-bit
 * framebuffer and keeps the NVRAM file.  Cmd-Q quits.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/procfs.h>
#include <sys/ucontext.h>
#include <siginfo.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nativehost.h"
#include "profile.h"
#include "hfswire.h"

extern unsigned char hfs_blob[];
extern unsigned long hfs_blob_len, hfs_entry;
int hf_open();
void hf_serve();

#define L(a) (*(volatile unsigned long *)(a))
static unsigned long width, height, rowbytes;	/* the guest's screen: the host's, 2 bits per pixel */
static unsigned long shownw, shownh;	/* the part of it the host screen shows */

/* the kernel's console window, 100 columns by 48 rows framed, must fit */
#define MIN_WIDTH 832
#define MIN_HEIGHT 624
static unsigned long ramsize;	/* the largest of 32, 16, 8 MB that free memory holds */

static struct nh_display display;
static int bridge = -1;
static pid_t machine;

/* the host's key codes: command is 0x37, Q 0x0c */
#define KEY_COMMAND 0x37

static void
scale(row, vram, line)
	int row;
	unsigned char *vram, *line;
{
	unsigned char *s = vram + row * rowbytes;
	unsigned long x;

	for (x = 0; x < shownw; x++)
		line[x] = 255 - 85 * (s[x >> 2] >> (6 - 2 * (x & 3)) & 3);
}

/* the changed rows of the screen, 2 bits to 8 */
static void
present(vram, shown)
	unsigned char *vram, *shown;
{
	static unsigned char line[4096];
	unsigned char *fb = (unsigned char *)display.mapping + display.info.fi_offset;
	unsigned long y, rb = display.info.fi_rowbytes;

	for (y = 0; y < shownh; y++) {
		if (!memcmp(shown + y * rowbytes, vram + y * rowbytes, rowbytes))
			continue;
		memcpy(shown + y * rowbytes, vram + y * rowbytes, rowbytes);
		scale((int)y, vram, line);
		memcpy(fb + y * rb, line, shownw);
	}
}

/* the host screen's size, the guest's within its video memory */
static int
screen()
{
	struct fbinfo fi;
	int fd = open("/dev/fb0", O_RDWR);

	if (fd < 0 || ioctl(fd, FBIOGINFO, &fi) < 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	close(fd);
	width = (fi.fi_width > 4080 ? 4080 : fi.fi_width < MIN_WIDTH ? MIN_WIDTH : fi.fi_width) & ~15UL;
	rowbytes = width / 4;
	height = fi.fi_height > 4095 ? 4095 : fi.fi_height < MIN_HEIGHT ? MIN_HEIGHT : fi.fi_height;
	if (height > NX_VRAM_SIZE / rowbytes)
		height = NX_VRAM_SIZE / rowbytes;
	shownw = width < fi.fi_width ? width : fi.fi_width;
	shownh = height < fi.fi_height ? height : fi.fi_height;
	return 0;
}

static void
savenv(root, info)
	char *root;
	struct nx_info *info;
{
	char tmp[1100], path[1100];
	int fd;

	sprintf(tmp, "%s/nvram.tmp", root);
	sprintf(path, "%s/nvram", root);
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0)
		return;
	if (write(fd, info->nvram, NX_NVRAM) != NX_NVRAM) {
		close(fd);
		unlink(tmp);
		return;
	}
	close(fd);
	rename(tmp, path);
}

static void
census(info)
	struct nx_info *info;
{
	unsigned long i;

	for (i = 0; i < info->ncensus; i++)
		fprintf(stderr, "next: census %s %lx size %d value %lx pc %lx\n",
		    info->census[i].write ? "write" : "read", info->census[i].addr,
		    info->census[i].size, info->census[i].value, info->census[i].pc);
	fprintf(stderr, "next: last %lx pc %lx\n", info->lasta, info->lastpc);
	fprintf(stderr, "next: ticks %lu ints %lu io %lu faults %lu level %lu stat %lx mask %lx\n",
	    info->ticks, info->ints, info->io, info->faults, info->level, info->stat, info->mask);
}

/*
 * Host key codes (ADB, Amiga, Atari) to US ASCII for the console; 1 is
 * Shift, 2 Control.
 */
static char *keymap[] = {
	"asdfhgzxcv\\bqweryt123465=97-80]o"
	"u[ip\015lj'k;\\,/nm.\011 `\177\015\033\002\000\001\000\000\000\000\000\000\000"
	"\000.\000*\000+\000\000\000\000\000/\015\000-\000\000=01234567\00089\000\000\000"
	"\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\177\000\000\000\000\000\001\000\002\000\000",
	"`1234567890-=\\\0000qwertyuiop[]\000123"
	"asdfghjkl;'\000\000456\\zxcvbnm,./\000.789"
	" \177\011\015\015\033\177\000\000\000-\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000/*+\000"
	"\001\001\000\002\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000",
	"\000\0331234567890-=\177\011qwertyuiop[]\015\002as"
	"dfghjkl;'`\001\\zxcvbnm,./\001\000\000 \000\000\000\000\000\000"
	"\000\000\000\000\000\000\000\000\000\000-\000\000\000+\000\000\000\000\177\000\000\000\000\000\000\000\000\000\000\000\000"
	"\000\000\000\000\000/*7894561230.\015\000\000\000\000\000\000\000\000\000\000\000\000\000",
};

/* a key going down: its character into the guest's console ring */
static void
typed(map, code, mods)
	char *map;
	int code, mods;
{
	static char *plain = "1234567890-=[]\\;',./`", *shifted = "!@#$%^&*()_+{}|:\"<>?~";
	struct hf_keys *k = (struct hf_keys *)(NX_PHYS + HF_KEYS);
	char *p;
	int c;

	if (code >= 128 || ((c = map[code]) < ' ' && c != '\r' && c != '\t' && c != 033))
		return;
	if (mods & 1 && c >= 'a' && c <= 'z')
		c -= 32;
	else if (mods & 1 && (p = strchr(plain, c)) != 0)
		c = shifted[p - plain];
	if (mods & 2 && (c & 0x5f) >= '@' && (c & 0x5f) <= '_')
		c &= 0x1f;
	if (k->head - k->tail < sizeof k->buf)
		k->buf[k->head++ & 255] = c;
}

/* the child: the screen out, keys and mouse in, NVRAM kept, until the machine ends */
static int
host(root)
	char *root;
{
	static unsigned char shown[NX_VRAM_SIZE];
	static struct nx_info info;
	struct fbcmap colors;
	unsigned short grey[256];
	struct pollfd p;
	struct inev ev;
	struct evinfo ei;
	char *map;
	unsigned long seq = 0, shownc = 0;
	unsigned char *vram = (unsigned char *)NX_VRAM;
	int i, n, mods = 0, loops = 0, dump = getenv("NEXT_CENSUS") != 0;

	if (nh_display_open(&display, "/dev/fb0", "NeXT", 1) < 0 || display.info.fi_depth != 8) {
		perror("next: display (8 bits per pixel)");
		kill(machine, SIGTERM);
		return 1;
	}
	for (i = 0; i < 256; i++)
		grey[i] = i * 257;
	memset(&colors, 0, sizeof colors);
	colors.cm_count = 256;
	colors.cm_red = colors.cm_green = colors.cm_blue = grey;
	ioctl(display.framebuffer, FBIOPUTCMAP, &colors);
	memset(display.mapping, 255, display.info.fi_size);
	memset(shown, 0, sizeof shown);
	if (ioctl(display.keyboard, EVIOCGINFO, &ei) < 0)
		ei.ei_kset = EVK_ADB;
	map = keymap[ei.ei_kset == EVK_AMIGA ? 1 : ei.ei_kset == EVK_IKBD ? 2 : 0];
	while (getppid() == machine) {
		p.fd = display.keyboard;
		p.events = POLLIN;
		p.revents = 0;
		poll(&p, 1, 20);
		while ((n = nh_input(display.keyboard, &ev)) > 0) {
			if (ev.ie_type == IE_DROP)
				mods = 0;
			else if (ev.ie_type == IE_KEY && ev.ie_code == KEY_COMMAND)
				mods = ev.ie_value != 0 ? mods | 4 : mods & ~4;
			else if (ev.ie_type == IE_KEY && ev.ie_code == 0x0c && mods & 4 && ev.ie_value) {
				kill(machine, SIGTERM);
				return 0;
			} else if (ev.ie_type == IE_KEY && ev.ie_code < 128 &&
			    (i = map[ev.ie_code]) > 0 && i < 3)
				mods = ev.ie_value != 0 ? mods | i : mods & ~i;
			else if (ev.ie_type == IE_KEY && ev.ie_value)
				typed(map, ev.ie_code, mods);
		}
		if (n < 0 && errno != EAGAIN && errno != EINTR)
			break;
		present(vram, shown);
		if (++loops % 10 == 0 && ioctl(bridge, NX_INFO, &info) == 0) {
			if (info.nvseq != seq) {
				seq = info.nvseq;
				savenv(root, &info);
			}
			if (dump && info.ncensus != shownc && loops % 100 == 0) {
				shownc = info.ncensus;
				census(&info);
			}
		}
	}
	if (dump && ioctl(bridge, NX_INFO, &info) == 0)
		census(&info);
	nh_display_close(&display);
	return 0;
}

/* nothing of ours may sit where the machine's memory goes */
static int
conflicts()
{
	char name[40];
	prmap_t *maps;
	int fd, n, i, result = -1;

	sprintf(name, "/proc/%05ld", (long)getpid());
	if ((fd = open(name, O_RDONLY)) < 0)
		return -1;
	if (ioctl(fd, PIOCNMAP, &n) == 0 && n > 0 && n <= 4096 &&
	    (maps = calloc(n + 8, sizeof *maps)) != 0) {
		if (ioctl(fd, PIOCMAP, maps) == 0) {
			result = 0;
			for (i = 0; i < n; i++)
				if ((unsigned long)maps[i].pr_vaddr < NX_TOP ||
				    (unsigned long)maps[i].pr_vaddr + maps[i].pr_size > NX_PHYS &&
				    (unsigned long)maps[i].pr_vaddr < NX_PHYS + NX_LIMIT) {
					errno = EBUSY;
					result = -1;
				}
		}
		free(maps);
	}
	close(fd);
	return result;
}

static void
defaults(nv)
	unsigned char *nv;
{
	unsigned long sum = 0;
	int i;

	memset(nv, 0, NX_NVRAM);
	nv[0] = 0x94;
	nv[1] = 0x0f;
	nv[2] = 0x40;
	for (i = 0; i < 30; i += 2) {
		sum += nv[i] << 8 | nv[i + 1];
		if (sum >= 0x10000)
			sum -= 0xffff;
	}
	sum = 0xffff - sum;
	nv[30] = sum >> 8;
	nv[31] = sum;
}

/*
 * The ROM at 0 and 0x01000000, RAM in bank 0, VRAM from its file; each
 * also at NX_PHYS above, where it stays when the guest's MMU takes the
 * low addresses.
 */
static int
twice(a, len, prot, flags, fd, off)
	unsigned long a, len;
	int prot, flags, fd;
	off_t off;
{
	return mmap((void *)a, len, prot, flags | MAP_FIXED, fd, off) == (void *)-1 ||
	    mmap((void *)(NX_PHYS + a), len, prot, flags | MAP_FIXED, fd, off) == (void *)-1 ? -1 : 0;
}

static struct {
	unsigned long a, n;
} windows[] = NX_WINDOWS;

static int
memory(root, rom)
	char *root, *rom;
{
	char path[1100];
	struct stat st;
	struct rlimit rl;
	unsigned long size, off;
	int fd, zero, i;

	if ((zero = open("/dev/zero", O_RDWR)) < 0)
		return -1;
	if (!rom) {
		/* no ROM: its address range reads as zeros */
		if (twice(NX_ROM, NX_ROM_SIZE, PROT_READ, MAP_PRIVATE, zero, (off_t)0) < 0)
			return -1;
	} else {
		if ((fd = open(rom, O_RDONLY)) < 0 || fstat(fd, &st) < 0 || st.st_size != NX_ROM_SIZE) {
			errno = fd < 0 ? errno : EINVAL;
			return -1;
		}
		if (twice(NX_ROM, NX_ROM_SIZE, PROT_READ | PROT_EXEC, MAP_SHARED, fd, (off_t)0) < 0 ||
		    twice(NX_ROM2, NX_ROM_SIZE, PROT_READ | PROT_EXEC, MAP_SHARED, fd, (off_t)0) < 0)
			return -1;
		close(fd);
	}
	/* every view of guest memory counts against the mapping limit */
	if (getrlimit(RLIMIT_VMEM, &rl) == 0) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_VMEM, &rl);
	}
	/* the host filesystem's region and the I/O windows follow the RAM in the object */
	for (ramsize = 0x2000000UL; ramsize >= 0x800000UL; ramsize >>= 1) {
		size = ramsize + HF_SIZE + NX_WINSIZE;
		if (ioctl(bridge, NX_RAMNEW, &size) == 0 &&
		    twice(NX_RAM, ramsize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED, bridge, (off_t)0) == 0)
			break;
		munmap((void *)NX_RAM, ramsize);
		munmap((void *)(NX_PHYS + NX_RAM), ramsize);
	}
	if (ramsize < 0x800000UL)
		return -1;
	close(zero);
	if (twice(HF_BASE, HF_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED, bridge, (off_t)ramsize) < 0)
		return -1;
	for (i = 0, off = ramsize + HF_SIZE; i < sizeof windows / sizeof windows[0]; off += windows[i++].n)
		if (twice(windows[i].a, windows[i].n, PROT_READ | PROT_WRITE, MAP_SHARED, bridge, (off_t)off) < 0)
			return -1;
	sprintf(path, "%s/vram", root);
	if ((fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0 ||
	    ftruncate(fd, (off_t)NX_VRAM_SIZE) < 0 ||
	    twice(NX_VRAM, NX_VRAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0) < 0)
		return -1;
	close(fd);
	return 0;
}

#define BE(p) ((unsigned long)(p)[0] << 24 | (p)[1] << 16 | (p)[2] << 8 | (p)[3])
#define CPU_M68K 6
#define ARGS (NX_RAM + 0x00400000UL)	/* boot frame; the kernel and its tables lie below */
#define MG_FLAGS 369
#define MGF_VERBOSE 8
#define MAXBOOT 0x1000UL
#define BANKTOP 0x2000UL	/* the kernel keeps its message ring just above a bank's end */

static unsigned long adbinit;	/* the desktop-bus driver's start routine */
static unsigned long kmsend;	/* the keyboard unit's command sender */
static unsigned long kmpaint;	/* the console's character painter */
static unsigned long kminput, timeout, hz;	/* the console's input, the callout, its rate */
static unsigned long msgp;		/* address of the kernel's pointer to its message ring */
static unsigned long msgdone;

/* kernel routines and tables the host filesystem hooks into, found by name */
static struct ksym {
	char *name;
	unsigned long addr;
} ksyms[] = {
	{ "_kalloc" }, { "_vm_info_init" }, { "_uiomove" }, { "_vfs_add" }, { "_vfs_unlock" },
	{ "_bcopy" }, { "_specvp" }, { "_active_u" }, { "_vfssw" }, { "_vfsNVFS" },
	{ "_setconf" }, { "_rootfs" },
};
#define K_KALLOC 0
#define K_VMINFO 1
#define K_UIOMOVE 2
#define K_VFSADD 3
#define K_VFSUNLOCK 4
#define K_BCOPY 5
#define K_SPECVP 6
#define K_ACTIVEU 7
#define K_VFSSW 8
#define K_VFSEND 9
#define K_SETCONF 10
#define K_ROOTFS 11

/* the kernel's routines that drive the board, and the variables they use, found by name */
static struct ksym bsyms[] = {
	{ "_clock_value" }, { "_kern_timestamp" }, { "_delay" }, { "__set_timer" }, { "_callout_dispatch" },
	{ "_clock_timer_init" }, { "_install_scanned_intr" }, { "_eventc_latch" }, { "_eventc_l" }, { "_timer_csr" },
	{ "_intrstat" }, { "_scr2" }, { "_rtc_get" }, { "_rtc_set" }, { "_startrtclock" }, { "_nvram_check" },
	{ "_nvram_set" }, { "_rtc_power_down" }, { "_mon_reset" }, { "_zsintsetup" }, { "_enintsetup" }, { "ipl6" },
};
/* drivers of devices this machine has none of: their probes find nothing */
static struct ksym absent[] = {
	{ "_fcdriver" }, { "_scdriver" }, { "_ocdriver" }, { "_npdriver" }, { "_sounddriver" }, { "_dspdriver" },
	{ "_endriver" },
};
#define B_CLOCKVALUE 0
#define B_TIMESTAMP 1
#define B_DELAY 2
#define B_SETTIMER 3
#define B_CALLOUT 4
#define B_TIMERINIT 5
#define B_INSTALL 6
#define B_LATCH 7
#define B_EVL 8
#define B_CSR 9
#define B_INTRSTAT 10
#define B_SCR2 11
#define B_RTCGET 12
#define B_RTCSET 13
#define B_RTCSTART 14
#define B_NVGET 15
#define B_NVSET 16
#define B_POWER 17
#define B_MONRESET 18
#define B_ZSSETUP 19
#define B_ENSETUP 20
#define B_IPL6 21

/* the address of the named symbol in the slice at off, 0 if none */
static unsigned long
symbol(fd, off, symoff, nsyms, stroff, strsize, name)
	int fd;
	unsigned long off, symoff, nsyms, stroff, strsize;
	char *name;
{
	static unsigned char sym[12 * 8192], str[256 * 1024];
	unsigned long i, x, value = 0;

	if (nsyms > 8192 || strsize >= sizeof str)
		return 0;
	if (lseek(fd, (off_t)(off + symoff), SEEK_SET) >= 0 && read(fd, sym, nsyms * 12) == nsyms * 12 &&
	    lseek(fd, (off_t)(off + stroff), SEEK_SET) >= 0 && read(fd, str, strsize) == strsize) {
		str[strsize] = 0;
		for (i = 0; i < nsyms; i++) {
			x = BE(sym + 12 * i);
			if (x < strsize && strcmp((char *)str + x, name) == 0)
				value = BE(sym + 12 * i + 8);
		}
	}
	return value;
}

/* the kernel's m68k slice, a Mach-O: segments to their addresses; the entry, or 0 */
static unsigned long
loadkernel(path)
	char *path;
{
	unsigned char hdr[2048], *cmd;
	unsigned long off = 0, n, i, entry = 0, ncmds, size;
	unsigned long va, vsz, fo, fsz;
	int fd;

	if ((fd = open(path, O_RDONLY)) < 0 || read(fd, hdr, sizeof hdr) < 32)
		return 0;
	if (BE(hdr) == 0xcafebabe) {
		n = BE(hdr + 4);
		for (i = 0; i < n && i < 8; i++)
			if (BE(hdr + 8 + 20 * i) == CPU_M68K)
				off = BE(hdr + 16 + 20 * i);
		if (!off || lseek(fd, (off_t)off, SEEK_SET) < 0 || read(fd, hdr, sizeof hdr) < 32)
			return 0;
	}
	if (BE(hdr) != 0xfeedface || BE(hdr + 4) != CPU_M68K || BE(hdr + 12) != 2)
		return 0;
	ncmds = BE(hdr + 16);
	size = BE(hdr + 20);
	if (size > sizeof hdr - 28)
		return 0;
	for (cmd = hdr + 28, i = 0; i < ncmds && cmd < hdr + 28 + size; i++, cmd += BE(cmd + 4)) {
		if (BE(cmd) == 1) {
			va = BE(cmd + 24);
			vsz = BE(cmd + 28);
			fo = BE(cmd + 32);
			fsz = BE(cmd + 36);
			if (!fsz && va < NX_RAM)
				continue;	/* the guard page */
			if (va < NX_RAM || va + vsz > NX_RAM + ramsize || fsz > vsz ||
			    (fsz && (lseek(fd, (off_t)(off + fo), SEEK_SET) < 0 ||
			    read(fd, (void *)va, fsz) != fsz)))
				return 0;
		} else if (BE(cmd) == 2) {
			msgp = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_pmsgbuf");
			adbinit = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20),
			    "_adb_initialize");
			kmpaint = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_kmpaint");
			kmsend = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_km_send");
			kminput = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_km_input");
			timeout = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_timeout");
			hz = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_hz");
			for (n = 0; n < sizeof ksyms / sizeof ksyms[0]; n++)
				ksyms[n].addr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16),
				    BE(cmd + 20), ksyms[n].name);
			for (n = 0; n < sizeof bsyms / sizeof bsyms[0]; n++)
				bsyms[n].addr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16),
				    BE(cmd + 20), bsyms[n].name);
			for (n = 0; n < sizeof absent / sizeof absent[0]; n++)
				absent[n].addr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16),
				    BE(cmd + 20), absent[n].name);
		} else if (BE(cmd) == 5)
			entry = BE(cmd + 12 + 4 * BE(cmd + 12));	/* the last word of the state: pc */
		if (BE(cmd + 4) < 8)
			return 0;
	}
	close(fd);
	return entry;
}

/*
 * The boot program's call frame, as the kernel reads it from its entry
 * stack: [1] the monitor globals, [2] boot flags, [4] device name, [5]
 * argument string, [7] slot address base, [9] memory banks, [10] their
 * (start, end) list, [11] hardware address, [12] the boot file's name
 * (64 bytes), which a "$BOOTFILE" symbolic link stands for.
 */
static unsigned long *
bootframe(kernel)
	char *kernel;
{
	char *name = strrchr(kernel, '/') ? strrchr(kernel, '/') + 1 : kernel;

	unsigned long *f = (unsigned long *)ARGS;
	unsigned char *b = (unsigned char *)ARGS + 0x100;

	memset((void *)ARGS, 0, MAXBOOT);
	f[1] = ARGS + 0x200;		/* zeroed globals: no monitor services */
	((unsigned char *)f[1])[MG_FLAGS] |= MGF_VERBOSE;	/* the kernel's console text shows on the screen */
	f[4] = (unsigned long)b;
	strcpy((char *)b, "hf");	/* no device probe answers to it: the root comes from the switch entry */
	f[5] = (unsigned long)b + 16;	/* empty argument string */
	f[9] = 1;
	f[10] = (unsigned long)b + 32;
	f[11] = (unsigned long)b + 48;
	f[12] = (unsigned long)b + 64;
	strncpy((char *)b + 64, name, 63);
	((unsigned long *)(b + 32))[0] = NX_RAM;
	((unsigned long *)(b + 32))[1] = NX_RAM + ramsize - BANKTOP;
	return f;
}

static char altstack[16384];

static void
carrier(sig)
	int sig;
{
}

/*
 * The kernel's console is its own text drawing on the frame buffer, and its
 * printf text also lands in a message ring.  The host console shows that
 * ring: the address of the ring's pointer comes from the kernel's symbols,
 * and the timer tick (and any fault) copies out what is new.
 */
#define MSGMAGIC 0x63061UL
#define MSGSIZE 4084UL

#define KMSTUB (HF_BASE + 0x6200UL)
#define KMHEAD (HF_BASE + 0x6ffcUL)	/* the ring ends where the keys start */
#define KMRING (KMHEAD + 4)
#define KMSIZE 0x800UL
static unsigned long kmdone;

static void
console()
{
	unsigned char *ring;
	unsigned long end;

	/* once the screen console runs, it has all the text */
	if ((end = kmpaint ? L(NX_PHYS + KMHEAD) : 0) != 0 || kmdone) {
		ring = (unsigned char *)(NX_PHYS + KMRING);
		if (end < kmdone) {
			write(1, ring + kmdone, KMSIZE - kmdone);
			kmdone = 0;
		}
		if (end > kmdone)
			write(1, ring + kmdone, end - kmdone);
		kmdone = end;
		return;
	}
	if (!msgp || (ring = (unsigned char *)L(NX_PHYS + msgp)) == 0 ||
	    (unsigned long)ring < NX_RAM || (unsigned long)ring + 12 + MSGSIZE > NX_RAM + ramsize)
		return;
	ring += NX_PHYS;
	if (L(ring) != MSGMAGIC)
		return;
	end = L(ring + 4);
	if (end > MSGSIZE)
		return;
	if (end < msgdone) {
		write(1, ring + 12 + msgdone, MSGSIZE - msgdone);
		msgdone = 0;
	}
	if (end > msgdone)
		write(1, ring + 12 + msgdone, end - msgdone);
	msgdone = end;
}

static void
tick(sig, si, uc)
	int sig;
	siginfo_t *si;
	ucontext_t *uc;
{
	static int n;
	char line[60];

	console();
	if (getenv("NEXT_PC") && ++n % 100 == 0) {
		sprintf(line, "next: pc %lx sp %lx\n", (unsigned long)uc->uc_mcontext.gregs[R_PC], (unsigned long)uc->uc_mcontext.gregs[R_SP]);
		write(2, line, strlen(line));
	}
}

/* the kernel went back to the monitor: its command line, then the end */
static void
leave(sig)
	int sig;
{
	static struct nx_info info;
	char line[120];
	unsigned long a;

	console();
	if (ioctl(bridge, NX_INFO, &info) == 0) {
		a = info.monexit & 0x7fffffffUL;
		sprintf(line, "next: the kernel returned to the monitor: %.60s\n",
		    a >= NX_RAM && a < NX_RAM + ramsize - 64 ? (char *)(NX_PHYS + a) : "");
		write(1, line, strlen(line));
		sprintf(line, "next: privileged %lu, MMU fills %lu faults %lu switches %lu flushes %lu, ticks %lu\n",
		    info.mmu[4], info.mmu[0], info.mmu[1], info.mmu[2], info.mmu[3], info.ticks);
		write(1, line, strlen(line));
	}
	kill(machine, SIGTERM);
	_exit(0);
}

/* a fault of the guest: the console first, then where it was */
static void
trap(sig, si, uc)
	int sig;
	siginfo_t *si;
	ucontext_t *uc;
{
	char line[80];

	console();
	/* a fault of the guest that could not be sent to its own handler */
	if ((unsigned long)uc->uc_mcontext.gregs[R_PC] < NX_TOP)
		sprintf(line, "next: double fault at %lx pc %lx sp %lx\n", (unsigned long)si->si_addr,
		    (unsigned long)uc->uc_mcontext.gregs[R_PC], (unsigned long)uc->uc_mcontext.gregs[R_SP]);
	else
		sprintf(line, "next: signal %d addr %lx pc %lx sp %lx\n", sig, (unsigned long)si->si_addr,
		    (unsigned long)uc->uc_mcontext.gregs[R_PC], (unsigned long)uc->uc_mcontext.gregs[R_SP]);
	write(2, line, strlen(line));
	{
		unsigned long *sp = (unsigned long *)(uc->uc_mcontext.gregs[R_SP] & ~3UL) - 8;
		int i;

		for (i = 0; i < 32 && (unsigned long)sp < NX_TOP; i += 4, sp += 4) {
			sprintf(line, "next: stack %lx: %08lx %08lx %08lx %08lx\n", (unsigned long)sp, sp[0], sp[1], sp[2], sp[3]);
			write(2, line, strlen(line));
		}
	}
	_exit(1);
}

/*
 * The root filesystem is the environment folder over host calls.  The
 * routines run in the guest kernel; here they are placed in their own
 * region, given the kernel's routines, and made the operations of the
 * a filesystem-switch entry the root setup is pointed at, with no device
 * probe and no network code.  The kernel file is not touched.
 */
struct hctx {
	long (*kalloc)(), (*vm_info_init)(), (*uiomove)(), (*vfs_add)(), (*vfs_unlock)(), (*bcopy)(), (*specvp)();
	unsigned long *active_u;
	long (*hcall)();
	unsigned long vfsops[8], vnodeops[36];
};

static int
hostfs(folder)
	char *folder;
{
	struct hctx *c = (struct hctx *)HF_CTX;
	struct hf_keys *k;
	unsigned long e, *sw;
	unsigned short *stub;
	int i;

	for (i = 0; i < sizeof ksyms / sizeof ksyms[0]; i++)
		if (!ksyms[i].addr)
			return -1;
	if (HF_CODE - HF_BASE + hfs_blob_len > HF_SIZE || hf_open(folder) < 0)
		return -1;
	memcpy((void *)HF_CODE, hfs_blob, hfs_blob_len);
	c->kalloc = (long (*)())ksyms[K_KALLOC].addr;
	c->vm_info_init = (long (*)())ksyms[K_VMINFO].addr;
	c->uiomove = (long (*)())ksyms[K_UIOMOVE].addr;
	c->vfs_add = (long (*)())ksyms[K_VFSADD].addr;
	c->vfs_unlock = (long (*)())ksyms[K_VFSUNLOCK].addr;
	c->bcopy = (long (*)())ksyms[K_BCOPY].addr;
	c->specvp = (long (*)())ksyms[K_SPECVP].addr;
	c->active_u = (unsigned long *)ksyms[K_ACTIVEU].addr;
	c->hcall = (long (*)())HF_HCALL;
	if (kminput && timeout && hz) {
		k = (struct hf_keys *)HF_KEYS;
		k->input = (long (*)())kminput;
		k->timeout = (long (*)())timeout;
		k->ticks = L(hz) / 20 ? L(hz) / 20 : 1;
	}
	e = HF_CODE + hfs_entry;
	((long (*)())e)();
	/*
	 * The kernel's root-device setup is replaced by a stub that names our
	 * filesystem type as the root's; the first free switch entry carries it.
	 */
	for (sw = (unsigned long *)ksyms[K_VFSSW].addr; sw < (unsigned long *)ksyms[K_VFSEND].addr; sw += 2) {
		if (sw[0] || sw[1])
			continue;
		strcpy((char *)HF_FSNAME, "hfs");
		sw[0] = HF_FSNAME;
		sw[1] = (unsigned long)c->vfsops;
		stub = (unsigned short *)HF_STUB;
		stub[0] = 0x23fc;			/* move.l #"hfs\0",_rootfs */
		stub[1] = 'h' << 8 | 'f';
		stub[2] = 's' << 8;
		stub[3] = ksyms[K_ROOTFS].addr >> 16;
		stub[4] = (unsigned short)ksyms[K_ROOTFS].addr;
		stub[5] = 0x4e75;			/* rts */
		stub = (unsigned short *)ksyms[K_SETCONF].addr;
		stub[0] = 0x4ef9;			/* jmp HF_STUB */
		stub[1] = HF_STUB >> 16;
		stub[2] = (unsigned short)HF_STUB;
		return 0;
	}
	return -1;
}

/*
 * Each character the console paints also goes to a ring the host reads:
 * the painter's frame setup (link, movem) moves into a stub that stores
 * the character first.  The head moves by compare-and-swap: a painter
 * that interrupts another (key echo) must not reuse its slot.
 */
static void
kmhook()
{
	unsigned short *k = (unsigned short *)kmpaint, *c = (unsigned short *)KMSTUB;
	static unsigned short code[] = {
		0x207c, 0, 0,			/* movea.l #KMHEAD,a0 */
		0x227c, 0, 0,			/* movea.l #KMRING,a1 */
		0x2010,				/* 1: move.l (a0),d0 */
		0x13af, 0x0007, 0x0800,		/* move.b 7(sp),(a1,d0.l) */
		0x2200,				/* move.l d0,d1 */
		0x5281,				/* addq.l #1,d1 */
		0x0281, 0, KMSIZE - 1,		/* andi.l #KMSIZE-1,d1 */
		0x0ed0, 0x0040,			/* cas.l d0,d1,(a0) */
		0x66e8,				/* bne.s 1b */
	};
	int n = sizeof code / sizeof code[0];

	if (kmpaint < NX_RAM || kmpaint >= NX_RAM + ramsize || k[0] != 0x4e56 || k[2] != 0x48e7)
		return;
	L(KMHEAD) = 0;
	memcpy(c, code, sizeof code);
	c[1] = KMHEAD >> 16;
	c[2] = (unsigned short)KMHEAD;
	c[4] = KMRING >> 16;
	c[5] = (unsigned short)KMRING;
	memcpy(c + n, k, 8);
	c[n + 4] = 0x4ef9;			/* jmp kmpaint + 8 */
	c[n + 5] = (kmpaint + 8) >> 16;
	c[n + 6] = (unsigned short)(kmpaint + 8);
	k[0] = 0x4ef9;				/* jmp KMSTUB */
	k[1] = KMSTUB >> 16;
	k[2] = (unsigned short)KMSTUB;
}

/*
 * The board's timer, event counter, clock chip and software interrupts
 * are reached only through the kernel's own routines; in the loaded copy
 * those become host calls.  Each is found by name and checked for the
 * code it expects; one that does not match stays as it is.
 *
 *	routine entry:	moveq #service,d0; jmp HF_HCALL+4
 *	inline access:	jsr HF_HSTUB+8*service (+ nops)
 *	HF_HCALL:	move.l 4(sp),d0; illegal; rts
 *	HF_HSTUB:	moveq #0,d0; rts (a probe that finds nothing)
 *
 * Devices the machine lacks (disks, floppy, printer, sound, ethernet, the
 * serial and keyboard units) are absent: their drivers' probes and
 * interrupt setup return at once, so nothing touches their registers.
 */
static int
inkernel(a)
	unsigned long a;
{
	return a >= NX_RAM && a < NX_RAM + ramsize;
}

/* the first n words w at or after p, within max words; 0 if none */
static unsigned short *
find(p, max, w, n)
	unsigned short *p, *w;
	int max, n;
{
	int i;

	for (; max-- > 0; p++) {
		for (i = 0; i < n && p[i] == w[i]; i++)
			;
		if (i == n)
			return p;
	}
	return 0;
}

static void
jsrstub(p, svc)
	unsigned short *p;
	int svc;
{
	unsigned long a = HF_HSTUB + 8 * svc;

	p[0] = 0x4eb9;
	p[1] = a >> 16;
	p[2] = (unsigned short)a;
}

/* the routine at a, which starts with a frame link, is the host call svc; 0 for a plain return */
static int
hcentry(a, svc)
	unsigned long a;
	int svc;
{
	unsigned short *p = (unsigned short *)a;

	if (!inkernel(a) || (p[0] != 0x4e56 && p[0] != 0x4856))
		return -1;
	if (!svc) {
		p[0] = 0x4e75;
		return 0;
	}
	p[0] = 0x7000 | svc;
	p[1] = 0x4ef9;
	p[2] = (HF_HCALL + 4) >> 16;
	p[3] = (unsigned short)(HF_HCALL + 4);
	return 0;
}

/*
 * The event counter read in line: four byte loads through the pointers
 * from the latch's to the 20-bit mask, then d0 and a0 loaded again.  It
 * becomes a host call leaving the count in d1.
 */
static int
counter(a)
	unsigned long a;
{
	unsigned short at[3], mask[3], *s, *e;

	at[0] = 0x2079;
	at[1] = bsyms[B_LATCH].addr >> 16;
	at[2] = (unsigned short)bsyms[B_LATCH].addr;
	mask[0] = 0x0281;
	mask[1] = 0x000f;
	mask[2] = 0xffff;
	if (!inkernel(a) || !(s = find((unsigned short *)a, 64, at, 3)) || !(e = find(s, 40, mask, 3)) ||
	    e[3] != 0x2079 || e[6] != 0x2010)
		return -1;
	jsrstub(s, HC_US);
	for (s += 3, e += 3; s < e; s++)
		*s = 0x4e71;
	return 0;
}

/*
 * The timer's interrupt handler, which clock_timer_init installs (pea
 * handler; pea level; bsr.l install_scanned_intr), acknowledges by reading
 * the timer's register and reads the counter.  That becomes: clear the
 * timer's bit in the status word, then the counter from the host.
 */
static int
timerint()
{
	unsigned short *p = (unsigned short *)bsyms[B_TIMERINIT].addr, at[3], evl[4], *s, *e;
	unsigned long h = 0;
	int i;

	if (!inkernel((unsigned long)p))
		return -1;
	for (i = 0; i < 200 && !h; i++)
		if (p[i] == 0x4879 && p[i + 3] == 0x4878 && p[i + 5] == 0x61ff &&
		    (unsigned long)(p + i + 6) + ((unsigned long)p[i + 6] << 16 | p[i + 7]) == bsyms[B_INSTALL].addr)
			h = (unsigned long)p[i + 1] << 16 | p[i + 2];
	at[0] = evl[0] = 0x2079;
	at[1] = bsyms[B_CSR].addr >> 16;
	at[2] = (unsigned short)bsyms[B_CSR].addr;
	evl[1] = bsyms[B_EVL].addr >> 16;
	evl[2] = (unsigned short)bsyms[B_EVL].addr;
	evl[3] = 0x1010;
	if (!inkernel(h) || !(s = find((unsigned short *)h, 16, at, 3)) || !(e = find(s, 40, evl, 4)) ||
	    e - s < 6 || e[4] != 0x2079 || e[7] != 0x2010)
		return -1;
	jsrstub(s, HC_TACK);
	for (s += 3, e += 4; s < e; s++)
		*s = 0x4e71;
	return 0;
}

/* the level 6 dispatcher reads the status word through _intrstat: that read becomes a host call */
static int
ipl6()
{
	unsigned short at[4], *s;
	unsigned long a = bsyms[B_IPL6].addr;

	at[0] = 0x2079;				/* movea.l _intrstat,a0; move.l (a0),d0 */
	at[1] = bsyms[B_INTRSTAT].addr >> 16;
	at[2] = (unsigned short)bsyms[B_INTRSTAT].addr;
	at[3] = 0x2010;
	if (!inkernel(a) || !bsyms[B_INTRSTAT].addr || !(s = find((unsigned short *)a, 64, at, 4)))
		return -1;
	jsrstub(s, HC_STAT);
	s[3] = 0x4e71;
	return 0;
}

/* callout_dispatch raises software interrupt 1 or 2 by setting SCR2 bit 24 or 25 (d1) */
static int
softint()
{
	unsigned short at[3], or[3], *s;
	unsigned long a = bsyms[B_CALLOUT].addr;

	at[0] = 0x2079;
	at[1] = bsyms[B_SCR2].addr >> 16;
	at[2] = (unsigned short)bsyms[B_SCR2].addr;
	or[0] = 0x2010;				/* move.l (a0),d0; or.l d1,d0; move.l d0,(a0) */
	or[1] = 0x8081;
	or[2] = 0x2080;
	if (!inkernel(a) || !(s = find((unsigned short *)a, 128, at, 3)) || !(s = find(s, 12, or, 3)))
		return -1;
	jsrstub(s, HC_SOFT);
	return 0;
}

static int
services()
{
	unsigned short *c = (unsigned short *)HF_HCALL;
	char *bad[30];
	int i, n = 0;

	c[0] = 0x202f;				/* move.l 4(sp),d0 */
	c[1] = 4;
	c[2] = 0x4afc;				/* illegal */
	c[3] = 0x4e75;				/* rts */
	c = (unsigned short *)HF_HSTUB;
	c[0] = 0x7000;				/* moveq #0,d0 */
	c[1] = 0x4e75;				/* rts */
	for (i = 1; i < HC_N; i++) {
		c = (unsigned short *)(HF_HSTUB + 8 * i);
		c[0] = 0x7000 | i;
		c[1] = 0x4ef9;
		c[2] = (HF_HCALL + 4) >> 16;
		c[3] = (unsigned short)(HF_HCALL + 4);
	}
	if (counter(bsyms[B_CLOCKVALUE].addr) < 0)
		bad[n++] = "clock_value";
	if (counter(bsyms[B_TIMESTAMP].addr) < 0)
		bad[n++] = "kern_timestamp";
	if (timerint() < 0)
		bad[n++] = "timer interrupt";
	if (ipl6() < 0)
		bad[n++] = "ipl6";
	if (softint() < 0)
		bad[n++] = "callout_dispatch";
	if (hcentry(bsyms[B_DELAY].addr, HC_DELAY) < 0)
		bad[n++] = "delay";
	if (hcentry(bsyms[B_SETTIMER].addr, HC_TIMER) < 0)
		bad[n++] = "_set_timer";
	if (hcentry(bsyms[B_RTCGET].addr, HC_TIME) < 0)
		bad[n++] = "rtc_get";
	if (hcentry(bsyms[B_RTCSET].addr, 0) < 0)
		bad[n++] = "rtc_set";
	if (hcentry(bsyms[B_RTCSTART].addr, 0) < 0)
		bad[n++] = "startrtclock";
	if (hcentry(bsyms[B_NVGET].addr, HC_NVGET) < 0)
		bad[n++] = "nvram_check";
	if (hcentry(bsyms[B_NVSET].addr, HC_NVSET) < 0)
		bad[n++] = "nvram_set";
	if (hcentry(bsyms[B_POWER].addr, HC_OFF) < 0)
		bad[n++] = "rtc_power_down";
	if (hcentry(bsyms[B_MONRESET].addr, 0) < 0)
		bad[n++] = "mon_reset";
	if (hcentry(bsyms[B_ZSSETUP].addr, 0) < 0)
		bad[n++] = "zsintsetup";
	if (hcentry(bsyms[B_ENSETUP].addr, 0) < 0)
		bad[n++] = "enintsetup";
	/* a driver's probe and slave probe are its first two words, its controller setup the seventh */
	for (i = 0; i < sizeof absent / sizeof absent[0]; i++)
		if (inkernel(absent[i].addr) && inkernel(L(absent[i].addr))) {
			L(absent[i].addr) = HF_HSTUB;
			if (inkernel(L(absent[i].addr + 4)))
				L(absent[i].addr + 4) = HF_HSTUB;
			if (inkernel(L(absent[i].addr + 24)))
				L(absent[i].addr + 24) = HF_HSTUB;
		} else
			bad[n++] = absent[i].name;
	for (i = 0; i < n; i++)
		fprintf(stderr, "next: not a host call: %s\n", bad[i]);
	if (!n)
		fprintf(stderr, "next: the board's routines are host calls\n");
	return n ? -1 : 0;
}

/* a host call from the guest: the request in the mailbox */
static void
hostcall(sig)
	int sig;
{
	struct hf_box *b = (struct hf_box *)(NX_PHYS + HF_BASE);
	char line[100];

	if (b->state != HF_REQUEST)
		return;
	hf_serve(b);
	if (b->op == HF_ROOT && !b->error) {
		sprintf(line, "next: root mounted from the host folder\n");
		write(1, line, strlen(line));
	}
	if (getenv("NEXT_CENSUS")) {
		sprintf(line, "next: hfs op %lu node %lu error %ld result %ld %.40s\n", b->op, b->node, b->error, b->result,
		    b->op == HF_LOOKUP || b->op == HF_CREATE || b->op == HF_REMOVE || b->op == HF_MKDIR || b->op == HF_RENAME ?
		    b->name : "");
		write(2, line, strlen(line));
	}
}

/* the ROM's stack and entry: the first two longs */
static void
reset()
{
	unsigned long ssp = L(NX_ROM), pc = L(NX_ROM + 4);

	__asm__ __volatile__("mov.l %0,%%sp\n\tjmp (%1)" : : "a" (ssp), "a" (pc));
}

/*
 * The monitor's services as host calls.  The kernel takes the monitor's
 * trap 13 vector (back to the monitor, d0 = command line) from the vector table
 * it finds at entry; it points at the host call that ends the machine.
 */
#define VECTORS (ARGS + MAXBOOT)
#define STUB (VECTORS + 0x400UL)

static void
monitor()
{
	unsigned long *v = (unsigned long *)VECTORS;
	unsigned short *c = (unsigned short *)STUB;

	memset(v, 0, 1024);
	v[45] = STUB;
	c[0] = 0x2200;				/* move.l d0,d1 */
	c[1] = 0x7000 | HC_MONITOR;		/* moveq #HC_MONITOR,d0 */
	c[2] = 0x4eb9;				/* jsr HF_HCALL+4 */
	c[3] = (HF_HCALL + 4) >> 16;
	c[4] = (unsigned short)(HF_HCALL + 4);
	c[5] = 0x60fe;				/* bra . */
}

/* the kernel's entry: its stack is the boot frame */
static void
enter(sp, pc)
	unsigned long *sp, pc;
{
	__asm__ __volatile__("mov.l %2,%%a1\n\t.short 0x4e7b,0x9801\n\tmov.l %0,%%sp\n\tjmp (%1)"
	    : : "a" (sp), "a" (pc), "g" (VECTORS) : "a1");
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	char *media = getenv("NATIVEHOST_MEDIA"), *root = getenv("NATIVEHOST_ROOT");
	char path[1100];
	struct nx_enter ne;
	struct sigaction sa;
	stack_t ss;
	struct stat st;
	pid_t child;
	int fd;

	char *kernel = 0;
	unsigned long entry = 0;

	if (argc > 2 && strcmp(argv[1], "-k") == 0)
		kernel = argv[2];
	else if (argc > 1)
		media = argv[1];
	if (!kernel && media && stat(media, &st) == 0 && st.st_size != NX_ROM_SIZE)
		kernel = media;		/* not a ROM image: the kernel */
	if (!root)
		root = ".";
	if (!media && !kernel) {
		fprintf(stderr, "next: no ROM or kernel\n");
		return 1;
	}
	nh_display_init(&display);
	if ((bridge = open("/dev/nextguest", O_RDWR)) < 0) {
		perror("next: /dev/nextguest");
		return 1;
	}
	if (conflicts() < 0 || memory(root, kernel ? 0 : media) < 0) {
		perror("next: memory or ROM (128 KB)");
		return 1;
	}
	if (kernel && (entry = loadkernel(kernel)) == 0) {
		fprintf(stderr, "next: %s: no m68k kernel\n", kernel);
		return 1;
	}
	memset(&ne, 0, sizeof ne);
	sprintf(path, "%s/nvram", root);
	if (stat(path, &st) == 0 && st.st_size == NX_NVRAM && (fd = open(path, O_RDONLY)) >= 0) {
		if (read(fd, ne.nvram, NX_NVRAM) != NX_NVRAM)
			defaults(ne.nvram);
		close(fd);
	} else
		defaults(ne.nvram);
	ne.ramsize = ramsize;
	if (screen() < 0) {
		perror("next: the host screen");
		return 1;
	}
	ne.width = width;
	ne.height = height;
	if (getenv("NEXT_CENSUS"))
		fprintf(stderr, "next: RAM %lu MB\n", ramsize >> 20);
	ne.flags = (getenv("NEXT_CENSUS") ? NXF_CENSUS : 0) | NXF_HOST;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = carrier;
	sigaction(SIGUSR2, &sa, (struct sigaction *)0);
	/* the handlers run on a stack of their own: the guest's is virtual */
	ss.ss_sp = altstack;
	ss.ss_size = sizeof altstack / 4;	/* this kernel counts it in longs */
	ss.ss_flags = 0;
	sigaltstack(&ss, (stack_t *)0);
	sa.sa_flags = SA_ONSTACK;
	sa.sa_handler = leave;
	sigaction(SIGUSR1, &sa, (struct sigaction *)0);
	sa.sa_handler = (void (*)())trap;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigaction(SIGBUS, &sa, (struct sigaction *)0);
	sigaction(SIGSEGV, &sa, (struct sigaction *)0);
	if (entry) {
		if (hostfs(root) < 0)
			fprintf(stderr, "next: host filesystem not installed\n");
		else {
			fprintf(stderr, "next: root filesystem installed\n");
			kmhook();
			sa.sa_handler = hostcall;
			sa.sa_flags = SA_ONSTACK;
			sigaction(SIGPWR, &sa, (struct sigaction *)0);
		}
		services();
	}
	/* the child shares the host filesystem's region for the keys */
	machine = getpid();
	if ((child = fork()) < 0) {
		perror("next: fork");
		return 1;
	}
	if (child == 0)
		_exit(host(root));
	{
		struct itimerval it;

		sa.sa_handler = (void (*)())tick;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigaction(SIGALRM, &sa, (struct sigaction *)0);
		it.it_interval.tv_sec = it.it_value.tv_sec = 0;
		it.it_interval.tv_usec = it.it_value.tv_usec = 20000;
		setitimer(ITIMER_REAL, &it, (struct itimerval *)0);
	}
	if (ioctl(bridge, NX_ENTER, &ne) < 0) {
		perror("next: enter");
		kill(child, SIGTERM);
		return 1;
	}
	if (entry) {
		/* no desktop-bus controller: its driver would wait on it without end */
		if (adbinit >= NX_RAM && adbinit < NX_RAM + ramsize)
			*(unsigned short *)adbinit = 0x4e75;	/* rts */
		/* nor a keyboard unit: its commands time out at once, not after 100000 polls */
		if (kmsend >= NX_RAM && kmsend < NX_RAM + ramsize && *(unsigned short *)kmsend == 0x4856) {
			((unsigned short *)kmsend)[0] = 0x203c;	/* move.l #0x40000000,d0 */
			((unsigned short *)kmsend)[1] = 0x4000;
			((unsigned short *)kmsend)[2] = 0;
			((unsigned short *)kmsend)[3] = 0x4e75;	/* rts */
		}
		monitor();
		enter(bootframe(kernel), entry);
	} else
		reset();
	return 1;
}
