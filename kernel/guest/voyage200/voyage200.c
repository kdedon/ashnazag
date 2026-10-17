/*
 * voyage200 -- a Voyage 200 from its own reset vector.
 *
 *	native [image]
 *
 * The image (a 4 MB dump: boot code, AMS, archive) is read only.  The
 * calculator runs on a copy made at each start, with its hardware
 * routines patched (patch.c) and its archive laid out from the files in
 * $NATIVEHOST_ROOT/archive (archive.c).  RAM is $NATIVEHOST_ROOT/ram,
 * cleared at every start.  This process becomes the calculator
 * (tiplatform); a child shows the LCD, turns host keys into key rows
 * and writes changed archive entries back to their files.  Cmd-Q quits.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/procfs.h>
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

#define L(a) (*(__volatile__ unsigned long *)(a))
#define HOLD 3		/* clock ticks a key stays down at least */

static struct nh_display display;
static int bridge = -1;
static pid_t calculator;
static char *root;
extern void patch(), arc_load(), arc_save();
extern int mprotect();

/*
 * Host key (ADB code) to matrix position, row * 8 + column; the second
 * table for keys whose shifted symbol is a key of its own.
 */
static unsigned char plain[128], shifted[128], pair[128];
static char *layout[] = {
	"\x00\x4a", "\x01\x0a", "\x02\x12", "\x03\x1a", "\x04\x2a", "\x05\x22",
	"\x06\x09", "\x07\x11", "\x08\x19", "\x09\x21", "\x0b\x29", "\x0c\x4b",
	"\x0d\x0b", "\x0e\x13", "\x0f\x1b", "\x10\x2b", "\x11\x23", "\x12\x0d",
	"\x13\x0e", "\x14\x0f", "\x15\x15", "\x16\x17", "\x17\x16", "\x18\x38",
	"\x19\x1f", "\x1a\x1d", "\x1b\x48", "\x1c\x1e", "\x1d\x4d", "\x1f\x43",
	"\x20\x33", "\x22\x3b", "\x23\x37", "\x24\x49", "\x25\x42", "\x26\x32",
	"\x27\x18", "\x28\x3a", "\x29\x41", "\x2b\x27", "\x2c\x28", "\x2d\x31",
	"\x2e\x39", "\x2f\x4e", "\x31\x20", "\x32\x4f", "\x33\x40", "\x35\x46",
	"\x36\x01", "\x38\x02", "\x3a\x00", "\x3b\x04", "\x3c\x06", "\x3d\x07",
	"\x3e\x05", "\x41\x4e", "\x43\x3f", "\x45\x44", "\x4b\x28", "\x4c\x36",
	"\x4e\x48", "\x52\x4d", "\x53\x0d", "\x54\x0e", "\x55\x0f", "\x56\x15",
	"\x57\x16", "\x58\x17", "\x59\x1d", "\x5b\x1e", "\x5c\x1f", "\x60\x3c",
	"\x61\x2c", "\x62\x1c", "\x63\x14", "\x64\x0c", "\x65\x3e", "\x67\x3d",
	"\x6d\x45", "\x6f\x7f", "\x75\x3d", "\x76\x4c", "\x78\x24", "\x7a\x34", "\x7b\x02",
	"\x7c\x00", "\x7d\x01",
	0
};
static char *shifts[] = {
	"\x18\x44", "\x1c\x3f", "\x19\x25", "\x1d\x26", "\x16\x30", 0
};
/* keys that press two: Home is HOME (diamond Q), Help CATALOG (2nd 2); End is OFF (2nd, then ON) */
static char *pairs[] = {
	"\x73\x4b\x01", "\x72\x0e\x00", 0
};
#define KEY_2ND 0x00
#define KEY_SHIFT 0x02
#define MX_ON 0x7f	/* ON is no matrix key */
#define HK_END 0x77

