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
 * VRAM is $NATIVEHOST_ROOT.vram and NVRAM $NATIVEHOST_ROOT.nvram.  This
 * process becomes the machine (nextguest); a child shows the 2-bit
 * framebuffer and keeps the NVRAM file.  On an 8-bit host screen the
 * kernel draws 8-bit gray into the screen itself.  Cmd-Q quits.
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
#include "sndout.h"
#include "hostcall.h"

extern unsigned char hfs_blob[];
extern unsigned long hfs_blob_len, hfs_entry, fb_entry[];
#define FB_PROBE 0
#define FB_INFO 1
#define FB_NOP 2
#define FB_CURSOR 3
#define FB_UNCURSOR 4
#define FB_MAP 5
int hf_open();
void hf_serve();

#define L(a) (*(volatile unsigned long *)(a))
static unsigned long width, height, rowbytes;	/* the guest's screen: the host's, 2 bits per pixel */
static unsigned long shownw, shownh, shownox, shownoy;	/* where the host screen shows it */
static int direct;		/* the guest draws 8 bits a pixel into the host screen */
static unsigned long frame, vramend;	/* the guest's frame address, the end of the screen memory mapped */

#define NX_PAGE 8192UL
#define MIN_WIDTH 640
#define MIN_HEIGHT 480
static unsigned long ramsize;	/* the largest of 32, 16, 8 MB that free memory holds */
static unsigned long kmconi;		/* the console info the kernel took from the frame buffer driver */

static struct nh_display display;
static int bridge = -1;
static pid_t machine;

/* the host's key codes: command is 0x37, Q 0x0c */
#define KEY_COMMAND 0x37

/*
 * The changed rows of the screen, 2 bits to 8.  A host screen smaller than the
 * guest's shows it scaled down and centred: of the guest pixels that land on one
 * host pixel the darkest wins, so one-pixel lines survive.
 */
static void
present(vram, shown)
	unsigned char *vram, *shown;
{
	static unsigned short colx[2032];
	static unsigned char line[2032];
	static unsigned long passes;
	unsigned char *fb = (unsigned char *)display.mapping + display.info.fi_offset +
	    shownoy * display.info.fi_rowbytes + shownox, *s;
	unsigned long x, y, e, r, oy, rb = display.info.fi_rowbytes;
	int v;

	if (!colx[width - 1])
		for (x = 0; x < width; x++)
			colx[x] = x * shownw / width;
	for (y = 0; y < height; y = e) {
		oy = y * shownh / height;
		for (e = y + 1; e < height && e * shownh / height == oy; e++)
			;
		if (!memcmp(shown + y * rowbytes, vram + y * rowbytes, (e - y) * rowbytes))
			continue;
		memcpy(shown + y * rowbytes, vram + y * rowbytes, (e - y) * rowbytes);
		if (!passes++)
			fprintf(stderr, "next: present pass\n");
		memset(line, 0, shownw);
		for (r = y; r < e; r++)
			for (s = vram + r * rowbytes, x = 0; x < width; x++)
				if ((v = s[x >> 2] >> (6 - 2 * (x & 3)) & 3) > line[colx[x]])
					line[colx[x]] = v;
		for (x = 0; x < shownw; x++)
			fb[oy * rb + x] = 255 - 85 * line[x];
	}
}

/* the host screen's size, the guest's within its video memory */
/*
 * The host screen's frame in ROOT.vram, as the 2-bit path keeps it, for the census.
 * Written when ROOT.vram.req exists, which is then removed: a dump every second
 * kept the host disk busy.
 */
static void
dumpscreen(root, always)
	char *root;
	int always;
{
	char path[1100], req[1100];
	int fd;

	sprintf(req, "%s.vram.req", root);
	if (!always && access(req, 0) != 0)
		return;
	sprintf(path, "%s.vram", root);
	if ((fd = open(path, O_WRONLY | O_CREAT, 0600)) >= 0) {
		(void)write(fd, (char *)frame, rowbytes * height);
		close(fd);
	}
	unlink(req);
}

static int
screen()
{
	struct fbinfo fi;
	int fd = direct ? -1 : open("/dev/fb0", O_RDWR);

	if (direct)
		fi = display.info;
	else if (fd < 0 || ioctl(fd, FBIOGINFO, &fi) < 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	} else
		close(fd);
	if (direct && fi.fi_depth == 8 && fi.fi_offset % NX_PAGE + fi.fi_rowbytes * fi.fi_height <= NX_VRAM_SIZE &&
	    fi.fi_width >= MIN_WIDTH && fi.fi_height >= MIN_HEIGHT) {
		width = fi.fi_width;
		height = fi.fi_height;
		rowbytes = fi.fi_rowbytes;
		frame = NX_VRAM + fi.fi_offset % NX_PAGE;
		return 0;
	}
	direct = 0;
	/* at most 127 longs a row: the cursor's row length is a moveq */
	width = (fi.fi_width > 2032 ? 2032 : fi.fi_width < MIN_WIDTH ? MIN_WIDTH : fi.fi_width) & ~15UL;
	rowbytes = width / 4;
	height = fi.fi_height > 4095 ? 4095 : fi.fi_height < MIN_HEIGHT ? MIN_HEIGHT : fi.fi_height;
	if (height > NX_VRAM_SIZE / rowbytes)
		height = NX_VRAM_SIZE / rowbytes;
	shownw = width;
	shownh = height;
	if (fi.fi_width * height <= fi.fi_height * width && fi.fi_width < width) {
		shownw = fi.fi_width;
		shownh = height * fi.fi_width / width;
	} else if (fi.fi_height < height) {
		shownh = fi.fi_height;
		shownw = width * fi.fi_height / height;
	}
	shownox = (fi.fi_width - shownw) / 2;
	shownoy = (fi.fi_height - shownh) / 2;
	return 0;
}

static void
savenv(root, info)
	char *root;
	struct nx_info *info;
{
	char tmp[1100], path[1100];
	int fd;

