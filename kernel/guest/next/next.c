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
#define WIDTH 1120
#define HEIGHT 832
#define ROWBYTES 280
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
	unsigned char *s = vram + row * ROWBYTES;
	int x;

	for (x = 0; x < WIDTH; x++)
		line[x] = 255 - 85 * (s[x >> 2] >> (6 - 2 * (x & 3)) & 3);
}

/* the changed rows of the 1120x832 screen, 2 bits to 8, centred or clipped */
static void
present(vram, shown)
	unsigned char *vram, *shown;
{
	static unsigned char line[WIDTH];
	unsigned char *fb = (unsigned char *)display.mapping + display.info.fi_offset;
	unsigned int w = WIDTH, h = HEIGHT, x0 = 0, y0 = 0, y, rb = display.info.fi_rowbytes;

	if (display.info.fi_width > WIDTH)
		x0 = (display.info.fi_width - WIDTH) / 2;
	else
		w = display.info.fi_width;
	if (display.info.fi_height > HEIGHT)
		y0 = (display.info.fi_height - HEIGHT) / 2;
	else
		h = display.info.fi_height;
	for (y = 0; y < h; y++) {
		if (!memcmp(shown + y * ROWBYTES, vram + y * ROWBYTES, ROWBYTES))
			continue;
		memcpy(shown + y * ROWBYTES, vram + y * ROWBYTES, ROWBYTES);
		scale(y, vram, line);
		memcpy(fb + (y0 + y) * rb + x0, line, w);
	}
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

/* the child: the screen out, keys and mouse in, NVRAM kept, until the machine ends */
static int
host(root)
	char *root;
{
	static unsigned char shown[HEIGHT * ROWBYTES];
	static struct nx_info info;
	struct fbcmap colors;
	unsigned short grey[256];
	struct pollfd p;
	struct inev ev;
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
	while (getppid() == machine) {
		p.fd = display.keyboard;
		p.events = POLLIN;
		p.revents = 0;
		poll(&p, 1, 20);
		while ((n = nh_input(display.keyboard, &ev)) > 0) {
			if (ev.ie_type == IE_DROP)
				mods = 0;
			else if (ev.ie_type == IE_KEY && ev.ie_code == KEY_COMMAND)
				mods = ev.ie_value != 0;
			else if (ev.ie_type == IE_KEY && ev.ie_code == 0x0c && mods && ev.ie_value) {
				kill(machine, SIGTERM);
				return 0;
			}
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
				if ((unsigned long)maps[i].pr_vaddr < NX_LIMIT) {
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

/* the ROM at 0 and 0x01000000, RAM in bank 0, VRAM from its file */
static int
memory(root, rom)
	char *root, *rom;
{
	char path[1100];
	struct stat st;
	int fd, zero;

	if ((zero = open("/dev/zero", O_RDWR)) < 0)
		return -1;
	if (!rom) {
		/* no ROM: its address range reads as zeros */
		if (mmap((void *)NX_ROM, NX_ROM_SIZE, PROT_READ, MAP_PRIVATE | MAP_FIXED,
		    zero, 0) == (void *)-1)
			return -1;
	} else {
		if ((fd = open(rom, O_RDONLY)) < 0 || fstat(fd, &st) < 0 || st.st_size != NX_ROM_SIZE) {
			errno = fd < 0 ? errno : EINVAL;
			return -1;
		}
		if (mmap((void *)NX_ROM, NX_ROM_SIZE, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED,
		    fd, 0) == (void *)-1 ||
		    mmap((void *)NX_ROM2, NX_ROM_SIZE, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED,
		    fd, 0) == (void *)-1)
			return -1;
		close(fd);
	}
	for (ramsize = 0x2000000UL; ramsize >= 0x800000UL; ramsize >>= 1)
		if (mmap((void *)NX_RAM, ramsize, PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_PRIVATE | MAP_FIXED, zero, 0) != (void *)-1)
			break;
	if (ramsize < 0x800000UL)
		return -1;
	close(zero);
	sprintf(path, "%s/vram", root);
	if ((fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0 ||
	    ftruncate(fd, (off_t)NX_VRAM_SIZE) < 0 ||
	    mmap((void *)NX_VRAM, NX_VRAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
	    fd, 0) == (void *)-1)
		return -1;
	close(fd);
	return 0;
}

#define BE(p) ((unsigned long)(p)[0] << 24 | (p)[1] << 16 | (p)[2] << 8 | (p)[3])
#define CPU_M68K 6
#define ARGS (NX_RAM + 0x00400000UL)	/* boot frame; the kernel and its tables lie below */
#define MAXBOOT 0x1000UL
#define BANKTOP 0x2000UL	/* the kernel keeps its message ring just above a bank's end */

static unsigned long adbinit;	/* the desktop-bus driver's start routine */
static unsigned long msgp;		/* address of the kernel's pointer to its message ring */
static unsigned long msgdone;

/* kernel routines and tables the host filesystem hooks into, found by name */
static struct ksym {
	char *name;
	unsigned long addr;
} ksyms[] = {
	{ "_kalloc" }, { "_vm_info_init" }, { "_uiomove" }, { "_vfs_add" }, { "_vfs_unlock" },
	{ "_bcopy" }, { "_active_u" }, { "_vfssw" }, { "_vfsNVFS" },
	{ "_setconf" }, { "_rootfs" },
};
#define K_KALLOC 0
#define K_VMINFO 1
#define K_UIOMOVE 2
#define K_VFSADD 3
#define K_VFSUNLOCK 4
#define K_BCOPY 5
#define K_ACTIVEU 6
#define K_VFSSW 7
#define K_VFSEND 8
#define K_SETCONF 9
#define K_ROOTFS 10

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
			for (n = 0; n < sizeof ksyms / sizeof ksyms[0]; n++)
				ksyms[n].addr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16),
				    BE(cmd + 20), ksyms[n].name);
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
 * (start, end) list, [11] hardware address, [12] 64 bytes of boot data.
 */
static unsigned long *
bootframe()
{
	unsigned long *f = (unsigned long *)ARGS;
	unsigned char *b = (unsigned char *)ARGS + 0x100;

	memset((void *)ARGS, 0, MAXBOOT);
	f[1] = ARGS + 0x200;		/* zeroed globals: no monitor services */
	f[4] = (unsigned long)b;
	strcpy((char *)b, "hf");	/* no device probe answers to it: the root comes from the switch entry */
	f[5] = (unsigned long)b + 16;	/* empty argument string */
	f[9] = 1;
	f[10] = (unsigned long)b + 32;
	f[11] = (unsigned long)b + 48;
	f[12] = (unsigned long)b + 64;
	((unsigned long *)(b + 32))[0] = NX_RAM;
	((unsigned long *)(b + 32))[1] = NX_RAM + ramsize - BANKTOP;
	return f;
}

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

static void
console()
{
	unsigned char *ring;
	unsigned long end;

	if (!msgp || (ring = (unsigned char *)L(msgp)) == 0 ||
	    (unsigned long)ring < NX_RAM || (unsigned long)ring + 12 + MSGSIZE > NX_RAM + ramsize ||
	    L(ring) != MSGMAGIC)
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
tick(sig)
	int sig;
{
	console();
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
		    a >= NX_RAM && a < NX_RAM + ramsize - 64 ? (char *)a : "");
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
	sprintf(line, "next: signal %d pc %lx sp %lx\n", sig,
	    (unsigned long)uc->uc_mcontext.gregs[R_PC],
	    (unsigned long)uc->uc_mcontext.gregs[R_SP]);
	write(2, line, strlen(line));
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
	long (*kalloc)(), (*vm_info_init)(), (*uiomove)(), (*vfs_add)(), (*vfs_unlock)(), (*bcopy)();
	unsigned long *active_u;
	unsigned long doorbell;
	unsigned long vfsops[8], vnodeops[36];
};

static int
hostfs(folder)
	char *folder;
{
	struct hctx *c = (struct hctx *)HF_CTX;
	unsigned long e, *sw;
	unsigned short *stub;
	int i, zero;

	for (i = 0; i < sizeof ksyms / sizeof ksyms[0]; i++)
		if (!ksyms[i].addr)
			return -1;
	if ((zero = open("/dev/zero", O_RDWR)) < 0 ||
	    mmap((void *)HF_BASE, HF_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_FIXED,
	    zero, 0) == (void *)-1)
		return -1;
	close(zero);
	if (HF_CODE - HF_BASE + hfs_blob_len > HF_SIZE || hf_open(folder) < 0)
		return -1;
	memcpy((void *)HF_CODE, hfs_blob, hfs_blob_len);
	c->kalloc = (long (*)())ksyms[K_KALLOC].addr;
	c->vm_info_init = (long (*)())ksyms[K_VMINFO].addr;
	c->uiomove = (long (*)())ksyms[K_UIOMOVE].addr;
	c->vfs_add = (long (*)())ksyms[K_VFSADD].addr;
	c->vfs_unlock = (long (*)())ksyms[K_VFSUNLOCK].addr;
	c->bcopy = (long (*)())ksyms[K_BCOPY].addr;
	c->active_u = (unsigned long *)ksyms[K_ACTIVEU].addr;
	c->doorbell = NX_DOORBELL + 4;
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

/* a host call from the guest: the request in the mailbox */
static void
hostcall(sig)
	int sig;
{
	struct hf_box *b = (struct hf_box *)HF_BASE;

	char line[100];

	if (b->state != HF_REQUEST)
		return;
	hf_serve(b);
	if (b->op == HF_ROOT && !b->error) {
		sprintf(line, "next: root mounted from the host folder\n");
		write(1, line, strlen(line));
	}
	if (getenv("NEXT_CENSUS")) {
		sprintf(line, "next: hfs op %lu node %lu error %ld result %ld\n", b->op, b->node, b->error, b->result);
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
 * it finds at entry; it points at a store to the doorbell.
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
	c[0] = 0x23c0;				/* move.l d0,doorbell */
	c[1] = NX_DOORBELL >> 16;
	c[2] = NX_DOORBELL & 0xffff;
	c[3] = 0x60fe;				/* bra . */
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
	if (getenv("NEXT_CENSUS"))
		fprintf(stderr, "next: RAM %lu MB\n", ramsize >> 20);
	ne.flags = getenv("NEXT_CENSUS") ? NXF_CENSUS : 0;
	if ((bridge = open("/dev/nextguest", O_RDWR)) < 0) {
		perror("next: /dev/nextguest");
		return 1;
	}
	machine = getpid();
	if ((child = fork()) < 0) {
		perror("next: fork");
		return 1;
	}
	if (child == 0)
		_exit(host(root));
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = carrier;
	sigaction(SIGUSR2, &sa, (struct sigaction *)0);
	sa.sa_handler = leave;
	sigaction(SIGUSR1, &sa, (struct sigaction *)0);
	sa.sa_handler = (void (*)())trap;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGBUS, &sa, (struct sigaction *)0);
	sigaction(SIGSEGV, &sa, (struct sigaction *)0);
	if (entry) {
		if (hostfs(root) < 0)
			fprintf(stderr, "next: host filesystem not installed\n");
		else {
			fprintf(stderr, "next: root filesystem installed\n");
			sa.sa_handler = hostcall;
			sa.sa_flags = 0;
			sigaction(SIGPWR, &sa, (struct sigaction *)0);
		}
	}
	{
		struct itimerval it;

		sa.sa_handler = tick;
		sa.sa_flags = 0;
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
		monitor();
		enter(bootframe(), entry);
	} else
		reset();
	return 1;
}