static void
keymaps()
{
	int i;

	memset(plain, 0xff, sizeof plain);
	memset(shifted, 0xff, sizeof shifted);
	memset(pair, 0xff, sizeof pair);
	for (i = 0; layout[i]; i++)
		plain[(unsigned char)layout[i][0]] = layout[i][1];
	for (i = 0; shifts[i]; i++)
		shifted[(unsigned char)shifts[i][0]] = shifts[i][1];
	for (i = 0; pairs[i]; i++) {
		plain[(unsigned char)pairs[i][0]] = pairs[i][1] | 0x80;
		pair[(unsigned char)pairs[i][0]] = pairs[i][2];
	}
}

static void
press(k, c)
	struct ti_keys *k;
	int c;
{
	if (c == MX_ON)
		k->on = 1;
	else
		k->rows[c >> 3] |= 1 << (c & 7);
}

static long
now()
{
	struct nh_clock c;

	return nh_clock_read(&c) < 0 ? 0 : (long)c.ticks;
}

/* the 240x128 LCD, scaled by whole steps and centred */
static void
present(lcd, shown)
	unsigned char *lcd, *shown;
{
	unsigned char *fb = (unsigned char *)display.mapping + display.info.fi_offset, *p;
	unsigned int x, y, dx, dy, ink, s, x0, y0, rb = display.info.fi_rowbytes;

	if (!memcmp(shown, lcd, 3840))
		return;
	memcpy(shown, lcd, 3840);
	s = display.info.fi_width / 240;
	if (display.info.fi_height / 128 < s)
		s = display.info.fi_height / 128;
	x0 = (display.info.fi_width - 240 * s) / 2;
	y0 = (display.info.fi_height - 128 * s) / 2;
	for (y = 0; y < 128; y++)
		for (x = 0; x < 240; x++) {
			ink = shown[y * 30 + x / 8] & 0x80 >> x % 8 ? 0 : 255;
			for (dy = 0; dy < s; dy++) {
				p = fb + (y0 + y * s + dy) * rb + x0 + x * s;
				for (dx = 0; dx < s; dx++)
					p[dx] = ink;
			}
		}
}