	sprintf(tmp, "%s.nvram.tmp", root);
	sprintf(path, "%s.nvram", root);
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

/* NeXT key codes by position: the unshifted character each key types */
static char nextkeys[] = "\0\0\0\\][iop" "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" "\177=-890" "\0\0\0\0\0" "`" "\0\0\0"
	"\r';l,./zxcvbmn asdfgkjh\tqweruyt\033" "1234765";

/* an event word for the event driver */
static void
post(w)
	unsigned long w;
{
	struct hf_keys *k = (struct hf_keys *)(NX_PHYS + HF_KEYS);

	if (k->ehead - k->etail < sizeof k->ev / sizeof k->ev[0]) {
		k->ev[k->ehead & 63] = w;
		k->ehead++;
	}
}

/* a key or modifier change as the keyboard reports it: modifiers (valid bit, command, shifts, control), up bit, code */
static void
keyword(map, code, down, mods)
	char *map;
	int code, down, mods;
{
	unsigned long w = (mods & 4 ? 8 : 0) | (mods & 1 ? 2 : 0) | (mods & 2 ? 1 : 0);
	int c, i;

	w <<= 8;
	c = code < 128 ? map[code] : 0;
	if (c >= ' ' || c == '\r' || c == '\t' || c == 033 || c == 8) {
		if (c == 8)
			c = 0177;
		for (i = 0; i < sizeof nextkeys - 1 && nextkeys[i] != c; i++)
			;
		if (i < sizeof nextkeys - 1)
			w |= 0x8000 | (down ? 0 : 0x80) | i;
	}
	post(w);
}

/* mouse motion and buttons: deltas of at most 63, subtracted by the driver; buttons low when down */
static void
mouseword(dx, dy, buttons)
	long dx, dy;
	int buttons;
{
	long x, y;

	do {
		x = dx > 63 ? 63 : dx < -63 ? -63 : dx;
		y = dy > 63 ? 63 : dy < -63 ? -63 : dy;
		post(HF_MOUSE | (-y & 0x7f) << 9 | (buttons & 2 ? 0 : 0x100) | (-x & 0x7f) << 1 | (buttons & 1 ? 0 : 1));
		dx -= x;
		dy -= y;
	} while (dx || dy);
}

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
static struct sndout so;

/* the guest's queued output to the host's sound service, a tenth of a second ahead */
static void
pump()
{
	static unsigned long gen, rate, tk, off, ends[HF_SNDBUFS], played;
	struct hf_snd *sd = (struct hf_snd *)(NX_PHYS + HF_SND);
	unsigned long b, e, head = sd->head;
	long n, k;

	if (sd->gen != gen) {
		gen = sd->gen;
		if (rate)
			so_flush(&so);
		tk = sd->tail = sd->from;
		off = 0;
		played = 0;
		head = sd->head;
	}
	if (tk == head && sd->tail == tk)
		return;
	if ((sd->rate ? sd->rate : 44100) != rate) {
		rate = sd->rate ? sd->rate : 44100;
		so_start(&so, rate << 16, SNDE_S16, 2);
		played = 0;
	}
	for (n = so_due(&so, (long)rate / 10); n > 0 && tk != head; ) {
		b = sd->buf[tk % HF_SNDBUFS][0];
		e = sd->buf[tk % HF_SNDBUFS][1];
		if (b < NX_RAM || e > NX_RAM + ramsize || e < b)
			e = b;
		k = (long)((e - b) / 4 - off);
		if (k > n)
			k = n;
		if (k > 0) {
			so_put(&so, (char *)(NX_PHYS + b) + 4 * off, k);
			off += k;
			n -= k;
		}
		if (off >= (e - b) / 4) {
			ends[tk++ % HF_SNDBUFS] = so.so_taken;
			off = 0;
			if (!played++)
				fprintf(stderr, "next: sound out at %lu Hz\n", rate);
		}
	}
	while (sd->tail != tk && (long)(so_now(&so) - ends[sd->tail % HF_SNDBUFS]) >= 0)
		sd->tail++;
}

static void console();

static int
host(root)
	char *root;
{
	static unsigned char shown[NX_VRAM_SIZE];
	static struct nx_info info;
	struct fbcmap colors;
	unsigned short grey[256];
	struct pollfd p[2];
	struct inev ev;
	long dx, dy;
	int buttons = 0;
	struct evinfo ei;
	char *map;
	unsigned long seq = 0, shownc = 0, coni = 0;
	unsigned char *vram = (unsigned char *)NX_VRAM;
	int i, n, mods = 0, loops = 0, dump = getenv("NEXT_CENSUS") != 0;

	if ((!direct && nh_display_open(&display, "/dev/fb0", "NeXT", 1) < 0) || display.info.fi_depth != 8) {
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
	if (!direct)
		memset(display.mapping, 255, display.info.fi_size);
	memset(shown, 0, sizeof shown);
	if (ioctl(display.keyboard, EVIOCGINFO, &ei) < 0)
		ei.ei_kset = EVK_ADB;
	map = keymap[ei.ei_kset == EVK_AMIGA ? 1 : ei.ei_kset == EVK_IKBD ? 2 : 0];
	so_init(&so);
	while (getppid() == machine) {
		p[0].fd = display.keyboard;
		p[1].fd = display.mouse;
		p[0].events = p[1].events = POLLIN;
		p[0].revents = p[1].revents = 0;
		poll(p, display.mouse >= 0 ? 2 : 1, 20);
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
			if (ev.ie_type == IE_KEY)
				keyword(map, ev.ie_code, ev.ie_value != 0, mods);
		}
		if (n < 0 && errno != EAGAIN && errno != EINTR)
			break;
		dx = dy = 0;
		i = buttons;
		while (display.mouse >= 0 && nh_input(display.mouse, &ev) > 0)
			if (ev.ie_type == IE_REL && ev.ie_code == IE_RELX)
				dx += ev.ie_value;
			else if (ev.ie_type == IE_REL && ev.ie_code == IE_RELY)
				dy += ev.ie_value;
			else if (ev.ie_type == IE_BTN && ev.ie_code >= 1 && ev.ie_code <= 2)
				buttons = ev.ie_value ? buttons | ev.ie_code : buttons & ~ev.ie_code;
		if (dx || dy || buttons != i)
			mouseword(dx, dy, buttons);
		if (!direct)
			present(vram, shown);
		pump();
		console();
		if (++loops % 10 == 0 && ioctl(bridge, NX_INFO, &info) == 0) {
			if (info.nvseq != seq) {
				seq = info.nvseq;
				savenv(root, &info);
			}
			if (dump && kmconi >= NX_RAM && kmconi < NX_RAM + ramsize && L(NX_PHYS + kmconi + 8) != coni) {
				coni = L(NX_PHYS + kmconi + 8);
				fprintf(stderr, "next: console %lux%lu, %lu pixels a long\n", coni,
				    L(NX_PHYS + kmconi + 16), L(NX_PHYS + kmconi));
			}
			if (dump && direct)
				dumpscreen(root, 0);
			if (dump && info.ncensus != shownc && loops % 100 == 0) {
				shownc = info.ncensus;
				census(&info);
			}
		}
	}
	console();
	if (dump && direct)
		dumpscreen(root, 1);
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
					fprintf(stderr, "next: %lx-%lx is in the machine's space\n", (unsigned long)maps[i].pr_vaddr,
					    (unsigned long)maps[i].pr_vaddr + maps[i].pr_size);
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
	unsigned long size, off, sum = 0;
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
	if (direct) {
		off = display.info.fi_offset / NX_PAGE * NX_PAGE;
		size = display.info.fi_size - off < NX_VRAM_SIZE ? display.info.fi_size - off : NX_VRAM_SIZE;
		vramend = NX_VRAM + size;
		if (twice(NX_VRAM, (size + 4095) & ~4095UL, PROT_READ | PROT_WRITE, MAP_SHARED,
		    display.framebuffer, (off_t)off) < 0)
			return -1;
		/* the guest's MMU finds only pages the host has mapped in */
		for (off = 0; off < size; off += 4096)
			sum += *(volatile unsigned char *)(NX_VRAM + off) + *(volatile unsigned char *)(NX_PHYS + NX_VRAM + off);
		return 0;
	}
	sprintf(path, "%s.vram", root);
	if ((fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0 ||
	    ftruncate(fd, (off_t)NX_VRAM_SIZE) < 0 ||
	    twice(NX_VRAM, NX_VRAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0) < 0)
		return -1;
	close(fd);
	return 0;
}

#define BE(p) ((unsigned long)(p)[0] << 24 | (p)[1] << 16 | (p)[2] << 8 | (p)[3])
#define CPU_M68K 6
#define ARGS (NX_RAM + ramsize - BANKTOP + 0x8000UL)	/* boot frame and monitor globals, kept for good: the kernel's entry stack grows down from it */
#define MG_FLAGS 369
#define MGF_VERBOSE 8
#define MAXBOOT 0x1000UL
#define BANKTOP 0x10000UL	/* above the bank's end: the kernel's message ring, then ARGS */

static unsigned long adbinit;	/* the desktop-bus driver's start routine */
static unsigned long kmsend;	/* the keyboard unit's command sender */
static unsigned long kmpaint;	/* the console's character painter */
static unsigned long kmpopup, kmbig;	/* the console panel's placement and its large size */
static unsigned long kminput, timeout, hz;	/* the console's input, the callout, its rate */
static unsigned long evkintr, evmintr, evopen;	/* the event driver's keyboard and mouse entries, its open flag */
static unsigned long vidon, vidoff;	/* the frame buffer's retrace interrupt enable and disable */
static unsigned long vidprobe;		/* the frame buffer probe, which walks the drivers' table */
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
	{ "_copyin" }, { "_copyout" }, { "_copyinstr" }, { "_copyoutstr" }, { "_idle_thread_continue" },
};
/* drivers of devices this machine has none of: their probes find nothing */
static struct ksym absent[] = {
	{ "_fcdriver" }, { "_scdriver" }, { "_ocdriver" }, { "_npdriver" }, { "_sounddriver" }, { "_dspdriver" },
	{ "_endriver" },
};
/* what the audio server's output needs: found by name */
static struct ksym ssyms[] = {
	{ "_softint_sched" }, { "_vm_map_lookup_entry" }, { "_kernel_map" }, { "_dma_start" }, { "_dma_abort" },
	{ "_kern_serv_call_proc" },
};
/* the DSP's and the DMA channels' setup, start and stop */
static struct ksym nodsp[] = {
	{ "_dsp_dev_init" }, { "_dsp_dev_reset_hard" }, { "_dma_init" }, { "_dma_close" }, { "_dma_start" },
	{ "_dma_cleanup" }, { "_dma_abort" },
};
/* the pmap's flushes and the routines that change its tables, found by name */
static struct ksym psyms[] = {
	{ "_pflush_super" }, { "_pflush_user" }, { "_tlb_update_read" }, { "_tlb_update_write" }, { "_pmove_crp" },
	{ "_kernel_pmap" }, { "_page_size" }, { "_pmap_pte_valid" }, { "_pmap_enter_mapping" },
	{ "_pmap_remove_range" }, { "_pmap_protect" }, { "_pmap_destroy" }, { "_pagemove" },
	{ "_pmap_remove_all" }, { "_pmap_copy_on_write" }, { "_pmap_change_wiring" }, { "_pmap_clear_modify" },
	{ "_pmap_clear_reference" },
};
#define P_CRP 4
#define P_KPMAP 5
#define P_PGSIZE 6
#define P_VALID 7	/* (pmap, va): its page's entry, which a routine below may change */
#define P_ENTER 8	/* (pmap, va, ...) */
#define P_REMOVE 9	/* (pmap, start, end) */
#define P_PROTECT 10	/* (pmap, start, end, prot) */
#define P_DESTROY 11	/* (pmap) */
#define P_MOVE 12	/* (from, to, size), in the kernel's map */
#define P_RMALL 13	/* on: routines that change the entries they look up with P_VALID */
#define P_N 18
#define P_RET P_N	/* a patched routine's return */
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
#define B_COPYIN 22
#define B_COPYOUT 23
#define B_COPYSTR 24
#define B_COPYOSTR 25
#define B_IDLE 26

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
			kmpopup = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_kmpopup");
			kmbig = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_km_big");
			kmconi = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_km_coni");
			kmsend = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_km_send");
			kminput = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_km_input");
			timeout = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_timeout");
			hz = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_hz");
			evkintr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_ev_k_intr");
			evmintr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_ev_m_intr");
			evopen = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_eventsOpen");
			vidon = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_vidInterruptEnable");
			vidoff = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_vidInterruptDisable");
			vidprobe = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16), BE(cmd + 20), "_vidProbeForFB");
			for (n = 0; n < sizeof nodsp / sizeof nodsp[0]; n++)
				nodsp[n].addr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16),
				    BE(cmd + 20), nodsp[n].name);
			for (n = 0; n < sizeof ksyms / sizeof ksyms[0]; n++)
				ksyms[n].addr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16),
				    BE(cmd + 20), ksyms[n].name);
			for (n = 0; n < sizeof bsyms / sizeof bsyms[0]; n++)
				bsyms[n].addr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16),
				    BE(cmd + 20), bsyms[n].name);
			for (n = 0; n < sizeof ssyms / sizeof ssyms[0]; n++)
				ssyms[n].addr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16),
				    BE(cmd + 20), ssyms[n].name);
			for (n = 0; n < sizeof absent / sizeof absent[0]; n++)
				absent[n].addr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16),
				    BE(cmd + 20), absent[n].name);
			for (n = 0; n < P_N; n++)
				psyms[n].addr = symbol(fd, off, BE(cmd + 8), BE(cmd + 12), BE(cmd + 16),
				    BE(cmd + 20), psyms[n].name);
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
 * and the display loop, apart from the guest's process, copies out what is new.
 */
#define MSGMAGIC 0x63061UL
#define MSGSIZE 4084UL

#define KMSTUB (HF_BASE + 0x6200UL)
#define KMHEAD (HF_BASE + 0x6ffcUL)	/* the ring ends where the keys start */
#define KMRING (KMHEAD + 4)
#define KMSIZE 0x800UL
static unsigned long kmdone;
static int kmlive;

static void
console()
{
	unsigned char *ring;
	unsigned long end;

	/* once the screen console runs, it has all the text; the head may wrap to 0 */
	if (kmpaint && ((end = L(NX_PHYS + KMHEAD)) != 0 || kmlive)) {
		kmlive = 1;
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

	if (++n % 100 == 0) {
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

/* a fault of the guest: where it was */
static void
trap(sig, si, uc)
	int sig;
	siginfo_t *si;
	ucontext_t *uc;
{
	char line[80];

	/* a fault of the guest that could not be sent to its own handler */
	if ((unsigned long)uc->uc_mcontext.gregs[R_PC] < NX_TOP)
		sprintf(line, "next: double fault at %lx pc %lx sp %lx\n", (unsigned long)si->si_addr,
		    (unsigned long)uc->uc_mcontext.gregs[R_PC], (unsigned long)uc->uc_mcontext.gregs[R_SP]);
	else
		sprintf(line, "next: signal %d addr %lx pc %lx sp %lx\n", sig, (unsigned long)si->si_addr,
		    (unsigned long)uc->uc_mcontext.gregs[R_PC], (unsigned long)uc->uc_mcontext.gregs[R_SP]);
	write(2, line, strlen(line));
	/* the boot stack ends at the kernel's start: the first access fault's frame is its highest */
	{
		unsigned short *f;

		for (f = (unsigned short *)(NX_PHYS + NX_RAM + 0x300); f > (unsigned short *)(NX_PHYS + NX_RAM); f--)
			if (f[3] == 0x7008) {
				sprintf(line, "next: access fault pc %lx address %lx\n", *(unsigned long *)(f + 1),
				    *(unsigned long *)(f + 10));
				write(2, line, strlen(line));
				break;
			}
	}
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
/* a request takes more than the stub's own stack: a read or write buffers 8 KB */
static unsigned long hfstack[8192];

struct hctx {
	long (*kalloc)(), (*vm_info_init)(), (*uiomove)(), (*vfs_add)(), (*vfs_unlock)(), (*bcopy)(), (*specvp)();
	unsigned long *active_u;
	long (*hcall)();
	unsigned long vfsops[8], vnodeops[36];
};

static int inkernel(), framed();
static void fbentry();
static unsigned short *find();

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
	c->hcall = (long (*)())HC_STUB(0);
	hc_sp = hfstack + sizeof hfstack / sizeof hfstack[0];
	if (kminput && timeout && hz) {
		k = (struct hf_keys *)HF_KEYS;
		k->input = (long (*)())kminput;
		k->timeout = (long (*)())timeout;
		k->ticks = L(hz) / 20 ? L(hz) / 20 : 1;
		if (evkintr && evmintr && evopen) {
			k->kintr = (long (*)())evkintr;
			k->mintr = (long (*)())evmintr;
			k->open = evopen;
		}
	}
	e = HF_CODE + hfs_entry;
	((long (*)())e)();
	k = (struct hf_keys *)HF_KEYS;
	if (k->timeout && framed(vidon) && framed(vidoff)) {
		/* no retrace interrupt: its handler runs from the clock */
		stub = (unsigned short *)vidon;
		stub[0] = 0x4ef9;
		stub[1] = k->vidon >> 16;
		stub[2] = (unsigned short)k->vidon;
		stub = (unsigned short *)vidoff;
		stub[0] = 0x4ef9;
		stub[1] = k->vidoff >> 16;
		stub[2] = (unsigned short)k->vidoff;
	} else if (k->timeout)
		fprintf(stderr, "next: no retrace from the clock\n");
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
 * that interrupts another (key echo) must not reuse its slot.  A NUL
 * is a blank cell repainted: it stays out of the ring.
 */
static void
kmhook()
{
	unsigned short *k = (unsigned short *)kmpaint, *c = (unsigned short *)KMSTUB;
	static unsigned short code[] = {
		0x207c, 0, 0,			/* movea.l #KMHEAD,a0 */
		0x227c, 0, 0,			/* movea.l #KMRING,a1 */
		0x4a2f, 0x0007,			/* tst.b 7(sp): a blank cell's repaint */
		0x6718,				/* beq.s past the loop */
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
 * The console panel is 100x48 characters of 8x12 pixels, 80x40 when large,
 * centred with no check that it fits: on a smaller screen its border lands
 * outside the frame.  The sizes are word immediates: move.w #n,(a0) in
 * kmpopup, move.w #n,abs.l in km_big.
 */
static void
kmfit()
{
	unsigned short *p;
	unsigned long cols = (width - 48) / 8, rows = (height - 60) / 12;
	int i, n = 0;

	for (p = (unsigned short *)kmpopup, i = 0; inkernel(kmpopup) && i < 120; i++)
		if (p[i] == 0x30bc && p[i + 1] == 100 && cols < 100)
			p[i + 1] = cols, n++;
		else if (p[i] == 0x30bc && p[i + 1] == 48 && rows < 48)
			p[i + 1] = rows, n++;
	for (p = (unsigned short *)kmbig, i = 0; inkernel(kmbig) && i < 40; i++)
		if (p[i] == 0x33fc && p[i + 1] == 80 && cols < 80)
			p[i + 1] = cols, n++;
		else if (p[i] == 0x33fc && p[i + 1] == 40 && rows < 40)
			p[i + 1] = rows, n++;
	if (getenv("NEXT_CENSUS"))
		fprintf(stderr, "next: console panel %lux%lu, %d of 4 sizes\n", cols, rows, n);
}

/*
 * The frame buffer drivers' table (as cursorrow finds it): the last entry,
 * probed first, becomes the host screen's, from the turbo colour entry
 * with the probe, console info, set-up, mapping, brightness and the pointer's
 * drawing replaced.  The pointer routines' variables are read from the
 * colour entry's: movea.l abs,a0 (event globals), movea.l abs,a5 (row),
 * move.l a2,abs (drawn range), or.l d7,abs (flags), move.l a0,abs
 * (restored range).
 */
static void
fbentry()
{
	static unsigned short lea = 0x47f9, ev = 0x2079, row = 0x2a79, a2 = 0x23ca, or = 0x8fb9, a0 = 0x23c8;
	struct hf_fb *f = (struct hf_fb *)HF_FB;
	unsigned short *p, *d, *r;
	unsigned long *e, *c;
	int i;

	if (!inkernel(vidprobe) || !(p = find((unsigned short *)vidprobe, 16, &lea, 1)) ||
	    (p[-1] & 0xff00) != 0x7400 || (p[-1] & 0x7f) < 2 || p[4] != 0xc7fc || p[5] != 44) {
		fprintf(stderr, "next: no frame buffer table\n");
		return;
	}
	c = (unsigned long *)((unsigned long)p[1] << 16 | p[2]) + 11;
	e = c + 11 * ((p[-1] & 0x7f) - 1);
	if (!inkernel(c[5]) || !inkernel(c[6]) ||
	    !(d = find((unsigned short *)c[5], 16, &ev, 1)) || !(f->evp = (unsigned long *)L(d + 1)) ||
	    !(d = find(d, 40, &row, 1)) || !(f->rowpix = (unsigned long *)L(d + 1)) ||
	    !(d = find(d, 80, &a2, 1)) || !(f->drawn[0] = (unsigned long *)L(d + 1)) ||
	    !(d = find(d + 3, 120, &a2, 1)) || !(f->drawn[1] = (unsigned long *)L(d + 1)) ||
	    !(d = find(d, 8, &or, 1)) || !(f->flags = (unsigned long *)L(d + 1)) ||
	    !(r = find((unsigned short *)c[6], 80, &a0, 1)) || !(f->restored[0] = (unsigned long *)L(r + 1)) ||
	    !(r = find(r + 3, 80, &a0, 1)) || !(f->restored[1] = (unsigned long *)L(r + 1))) {
		fprintf(stderr, "next: no pointer routines\n");
		return;
	}
	memset(f->info, 0, sizeof f->info);
	f->info[0] = 4;			/* pixels a long */
	f->info[1] = rowbytes;
	f->info[2] = width;
	f->info[3] = rowbytes;		/* the row in pixels */
	f->info[4] = height;
	f->info[6] = 0xffffffffUL;	/* white, light and dark gray, black */
	f->info[7] = 0xaaaaaaaaUL;
	f->info[8] = 0x55555555UL;
	f->info[16] = f->info[17] = frame;
	f->info[18] = rowbytes * height;
	f->info[19] = f->info[20] = frame + rowbytes * height;
	f->info[21] = vramend - f->info[19];	/* the console's spare memory after the frame */
	for (i = 0; i < 11; i++)
		e[i] = c[i];
	e[0] = HF_CODE + fb_entry[FB_PROBE];
	e[1] = HF_CODE + fb_entry[FB_INFO];
	e[2] = e[7] = HF_CODE + fb_entry[FB_NOP];
	f->map = (long (*)())c[3];
	e[3] = HF_CODE + fb_entry[FB_MAP];
	e[5] = HF_CODE + fb_entry[FB_CURSOR];
	e[6] = HF_CODE + fb_entry[FB_UNCURSOR];
}

/*
 * The mono cursor's save and draw routines take the row as a constant,
 * 70 longs (1120 pixels), not the console info's row.  The probe gives
 * the drivers' table: moveq #last,d2; lea table,a3; move.w d2,d3;
 * muls.w #size,d3.  In each entry's routines 5 and 6, a moveq #70,Dn
 * followed by #313 (the older DMA chip's test) becomes the screen's row.
 */
static void
cursorrow()
{
	static unsigned short lea = 0x47f9;
	unsigned short *p, *r;
	unsigned long t, f, n;
	int e, i, j, k, done = 0;

	if (!inkernel(vidprobe) || !(p = find((unsigned short *)vidprobe, 16, &lea, 1)) ||
	    (p[-1] & 0xff00) != 0x7400 || p[4] != 0xc7fc) {
		fprintf(stderr, "next: no cursor row\n");
		return;
	}
	t = (unsigned long)p[1] << 16 | p[2];
	for (e = 0; e <= (p[-1] & 0x7f); e++)
		for (i = 5; i <= 6; i++) {
			n = t + e * p[5] + 4 * i;
			if (!inkernel(n) || !inkernel(f = L(n)))
				continue;
			r = (unsigned short *)f;
			for (j = 0; j < 64; j++) {
				if ((r[j] & 0xf1ff) != 0x7046)
					continue;
				for (k = 1; k < 8 && r[j + k] != 0x0139; k++)
					;
				if (k < 8) {
					r[j] = (r[j] & 0xff00) | rowbytes / 4;
					done++;
				}
			}
		}
	if (!done)
		fprintf(stderr, "next: no cursor row\n");
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

/* a routine in the kernel starting with link or pea */
static int
framed(a)
	unsigned long a;
{
	return inkernel(a) && (*(unsigned short *)a == 0x4e56 || *(unsigned short *)a == 0x4856);
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

/* the routine at a, which starts with a frame link, is the host call svc; 0 returns 0 */
static int
hcentry(a, svc)
	unsigned long a;
	int svc;
{
	unsigned short *p = (unsigned short *)a;

	if (!framed(a))
		return -1;
	if (!svc) {
		p[0] = 0x7000;
		p[1] = 0x4e75;
		return 0;
	}
	p[0] = 0x7000 | svc;
	p[1] = 0x4ef9;
	p[2] = (HF_HCALL + 4) >> 16;
	p[3] = (unsigned short)(HF_HCALL + 4);
	return 0;
}

/*
 * copyin or copyout at a, a host call: its first 10 bytes (link a6,#0;
 * moveq #1,d0; movec d0,sfc or dfc) go to the trampoline t, followed by a
 * jump back, for the pages the host call leaves to the routine.
 */
static int
copyentry(a, svc, t)
	unsigned long a, t;
	int svc;
{
	unsigned short *p = (unsigned short *)a, *q = (unsigned short *)t;
	int i;

	if (!inkernel(a) || p[0] != 0x4e56 || p[1] != 0 || p[2] != 0x7001 || p[3] != 0x4e7b || (p[4] & ~1) != 0)
		return -1;
	for (i = 0; i < 5; i++)
		q[i] = p[i];
	q[5] = 0x4ef9;
	q[6] = (a + 10) >> 16;
	q[7] = (unsigned short)(a + 10);
	return hcentry(a, svc);
}

/* after the routine's one-byte copy at t: its error to the caller, or the host call at a again */
static void
again(a, t)
	unsigned long a, t;
{
	unsigned short *q = (unsigned short *)t;

	q[0] = 0x4fef;			/* lea 12(sp),sp */
	q[1] = 12;
	q[2] = 0x4a80;			/* tst.l d0 */
	q[3] = 0x6606;			/* bne.s rts */
	q[4] = 0x4ef9;			/* jmp a */
	q[5] = a >> 16;
	q[6] = (unsigned short)a;
	q[7] = 0x4e75;			/* rts */
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

/*
 * The idle thread spins on the run queues.  Its loop head, which clears the
 * AST bits at spl7, gets a stop #$2000 in front, paid for by the loop's two
 * dead saves of sr:
 *
 *	L: movea dN,a2; move.l (a2,d2*4),d0; moveq #-8,d1; and.l d1,d0; beq.s X
 *	   move sr,d0; move #$2300,sr; move.l ..,d0; and.l; move.l d0,..
 *	   move sr,d0; move #$2000,sr
 *	X: move.l (a5),d0 ...			(the queues; empty: bra L)
 * becomes
 *	L: stop #$2000; movea ...; beq.s X; move #$2300,sr; ...; move #$2000,sr
 * An interrupt between the queue tests and the stop waits for the next tick.
 */
static int
idleloop()
{
	unsigned short at[3], *s, *l, w[17];
	unsigned long a = bsyms[B_IDLE].addr;

	at[0] = 0x40c0;
	at[1] = 0x46fc;
	at[2] = 0x2300;
	if (!inkernel(a) || !(s = find((unsigned short *)a, 64, at, 3)))
		return -1;
	l = s - 6;
	if ((l[0] & 0xfff8) != 0x2440 || l[3] != 0x72f8 || l[4] != 0xc081 || l[5] != 0x6716 ||
	    (l[1] & 0xffc0) != 0x2000 || (s[3] & 0xffc0) != 0x2000 || s[5] != 0xc081 ||
	    s[8] != 0x40c0 || s[9] != 0x46fc || s[10] != 0x2000 || (s[11] & 0xffc0) != 0x2000)
		return -1;
	w[0] = 0x4e72;				/* stop #$2000 */
	w[1] = 0x2000;
	memcpy(w + 2, l, 12);
	w[7] = 0x6712;				/* beq.s X, 4 bytes nearer */
	w[8] = 0x46fc;
	w[9] = 0x2300;
	memcpy(w + 10, s + 3, 10);
	w[15] = 0x46fc;
	w[16] = 0x2000;
	memcpy(l, w, sizeof w);
	return 0;
}

static int
services()
{
	unsigned short *c = (unsigned short *)HF_HCALL;
	char *bad[20 + sizeof absent / sizeof absent[0] + sizeof nodsp / sizeof nodsp[0]];
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
	if (idleloop() < 0)
		bad[n++] = "idle_thread_continue";
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
	if (copyentry(bsyms[B_COPYIN].addr, HC_COPYIN, HF_TRAMP) < 0)
		bad[n++] = "copyin";
	if (copyentry(bsyms[B_COPYOUT].addr, HC_COPYOUT, HF_TRAMP + 16) < 0)
		bad[n++] = "copyout";
	if (copyentry(bsyms[B_COPYSTR].addr, HC_COPYSTR, HF_TRAMP + 32) < 0)
		bad[n++] = "copyinstr";
	if (copyentry(bsyms[B_COPYOSTR].addr, HC_COPYOSTR, HF_TRAMP + 48) < 0)
		bad[n++] = "copyoutstr";
	for (i = B_COPYIN; i <= B_COPYOSTR; i++)
		again(bsyms[i].addr, HF_TRAMP + 64 + 16 * (i - B_COPYIN));
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
	/* no DSP or DMA: these do nothing */
	for (i = 0; i < sizeof nodsp / sizeof nodsp[0]; i++)
		if (hcentry(nodsp[i].addr, 0) < 0)
			bad[n++] = nodsp[i].name;
	for (i = 0; i < n; i++)
		fprintf(stderr, "next: not a host call: %s\n", bad[i]);
	if (!n)
		fprintf(stderr, "next: the board's routines are host calls\n");
	return n ? -1 : 0;
}

/* words of p's prologue that cover 6 bytes: link, pea (a6), movea sp,a6, movem.l and move.l to -(sp); 0 for any other */
static int
prologue(p)
	unsigned short *p;
{
	int n = 0;

	while (n < 3)
		if (p[n] == 0x4e56 || p[n] == 0x48e7)
			n += 2;
		else if (p[n] == 0x4856 || p[n] == 0x2c4f || (p[n] & 0xfff0) == 0x2f00)
			n++;
		else
			return 0;
	return n;
}

static unsigned short ptramp[P_N - P_VALID][10];
static unsigned short pret[5];

/*
 * The pmap tells the MMU each change through host calls at the entry and
 * return of the routines that make them, so its whole-ATC flushes (on
 * every change and task switch) and ATC loads go.  All or nothing.
 */
static int
pmap()
{
	unsigned short *p, *t, *crp, w[2];
	unsigned long a;
	int i, k, n[P_N - P_VALID];

	for (i = 0; i < P_N; i++)
		if (!inkernel(psyms[i].addr) || (i < P_CRP && !framed(psyms[i].addr)))
			return -1;
	w[0] = 0xf4f8;			/* cpusha bc */
	w[1] = 0xf510;			/* pflushan */
	if (!(crp = find((unsigned short *)psyms[P_CRP].addr, 12, w, 2)) || crp[4] != 0x4e7b || crp[5] != 0x0806)
		return -1;
	for (i = P_VALID; i < P_N; i++)
		if (!framed(psyms[i].addr) || !(n[i - P_VALID] = prologue((unsigned short *)psyms[i].addr)))
			return -1;
	for (i = 0; i < P_CRP; i++)
		*(unsigned short *)psyms[i].addr = 0x4e75;	/* rts */
	crp[1] = 0x4e71;		/* nop */
	for (i = P_VALID; i < P_N; i++) {
		p = (unsigned short *)psyms[i].addr;
		t = ptramp[i - P_VALID];
		a = HC_STUB(1 + i - P_VALID);
		t[0] = 0x4eb9;
		t[1] = a >> 16;
		t[2] = (unsigned short)a;
		for (k = 0; k < n[i - P_VALID]; k++)
			t[3 + k] = p[k];
		a = (unsigned long)(p + k);
		t[3 + k] = 0x4ef9;
		t[4 + k] = a >> 16;
		t[5 + k] = (unsigned short)a;
		a = (unsigned long)t;
		p[0] = 0x4ef9;
		p[1] = a >> 16;
		p[2] = (unsigned short)a;
	}
	a = HC_STUB(1 + P_RET - P_VALID);
	pret[0] = 0x598f;		/* subq.l #4,sp: the return address's slot */
	pret[1] = 0x4eb9;
	pret[2] = a >> 16;
	pret[3] = (unsigned short)a;
	pret[4] = 0x4e75;
	return 0;
}

/*
 * A patched routine in progress: the drops it needs wait for its return,
 * when the entries have changed.  Its arguments' address names it.
 */
#define PF_N 32
#define PF_Q 8
static struct pframe {
	unsigned long args, ret;	/* args 0: free */
	int n, ne;	/* queued drops; the first ne the routine's own, sent again at return */
	struct nx_mmu q[PF_Q];
} pf[PF_N];
static int pfull;	/* no frame was free: drops at entry from then on */

static void
pdrops(f)
	struct pframe *f;
{
	int k;

	for (k = 0; k < f->n; k++)
		(void)ioctl(bridge, NX_MMU, &f->q[k]);
	f->n = 0;
}

/* a pmap routine's entry, or with P_RET its return: r[18] on are its arguments */
static void
pmapcall(i, r)
	int i;
	unsigned long *r;
{
	unsigned long *a = r + 18, args = (unsigned long)a, ps = L(psyms[P_PGSIZE].addr);
	struct pframe *f, *in = 0;
	struct nx_mmu q[2];
	int n = 0, k;

	for (f = pf; f < pf + PF_N; f++)
		if (f->args == args)
			break;
	if (i == P_RET) {
		if (f == pf + PF_N) {
			fprintf(stderr, "next: pmap return without its entry\n");
			_exit(1);
		}
		pdrops(f);
		r[17] = f->ret;
		f->args = 0;
		return;
	}
	q[0].op = q[1].op = 2;
	q[0].pa = q[1].pa = 0;
	if (i == P_MOVE) {
		q[0].key = q[1].key = 0;
		q[0].start = a[0];
		q[0].end = a[0] + a[2];
		q[1].start = a[1];
		q[1].end = a[1] + a[2];
		n = 2;
	} else if (i < P_RMALL && a[0]) {
		q[0].key = a[0] == L(psyms[P_KPMAP].addr) ? 0 : L(a[0] + 4);
		n = 1;
		switch (i) {
		case P_ENTER:
			/* (pmap, va, pa, ...): the new page's code reaches the instruction cache */
			q[0].pa = a[2] & ~(ps - 1);
		case P_VALID:
			q[0].start = a[1] & ~(ps - 1);
			q[0].end = q[0].start + ps;
			break;
		case P_PROTECT:
			/* code written before it became executable */
			if (a[3] & 4)
				q[0].op = 4;
		case P_REMOVE:
			q[0].start = a[1];
			q[0].end = a[2];
			break;
		case P_DESTROY:
			if (!q[0].key)
				n = 0;
			q[0].op = 3;
			break;
		}
	}
	if (i == P_VALID && !pfull) {
		/* a lookup, unless a routine that changes entries is in progress below it */
		for (f = pf; f < pf + PF_N; f++)
			if (f->args > args && (!in || f->args < in->args))
				in = f;
		if (!in || !n)
			return;
		if (in->n == PF_Q) {
			pdrops(in);	/* changed by now */
			in->n = in->ne;
		}
		in->q[in->n++] = q[0];
		return;
	}
	if (f == pf + PF_N)
		for (f = pf; f < pf + PF_N && f->args; f++)
			;
	if (f == pf + PF_N || pfull) {
		pfull = 1;
		for (k = 0; k < n; k++)
			(void)ioctl(bridge, NX_MMU, &q[k]);
		return;
	}
	pdrops(f);
	f->args = args;
	f->ret = r[17];
	for (k = 0; k < n; k++)
		f->q[k] = q[k];
	f->n = f->ne = n;
	r[17] = (unsigned long)pret;
}

/*
 * The kernel calls a loaded server's procedures through kern_serv_call_proc
 * (no procedure: 100; else proc(arg), 0).  The guest code's copy of it also
 * finds the audio server and takes its output.
 */
static int
audio()
{
	struct hf_snd *sd = (struct hf_snd *)HF_SND;
	unsigned short *p = (unsigned short *)ssyms[5].addr;
	static unsigned short code[] = { 0x4856, 0x2c4f, 0x206e, 0x000c, 0x4a88, 0x6604, 0x7064 };
	int i;

	for (i = 0; i < sizeof ssyms / sizeof ssyms[0]; i++)
		if (!inkernel(ssyms[i].addr))
			return -1;
	if (!timeout || !sd->callproc || memcmp(p, code, sizeof code))
		return -1;
	sd->timeout = (long (*)())timeout;
	sd->softint = (long (*)())ssyms[0].addr;
	sd->lookup = (long (*)())ssyms[1].addr;
	sd->kmap = ssyms[2].addr;
	sd->dmastart = ssyms[3].addr;
	sd->dmaabort = ssyms[4].addr;
	p[0] = 0x4ef9;				/* jmp callproc */
	p[1] = sd->callproc >> 16;
	p[2] = (unsigned short)sd->callproc;
	return 0;
}

static int hfslog;	/* each request on stderr */

/* the guest's request in the mailbox */
static void
hfcall(sig)
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
	if (hfslog) {
		sprintf(line, "next: hfs op %lu node %lu error %ld result %ld %.40s\n", b->op, b->node, b->error, b->result,
		    b->op == HF_LOOKUP || b->op == HF_CREATE || b->op == HF_REMOVE || b->op == HF_MKDIR || b->op == HF_RENAME ?
		    b->name : "");
		write(2, line, strlen(line));
	}
}

HC_ENTRY

/* the kernel's host filesystem and pmap call this directly: no trap or signal */
void
hostcall(r)
	unsigned long *r;
{
	int n = HC_CALL(r);

	if (n)
		pmapcall(P_VALID + n - 1, r);
	else
		hfcall(0);
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
	direct = kernel && !getenv("NEXT_2BIT") && nh_display_open(&display, "/dev/fb0", "NeXT", 1) == 0;
	if (screen() < 0) {
		perror("next: the host screen");
		return 1;
	}
	if (!direct && display.framebuffer >= 0)
		nh_display_close(&display);
	if (conflicts() < 0 || memory(root, kernel ? 0 : media) < 0) {
		perror("next: memory or ROM (128 KB)");
		return 1;
	}
	if (kernel && (entry = loadkernel(kernel)) == 0) {
		fprintf(stderr, "next: %s: no m68k kernel\n", kernel);
		return 1;
	}
	memset(&ne, 0, sizeof ne);
	sprintf(path, "%s.nvram", root);
	if (stat(path, &st) == 0 && st.st_size == NX_NVRAM && (fd = open(path, O_RDONLY)) >= 0) {
		if (read(fd, ne.nvram, NX_NVRAM) != NX_NVRAM)
			defaults(ne.nvram);
		close(fd);
	} else
		defaults(ne.nvram);
	ne.ramsize = ramsize;
	ne.width = width;
	ne.height = height;
	if (getenv("NEXT_CENSUS"))
		fprintf(stderr, "next: RAM %lu MB\n", ramsize >> 20);
	fprintf(stderr, direct ? "next: screen %lux%lu, %lu bytes a row, 8-bit gray direct\n" :
	    "next: screen %lux%lu, %lu bytes a row, 2-bit\n", width, height, rowbytes);
	hfslog = getenv("NEXT_CENSUS") != 0;
	ne.flags = (hfslog ? NXF_CENSUS : 0) | NXF_HOST;
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
			kmfit();
			if (direct)
				fbentry();
			else
				cursorrow();
			if (audio() < 0)
				fprintf(stderr, "next: no sound out\n");
			sa.sa_handler = hfcall;
			sa.sa_flags = SA_ONSTACK;
			sigaction(SIGPWR, &sa, (struct sigaction *)0);
		}
		services();
		if (pmap() == 0)
			ne.flags |= NXF_PMAP;
		else
			fprintf(stderr, "next: the pmap keeps its own flushes\n");
	}
	/* the child shares the host filesystem's region for the keys */
	machine = getpid();
	if ((child = fork()) < 0) {
		perror("next: fork");
		return 1;
	}
	if (child == 0)
		_exit(host(root));
	if (getenv("NEXT_PC")) {
		struct itimerval it;

		sa.sa_handler = (void (*)())tick;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigaction(SIGALRM, &sa, (struct sigaction *)0);
		it.it_interval.tv_sec = it.it_value.tv_sec = 0;
		it.it_interval.tv_usec = it.it_value.tv_usec = 20000;
		setitimer(ITIMER_REAL, &it, (struct itimerval *)0);
	}
	/* code written before the enter, whose cache push takes it to the instruction cache */
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
	}
	if (ioctl(bridge, NX_ENTER, &ne) < 0) {
		perror("next: enter");
		kill(child, SIGTERM);
		return 1;
	}
	if (entry)
		enter(bootframe(kernel), entry);
	else
		reset();
	return 1;
}