/* the child: LCD out, keys in, until the calculator ends */
static int
host()
{
	static unsigned char shown[3840], dark[3840];
	unsigned char down[128], code[128];
	long since[128], t, offat = 0;
	struct ti_keys k, sent;
	struct ti_info info;
	struct fbcmap colors;
	unsigned short grey[256];
	struct pollfd p;
	struct inev ev;
	unsigned long flash = 0;
	long changed = 0;
	int i, n, shift = 0, own, cmd = 0;

	if (nh_display_open(&display, "/dev/fb0", "Voyage 200", 1) < 0 ||
	    display.info.fi_depth != 8 || display.info.fi_width < 240 || display.info.fi_height < 128) {
		perror("voyage200: display (8 bits per pixel)");
		kill(calculator, SIGTERM);
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
	memset(down, 0, sizeof down);
	memset(&sent, 0, sizeof sent);
	keymaps();
	while (getppid() == calculator) {
		p.fd = display.keyboard;
		p.events = POLLIN;
		p.revents = 0;
		poll(&p, 1, 20);
		t = now();
		while ((n = nh_input(display.keyboard, &ev)) > 0) {
			if (ev.ie_type == IE_DROP) {
				memset(down, 0, sizeof down);
				shift = cmd = 0;
				continue;
			}
			if (ev.ie_type != IE_KEY || ev.ie_code >= 128)
				continue;
			if (ev.ie_code == 0x37)
				cmd = ev.ie_value != 0;
			else if (ev.ie_code == 0x38 || ev.ie_code == 0x7b)
				shift = ev.ie_value != 0;
			if (cmd && ev.ie_code == 0x0c && ev.ie_value) {
				kill(calculator, SIGTERM);
				arc_save();
				return 0;
			}
			if (ev.ie_value && ev.ie_code == HK_END) {
				offat = t ? t : 1;
				continue;
			}
			if (ev.ie_value) {
				down[ev.ie_code] = 1;
				since[ev.ie_code] = t;
				code[ev.ie_code] = shift && shifted[ev.ie_code] != 0xff ?
				    shifted[ev.ie_code] | 0x80 : plain[ev.ie_code];
			} else if (down[ev.ie_code])
				down[ev.ie_code] = 2;
		}
		if (n < 0 && errno != EAGAIN && errno != EINTR)
			break;
		memset(&k, 0, sizeof k);
		own = 0;
		for (i = 0; i < 128; i++) {
			if (down[i] == 2 && t - since[i] >= HOLD)
				down[i] = 0;
			if (!down[i])
				continue;
			if (code[i] != 0xff && code[i] != KEY_SHIFT) {
				press(&k, code[i] & 0x7f);
				if (pair[i] != 0xff)
					press(&k, pair[i]);
				own |= code[i] & 0x80;
			}
		}
		/* OFF: 2nd, released, then ON; AMS takes ON with 2nd held as ON */
		if (offat && t - offat < HOLD)
			press(&k, KEY_2ND);
		else if (offat && t - offat >= 2 * HOLD && t - offat < 3 * HOLD)
			press(&k, MX_ON);
		else if (offat && t - offat >= 3 * HOLD)
			offat = 0;
		if (shift && !own)
			k.rows[0] |= 1 << KEY_SHIFT;
		if (memcmp(&k, &sent, sizeof k) && ioctl(bridge, TI_KEYS, &k) == 0)
			sent = k;
		if (ioctl(bridge, TI_INFO, &info) < 0)
			continue;
		if (info.dark)
			present(dark, shown);	/* LCD off: nothing lit */
		else if (info.lcd + 3840 <= TI_RAM_SIZE)
			present((unsigned char *)info.lcd, shown);
		if (info.flash != flash) {
			flash = info.flash;
			changed = t ? t : 1;
		} else if (changed && t - changed >= 30) {
			arc_save();
			changed = 0;
		}
	}
	if (changed)
		arc_save();
	nh_display_close(&display);
	return 0;
}

/* nothing of ours may sit where the calculator's memory goes */
static int
conflicts()
{
	char name[40];
	prmap_t *maps;
	int fd, n, i, result = -1;
	unsigned long lo, hi;

	sprintf(name, "/proc/%05ld", (long)getpid());
	if ((fd = open(name, O_RDONLY)) < 0)
		return -1;
	if (ioctl(fd, PIOCNMAP, &n) == 0 && n > 0 && n <= 4096 &&
	    (maps = calloc(n + 1, sizeof *maps)) != 0) {
		if (ioctl(fd, PIOCMAP, maps) == 0) {
			result = 0;
			for (i = 0; i < n; i++) {
				lo = (unsigned long)maps[i].pr_vaddr;
				hi = lo + maps[i].pr_size;
				if (lo < TI_HOST + TI_HOST_SIZE ||
				    (lo < TI_ALIAS + TI_FLASH_SIZE && hi > TI_ALIAS)) {
					errno = EBUSY;
					result = -1;
				}
			}
		}
		free(maps);
	}
	close(fd);
	return result;
}

static int
copyfile(from, to)
	char *from, *to;
{
	char buf[8192];
	int in, out, n;

	if ((in = open(from, O_RDONLY)) < 0)
		return -1;
	if ((out = open(to, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, n) != n) {
			n = -1;
			break;
		}
	close(in);
	if (close(out) < 0 || n < 0) {
		unlink(to);
		return -1;
	}
	return 0;
}

/*
 * RAM at 0 and its mirrors; the Flash copy read-only at TI_FLASH and
 * writable at TI_ALIAS; the register pages (and the open bus AMS posts
 * keys to) as memory with the constants AMS reads (battery comparator
 * high, ON up, no key);
 * the service stubs at TI_HOST.
 */
static unsigned long pages[] = { TI_IO, TI_IO + 0x100000, TI_OPEN, TI_HOST };

static int
memory(media)
	char *media;
{
	char path[1100];
	struct stat st;
	unsigned long a;
	int ram, flash = -1, zero, i;

	sprintf(path, "%s/ram", root);
	unlink(path);
	if ((ram = open(path, O_RDWR | O_CREAT | O_EXCL, 0600)) < 0 ||
	    ftruncate(ram, (off_t)TI_RAM_SIZE) < 0)
		return -1;
	for (a = 0; a < TI_FLASH; a += TI_RAM_SIZE)
		if (mmap((void *)a, TI_RAM_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_SHARED | MAP_FIXED, ram, 0) == (void *)-1)
			return -1;
	close(ram);
	for (a = 0; a < TI_FLASH; a += 0x1000)	/* each page present, so its cache mode is set at entry */
		L(a) = 0;
	sprintf(path, "%s/flash.run", root);
	unlink(path);
	if (copyfile(media, path) < 0 || (flash = open(path, O_RDWR)) < 0 || fstat(flash, &st) < 0 ||
	    st.st_size != TI_FLASH_SIZE) {
		errno = flash < 0 ? errno : EINVAL;
		return -1;
	}
	if (mmap((void *)TI_FLASH, TI_FLASH_SIZE, PROT_READ | PROT_EXEC,
	    MAP_SHARED | MAP_FIXED, flash, 0) == (void *)-1 ||
	    mmap((void *)TI_ALIAS, TI_FLASH_SIZE, PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_FIXED, flash, 0) == (void *)-1)
		return -1;
	close(flash);
	unlink(path);
	if ((zero = open("/dev/zero", O_RDWR)) < 0)
		return -1;
	for (i = 0; i < sizeof pages / sizeof pages[0]; i++)
		if (mmap((void *)pages[i], 0x1000, PROT_READ | PROT_WRITE | (pages[i] == TI_HOST ? PROT_EXEC : 0),
		    MAP_PRIVATE | MAP_FIXED, zero, 0) == (void *)-1)
			return -1;
	close(zero);
	*(char *)TI_IO = 4;
	*(char *)(TI_IO + 0x1a) = *(char *)(TI_IO + 0x1b) = 0xff;
	for (i = 0; i < TS_N; i++)
		L(TI_HOST + 8 * i) = 0x4afc4e75UL;	/* illegal; rts */
	return mprotect((void *)TI_HOST, TI_HOST_SIZE, PROT_READ | PROT_EXEC);
}

static void
carrier(sig)
	int sig;
{
}

/* the boot code's stack and entry: the first two Flash longs */
static void
reset()
{
	unsigned long ssp = L(TI_FLASH), pc = L(TI_FLASH + 4);

	__asm__ __volatile__("mov.l %0,%%sp\n\tjmp (%1)" : : "a" (ssp), "a" (pc));
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	char *media = getenv("NATIVEHOST_MEDIA");
	struct ti_enter te;
	struct sigaction sa;
	pid_t child;

	if (argc > 1)
		media = argv[1];
	if (!(root = getenv("NATIVEHOST_ROOT")))
		root = ".";
	if (strlen(root) > 1000) {
		fprintf(stderr, "voyage200: NATIVEHOST_ROOT too long\n");
		return 1;
	}
	nh_display_init(&display);
	if (!media) {
		fprintf(stderr, "voyage200: no image\n");
		return 1;
	}
	if (conflicts() < 0 || memory(media) < 0) {
		perror("voyage200: memory");
		return 1;
	}
	if ((L(TI_FLASH + 4) & 0xffe00001UL) != TI_FLASH || L(TI_FLASH) > TI_RAM_SIZE) {
		fprintf(stderr, "voyage200: %s has no reset vector\n", media);
		return 1;
	}
	arc_load(root);
	memset(&te, 0, sizeof te);
	patch(&te);
	if ((bridge = open("/dev/tiplatform", O_RDWR)) < 0) {
		perror("voyage200: /dev/tiplatform");
		return 1;
	}
	calculator = getpid();
	if ((child = fork()) < 0) {
		perror("voyage200: fork");
		return 1;
	}
	if (child == 0)
		_exit(host());
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = carrier;
	sigaction(SIGUSR2, &sa, (struct sigaction *)0);
	te.flags = getenv("VOYAGE200_CENSUS") ? TIF_CENSUS : 0;
	if (ioctl(bridge, TI_ENTER, &te) < 0) {
		perror("voyage200: enter");
		kill(child, SIGTERM);
		return 1;
	}
	reset();
	return 1;
}
