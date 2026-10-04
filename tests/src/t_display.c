/*
 * t_display.c -- display service: /dev/fb0 ioctls and mapping, the CLUT,
 * sessions and switching, /dev/kbd and /dev/mouse.
 *
 * Host requests (keys, mouse, screen dumps and their comparison) go out
 * as "@@ SEQ CMD ARGS" lines on /dev/term/b; run-qemu.sh's hostio.py
 * answers "@@ok SEQ RESULT".  Without an answer to the first request
 * the host-dependent checks are skipped.  Output to the console is
 * held back while a check compares console screens.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/termio.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"
#include "dspat.h"

#define DISPGID		25
#define NOBODY		60001

static int hfd = -1;		/* host line, -1 none */
static int hseq;
static char hrep[128];
static struct fbinfo fi;

/* send a host request; the answer text in hrep, 0 if none in 20 s */
static char *
host(char *req)
{
	char buf[160], line[256];
	struct pollfd p;
	int n, len = 0;
	long t0;

	if (hfd < 0)
		return 0;
	sprintf(buf, "@@ %d %s\n", ++hseq, req);
	write(hfd, buf, strlen(buf));
	t0 = t_now_ms();
	p.fd = hfd;
	p.events = POLLIN;
	while (t_now_ms() - t0 < 20000) {
		if (poll(&p, 1, 500) <= 0)
			continue;
		n = read(hfd, line + len, sizeof line - 1 - len);
		if (n <= 0)
			continue;
		len += n;
		line[len] = 0;
		for (;;) {
			char *nl = strchr(line, '\n'), *s;
			int seq;

			if (nl == 0)
				break;
			*nl = 0;
			s = strstr(line, "@@ok ");
			if (s && sscanf(s + 5, "%d", &seq) == 1 && seq == hseq) {
				s = strchr(s + 5, ' ');
				strncpy(hrep, s ? s + 1 : "", sizeof hrep - 1);
				hrep[strcspn(hrep, "\r")] = 0;
				return hrep;
			}
			if ((s = strstr(line, "@@err ")) != 0 && sscanf(s + 6, "%d", &seq) == 1 &&
			    seq == hseq) {
				strcpy(hrep, "error");
				return hrep;
			}
			len -= nl + 1 - line;
			memmove(line, nl + 1, len + 1);
		}
		if (len > 200)
			len = 0;
	}
	return 0;
}

/* compare screen dump shot with the pattern of seed, table rotated k */
static char *
hostref(char *names, int seed, int k)
{
	char buf[96];

	sprintf(buf, "ref %s %lu %d %d", names, fi.fi_depth, seed, k);
	return host(buf);
}

static void
hostopen()
{
	struct termio t;

	hfd = open("/dev/term/b", O_RDWR | O_NOCTTY);
	if (hfd < 0) {
		t_info("host", "no /dev/term/b: %s", T_ERR);
		return;
	}
	if (ioctl(hfd, TCGETA, &t) == 0) {
		t.c_iflag = IGNCR;
		t.c_oflag = 0;
		t.c_lflag = ICANON;
		t.c_cflag |= CREAD | CLOCAL;
		ioctl(hfd, TCSETAF, &t);
	}
	if (host("ping") == 0 || strcmp(hrep, "pong") != 0) {
		t_info("host", "no answer on /dev/term/b");
		close(hfd);
		hfd = -1;
	}
}

static int
fbopen()
{
	return open("/dev/fb0", O_RDWR);
}

static long
acquire(int fd, int front, char *name)
{
	struct fbacq a;

	memset(&a, 0, sizeof a);
	a.fa_kind = FBK_USER;
	a.fa_flags = front ? FBA_FRONT : 0;
	strncpy(a.fa_name, name, sizeof a.fa_name - 1);
	if (ioctl(fd, FBIOACQUIRE, &a) < 0)
		return -1;
	return (long)a.fa_id;
}

static long
front()
{
	struct fbstate st;
	int fd = fbopen();

	if (fd < 0)
		return -2;
	if (ioctl(fd, FBIOGSTATE, &st) < 0)
		st.st_front = -2;
	close(fd);
	return st.st_front;
}

/* wait up to secs for session id in front */
static int
waitfront(long id, int secs)
{
	long t0 = t_now_ms();

	while (t_now_ms() - t0 < secs * 1000L) {
		if (front() == id)
			return 1;
		poll((struct pollfd *)0, 0, 100);
	}
	return 0;
}

static int
putcmap(int fd, unsigned long seed, unsigned long k)
{
	static unsigned short r[256], g[256], b[256];
	struct fbcmap cm;

	if (fi.fi_cmapsize == 0)
		return 0;
	dspat_cmap(seed, k, fi.fi_cmapsize, r, g, b);
	cm.cm_start = 0;
	cm.cm_count = fi.fi_cmapsize;
	cm.cm_red = r;
	cm.cm_green = g;
	cm.cm_blue = b;
	return ioctl(fd, FBIOPUTCMAP, &cm);
}

/* next event within ms, 0 if none */
static int
nextev(int fd, struct inev *v, int ms)
{
	struct pollfd p;

	p.fd = fd;
	p.events = POLLIN;
	if (poll(&p, 1, ms) <= 0)
		return 0;
	return read(fd, v, sizeof *v) == sizeof *v;
}

/* drain fd; number of events read */
static int
drain(int fd, int ms)
{
	struct inev v;
	int n = 0;

	while (nextev(fd, &v, ms))
		n++;
	return n;
}

static int
evtime_le(struct inev *a, struct inev *b)
{
	return a->ie_sec < b->ie_sec || (a->ie_sec == b->ie_sec && a->ie_usec <= b->ie_usec);
}

/* a key tap: down then up of code, in time order; why in *why */
static int
keytap(int fd, int code, struct inev *last, char *why)
{
	struct inev d, u;

	if (!nextev(fd, &d, 3000) || !nextev(fd, &u, 3000)) {
		strcpy(why, "no events");
		return 0;
	}
	if (d.ie_type != IE_KEY || d.ie_code != code || d.ie_value != 1 ||
	    u.ie_type != IE_KEY || u.ie_code != code || u.ie_value != 0) {
		sprintf(why, "got type %d code 0x%x value %ld, type %d code 0x%x value %ld",
		    d.ie_type, d.ie_code, d.ie_value, u.ie_type, u.ie_code, u.ie_value);
		return 0;
	}
	if (!evtime_le(last, &d) || !evtime_le(&d, &u) ||
	    (d.ie_sec == u.ie_sec && d.ie_usec == u.ie_usec)) {
		sprintf(why, "times %ld.%06ld %ld.%06ld after %ld.%06ld", d.ie_sec, d.ie_usec,
		    u.ie_sec, u.ie_usec, last->ie_sec, last->ie_usec);
		return 0;
	}
	*last = u;
	return 1;
}

static void
t_ioctls(int fd)
{
	struct fbmodeinfo mi[16];
	struct fbmodes ms;
	struct fbcmap cm;
	unsigned long v0, v1, i, cur;
	long t0, dt;
	void *p;
	int k;

	k = ioctl(fd, FBIOGINFO, &fi);
	t_check("ginfo", k == 0 && fi.fi_width >= 160 && fi.fi_height >= 80 &&
	    fi.fi_rowbytes >= fi.fi_width * fi.fi_depth / 8 && fi.fi_offset < 4096 &&
	    fi.fi_size % 4096 == 0 &&
	    fi.fi_size >= fi.fi_offset + fi.fi_rowbytes * fi.fi_height &&
	    fi.fi_cmapsize == (fi.fi_depth <= 8 ? 1UL << fi.fi_depth : 0),
	    "ioctl %d, %lux%lu depth %lu row %lu offset %lu size %lu cmap %lu", k,
	    fi.fi_width, fi.fi_height, fi.fi_depth, fi.fi_rowbytes, fi.fi_offset,
	    fi.fi_size, fi.fi_cmapsize);
	t_info("mode", "%.16s %lux%lu depth %lu row %lu offset 0x%lx size 0x%lx mode 0x%lx flags 0x%lx",
	    fi.fi_name, fi.fi_width, fi.fi_height, fi.fi_depth, fi.fi_rowbytes, fi.fi_offset,
	    fi.fi_size, fi.fi_mode, fi.fi_flags);

	ms.ms_count = 0;
	ms.ms_modes = 0;
	k = ioctl(fd, FBIOGMODES, &ms);
	t_check("gmodes_count", k == 0 && ms.ms_count >= 1 && ms.ms_count <= 16,
	    "ioctl %d count %lu", k, ms.ms_count);
	ms.ms_count = 16;
	ms.ms_modes = mi;
	k = ioctl(fd, FBIOGMODES, &ms);
	for (i = 0, cur = 0; k == 0 && i < ms.ms_count && i < 16; i++) {
		t_info("modes", "0x%lx %lux%lu depth %lu row %lu offset 0x%lx flags %lu", mi[i].mi_id,
		    mi[i].mi_width, mi[i].mi_height, mi[i].mi_depth, mi[i].mi_rowbytes,
		    mi[i].mi_offset, mi[i].mi_flags);
		if ((mi[i].mi_flags & FBM_CURRENT) && mi[i].mi_id == fi.fi_mode &&
		    mi[i].mi_depth == fi.fi_depth && mi[i].mi_width == fi.fi_width &&
		    mi[i].mi_rowbytes == fi.fi_rowbytes)
			cur++;
	}
	t_check("gmodes_current", cur == 1, "%lu entries match the current mode", cur);
	ms.ms_count = 4;
	ms.ms_modes = (struct fbmodeinfo *)8;
	t_check("gmodes_efault", ioctl(fd, FBIOGMODES, &ms) < 0 && errno == EFAULT,
	    "bad pointer: %s", T_ERR);
	t_check("ginfo_efault", ioctl(fd, FBIOGINFO, (struct fbinfo *)8) < 0 && errno == EFAULT,
	    "bad pointer: %s", T_ERR);

	/* no session yet */
	p = mmap(0, fi.fi_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	t_check("mmap_needs_session", p == (void *)-1, "mapped without a session");
	if (p != (void *)-1)
		munmap(p, fi.fi_size);
	memset(&cm, 0, sizeof cm);
	cm.cm_count = 1;
	t_check("cmap_needs_session", ioctl(fd, FBIOPUTCMAP, &cm) < 0 && errno == EINVAL,
	    "%s", T_ERR);
	t_check("smode_enxio", ioctl(fd, FBIOSMODE, 0x80) < 0 && errno == ENXIO, "%s", T_ERR);
	t_check("vblwait_range", ioctl(fd, FBIOVBLWAIT, 0) < 0 && errno == EINVAL &&
	    ioctl(fd, FBIOVBLWAIT, FB_MAXVBLWAIT + 1) < 0 && errno == EINVAL, "%s", T_ERR);
	t_check("switch_bad_id", ioctl(fd, FBIOSWITCH, 9999) < 0 && errno == EINVAL,
	    "%s", T_ERR);
	t_check("bad_ioctl", ioctl(fd, FBIOC(99), 0) < 0 && errno == EINVAL, "%s", T_ERR);

	ioctl(fd, FBIOGVBL, &v0);
	t0 = t_now_ms();
	k = ioctl(fd, FBIOVBLWAIT, 6);
	dt = t_now_ms() - t0;
	ioctl(fd, FBIOGVBL, &v1);
	t_check("vblwait", k == 0 && v1 - v0 >= 6 && dt >= 40 && dt < 2000,
	    "ioctl %d, count %lu -> %lu in %ld ms", k, v0, v1, dt);
	ioctl(fd, FBIOGINFO, &fi);
	t_info("vbl", "%s VBL, 6 VBLs in %ld ms", (fi.fi_flags & FBF_VBL) ? "DAFB" : "tick", dt);
}

static void
t_session(int fd, long *ida, unsigned char **fbp)
{
	static unsigned short r[256], g[256], b[256], r2[256], g2[256], b2[256];
	struct fbcmap cm;
	struct fbstate st;
	unsigned char *fb;
	void *p;
	long id;
	int k, bad;

	*ida = id = acquire(fd, 1, "t_display");
	t_check("acquire_front", id > 0 && ioctl(fd, FBIOGSTATE, &st) == 0 &&
	    st.st_session == id && st.st_front == id, "id %ld: %s", id, T_ERR);
	t_check("acquire_twice", acquire(fd, 0, "x") < 0 && errno == EBUSY, "%s", T_ERR);

	p = mmap(0, fi.fi_size + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	t_check("mmap_too_big", p == (void *)-1 && errno == ENXIO, "%s", T_ERR);
	p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	t_check("mmap_private", p == (void *)-1 && errno == EINVAL, "%s", T_ERR);
	p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, fi.fi_size);
	t_check("mmap_past_end", p == (void *)-1, "offset fi_size mapped");
	fb = (unsigned char *)mmap(0, fi.fi_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	t_check("mmap", fb != (unsigned char *)-1, "%s", T_ERR);
	*fbp = fb;
	if (fb == (unsigned char *)-1)
		return;

	if (fi.fi_cmapsize) {
		dspat_cmap(1UL, 0UL, fi.fi_cmapsize, r, g, b);
		r[0] = 0x1234;			/* all 16 bits kept */
		cm.cm_start = 0;
		cm.cm_count = fi.fi_cmapsize;
		cm.cm_red = r;
		cm.cm_green = g;
		cm.cm_blue = b;
		k = ioctl(fd, FBIOPUTCMAP, &cm);
		cm.cm_red = r2;
		cm.cm_green = g2;
		cm.cm_blue = b2;
		k |= ioctl(fd, FBIOGETCMAP, &cm);
		t_check("cmap_roundtrip", k == 0 && memcmp(r, r2, fi.fi_cmapsize * 2) == 0 &&
		    memcmp(g, g2, fi.fi_cmapsize * 2) == 0 && memcmp(b, b2, fi.fi_cmapsize * 2) == 0,
		    "ioctl %d, table differs", k);
		cm.cm_start = 1;
		t_check("cmap_range", ioctl(fd, FBIOPUTCMAP, &cm) < 0 && errno == EINVAL &&
		    ioctl(fd, FBIOGETCMAP, &cm) < 0 && errno == EINVAL, "%s", T_ERR);
		cm.cm_start = 0;
		cm.cm_count = 2;
		cm.cm_red = (unsigned short *)8;
		t_check("cmap_efault", ioctl(fd, FBIOPUTCMAP, &cm) < 0 && errno == EFAULT,
		    "%s", T_ERR);
		cm.cm_count = 0;
		t_check("cmap_empty", ioctl(fd, FBIOPUTCMAP, &cm) == 0, "%s", T_ERR);
		t_check("cmap_set", putcmap(fd, 1UL, 0UL) == 0, "%s", T_ERR);
	}
	t_check("cache_bad", ioctl(fd, FBIOCACHE, 7) < 0 && errno == EINVAL, "%s", T_ERR);

	dspat_draw(fb, &fi, 1UL);
	bad = dspat_check(fb, &fi, 1UL);
	t_check("draw_readback", bad == 0, "%d pixels differ", bad);
	ioctl(fd, FBIOVBLWAIT, 2);
	if (host("shot pat_a") == 0)
		t_skip("shot_pattern", "no host");
	else {
		hostref("pat_a_ref pat_a", 1, 0);
		t_check("shot_pattern", strcmp(hrep, "same") == 0, "screen vs pattern: %s", hrep);
	}
	if (fi.fi_cmapsize) {
		for (k = 1, bad = 0; k <= 32; k++)
			bad |= ioctl(fd, FBIOVBLWAIT, 1) | putcmap(fd, 1UL, (unsigned long)k);
		ioctl(fd, FBIOVBLWAIT, 2);
		t_check("clut_cycle", bad == 0, "%s", T_ERR);
		if (host("shot clut32") == 0)
			t_skip("shot_clut_cycle", "no host");
		else {
			hostref("clut32_ref clut32", 1, 32);
			t_check("shot_clut_cycle", strcmp(hrep, "same") == 0,
			    "screen vs pattern, table rotated 32: %s", hrep);
		}
		ioctl(fd, FBIOBLANK, 1);
		ioctl(fd, FBIOVBLWAIT, 2);
		if (host("shot blank")) {
			hostref("blank_ref blank", 1, 32);
			t_check("blank", strncmp(hrep, "diff", 4) == 0, "screen unchanged by FBIOBLANK");
		}
		ioctl(fd, FBIOBLANK, 0);
		ioctl(fd, FBIOVBLWAIT, 2);
		if (host("shot unblank")) {
			hostref("unblank_ref unblank", 1, 32);
			t_check("unblank", strcmp(hrep, "same") == 0, "after unblank: %s", hrep);
		}
	}
}

/*
 * A's child touches VRAM, A loses the front, the child writes: the writes
 * reach A's shadow, B's screen is untouched.  Partial munmap is refused.
 */
static void
t_fork(int fda, long ida, unsigned char *fba, long idb, unsigned char *fbb)
{
	int go[2], rdy[2], status, k, bad, bada;
	pid_t pid;
	char c = 0;

	if (pipe(go) < 0 || pipe(rdy) < 0) {
		t_fail("fork_follows_switch", "pipe: %s", T_ERR);
		return;
	}
	pid = fork();
	if (pid == 0) {
		*(volatile unsigned char *)fba = *(volatile unsigned char *)fba;
		write(rdy[1], &c, 1);
		if (read(go[0], &c, 1) != 1)
			_exit(1);
		dspat_draw(fba, &fi, 3UL);
		_exit(0);
	}
	k = read(rdy[0], &c, 1) == 1 && ioctl(fda, FBIOSWITCH, idb) == 0 && front() == idb;
	write(go[1], &c, 1);
	status = -1;
	t_waitchild(pid, &status, 20);
	bad = dspat_check(fbb, &fi, 2UL);
	bada = dspat_check(fba, &fi, 3UL);
	t_check("fork_follows_switch", k && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
	    bad == 0 && bada == 0, "switch %d, status 0x%x, B %d, A %d pixels differ",
	    k, status, bad, bada);
	k = ioctl(fda, FBIOSWITCH, ida) == 0 && front() == ida;
	bada = dspat_check(fba, &fi, 3UL);
	t_check("fork_shadow_back", k && bada == 0, "switch %d, A %d pixels differ", k, bada);
	dspat_draw(fba, &fi, 1UL);
	close(go[0]);
	close(go[1]);
	close(rdy[0]);
	close(rdy[1]);
	if (fi.fi_size > 8192)
		t_check("munmap_partial", munmap((void *)(fbb + 4096), 4096) < 0 &&
		    errno == EINVAL, "%s", T_ERR);
}

static void
t_input(int fda, long ida, unsigned char *fba)
{
	struct evinfo ei;
	struct inev v, last;
	char why[160];
	unsigned char keys[16];
	long dx, dy, idb;
	int kbd, ms, kbdb, fdb, k, n, bad, hid, up36, up37, up3a;
	unsigned char *fbb;
	struct fbnote note;

	kbd = open("/dev/kbd", O_RDONLY);
	ms = open("/dev/mouse", O_RDONLY);
	t_check("open_input", kbd >= 0 && ms >= 0, "%s", T_ERR);
	if (kbd < 0 || ms < 0)
		return;
	k = ioctl(kbd, EVIOCGINFO, &ei);
	t_check("evinfo", k == 0 && ei.ei_kset == EVK_ADB && (ei.ei_flags & EVF_CAPSLATCH),
	    "ioctl %d kset %lu flags 0x%lx", k, ei.ei_kset, ei.ei_flags);
	t_info("keyboard", "handler %lu", ei.ei_id);
	t_check("bind_bad_fd", ioctl(kbd, EVIOCBIND, kbd) < 0 && errno == EINVAL, "%s", T_ERR);
	t_check("bind", ioctl(kbd, EVIOCBIND, fda) == 0 && ioctl(ms, EVIOCBIND, fda) == 0,
	    "%s", T_ERR);
	t_check("read_nonblock", fcntl(kbd, F_SETFL, O_NONBLOCK) == 0 &&
	    read(kbd, &v, sizeof v) < 0 && errno == EAGAIN, "%s", T_ERR);
	fcntl(kbd, F_SETFL, 0);
	t_check("read_short", read(kbd, &v, 8) < 0 && errno == EINVAL, "%s", T_ERR);
	if (hfd < 0) {
		t_skip("keys", "no host");
		return;
	}

	memset(&last, 0, sizeof last);
	host("key a");
	k = keytap(kbd, 0x00, &last, why);
	if (k) {
		host("key s");
		k = keytap(kbd, 0x01, &last, why);
	}
	if (k) {
		host("key ret");
		k = keytap(kbd, 0x24, &last, why);
	}
	t_check("keys", k, "%s", why);
	t_info("key_time", "%ld.%06ld", last.ie_sec, last.ie_usec);
	t_check("mouse_quiet_kbd", drain(kbd, 300) == 0, "extra key events");

	host("move 10 5");
	dx = dy = 0;
	bad = n = 0;
	while (nextev(ms, &v, 1500)) {
		n++;
		if (v.ie_type == IE_REL && v.ie_code == IE_RELX)
			dx += v.ie_value;
		else if (v.ie_type == IE_REL && v.ie_code == IE_RELY)
			dy += v.ie_value;
		else if (v.ie_type != IE_SYN)
			bad++;
		if (!evtime_le(&last, &v))
			bad++;
		last = v;
	}
	t_check("mouse_move", n > 0 && bad == 0 && dx == 10 && dy == 5,
	    "%d events, %d odd, dx %ld dy %ld", n, bad, dx, dy);
	host("button 1");
	k = nextev(ms, &v, 3000) && v.ie_type == IE_BTN && v.ie_code == 1 && v.ie_value == 1;
	drain(ms, 300);
	host("button 0");
	k = k && nextev(ms, &v, 3000) && v.ie_type == IE_BTN && v.ie_code == 1 && v.ie_value == 0;
	drain(ms, 300);
	t_check("mouse_button", k, "type %d code %d value %ld", v.ie_type, v.ie_code, v.ie_value);

	/* a second session in the background gets nothing */
	fdb = fbopen();
	idb = acquire(fdb, 0, "t_display_b");
	kbdb = open("/dev/kbd", O_RDONLY | O_NONBLOCK);
	t_check("acquire_background", idb > ida && kbdb >= 0 &&
	    ioctl(kbdb, EVIOCBIND, fdb) == 0 && front() == ida, "id %ld: %s", idb, T_ERR);
	host("key d");
	k = keytap(kbd, 0x02, &last, why);
	t_check("background_no_input", k && read(kbdb, &v, sizeof v) < 0 && errno == EAGAIN,
	    "front: %s; background read gave data", k ? "ok" : why);

	/* B draws while hidden, into its shadow */
	fbb = (unsigned char *)mmap(0, fi.fi_size, PROT_READ | PROT_WRITE, MAP_SHARED, fdb, 0);
	if (fbb == (unsigned char *)-1) {
		t_fail("mmap_hidden", "%s", T_ERR);
		return;
	}
	putcmap(fdb, 2UL, 0UL);
	dspat_draw(fbb, &fi, 2UL);
	bad = dspat_check(fbb, &fi, 2UL);
	t_check("hidden_draw", bad == 0 && dspat_check(fba, &fi, 1UL) == 0,
	    "%d pixels differ", bad);
	if (host("shot hidden_b")) {
		hostref("hidden_b_ref hidden_b", 1, 32);
		t_check("hidden_not_shown", strcmp(hrep, "same") == 0, "front screen changed: %s", hrep);
	}

	/* hotkey Control-Option-Command-2: B, the second session */
	host("key ctrl+alt+meta_l+2");
	t_check("hotkey_switch", waitfront(idb, 5), "front %ld", front());
	ioctl(fda, FBIOVBLWAIT, 2);
	if (host("shot hot_b")) {
		hostref("hot_b_ref hot_b", 2, 0);
		t_check("shot_switch_b", strcmp(hrep, "same") == 0, "B on screen: %s", hrep);
	}
	/* A's notes: FBN_SHOWN from its FBA_FRONT, then FBN_HIDDEN */
	fcntl(fda, F_SETFL, O_NONBLOCK);
	hid = 0;
	while (read(fda, &note, sizeof note) == sizeof note)
		hid = note.fn_type == FBN_HIDDEN;
	fcntl(fda, F_SETFL, 0);
	t_check("note_hidden", hid, "A's last note is not FBN_HIDDEN");
	up36 = up37 = up3a = bad = 0;
	while (nextev(kbd, &v, 500))
		if (v.ie_type == IE_KEY && v.ie_value == 0 && v.ie_code == 0x36)
			up36++;
		else if (v.ie_type == IE_KEY && v.ie_value == 0 && v.ie_code == 0x37)
			up37++;
		else if (v.ie_type == IE_KEY && v.ie_value == 0 && v.ie_code == 0x3A)
			up3a++;
		else if (v.ie_type == IE_KEY && v.ie_value == 1 &&
		    (v.ie_code == 0x36 || v.ie_code == 0x37 || v.ie_code == 0x3A))
			;
		else
			bad++;
	t_check("switch_release", up36 == 1 && up37 == 1 && up3a == 1 && bad == 0,
	    "A: ups ctl %d cmd %d opt %d, other %d", up36, up37, up3a, bad);
	t_check("switch_no_stray_up", read(kbdb, &v, sizeof v) < 0 && errno == EAGAIN,
	    "B got type %d code 0x%x value %ld", v.ie_type, v.ie_code, v.ie_value);
	bad = dspat_check(fba, &fi, 1UL);
	k = dspat_check(fbb, &fi, 2UL);
	t_check("contents_after_switch", bad == 0 && k == 0, "A %d, B %d pixels differ", bad, k);
	memset(keys, 0xFF, sizeof keys);
	t_check("keys_bitmap", ioctl(kbd, EVIOCGKEYS, keys) == 0 && keys[0] == 0 &&
	    keys[6] == 0 && keys[7] == 0, "%s", T_ERR);

	host("key ctrl+alt+meta_l+1");
	t_check("hotkey_back", waitfront(ida, 5), "front %ld", front());
	ioctl(fda, FBIOVBLWAIT, 2);
	if (host("shot hot_a")) {
		hostref("hot_a_ref hot_a", 1, 32);
		t_check("shot_switch_a", strcmp(hrep, "same") == 0, "A back: %s", hrep);
	}
	drain(kbd, 300);
	drain(kbdb, 300);

	/* a key held across a switch: A gets its up, B nothing */
	host("down a");
	k = nextev(kbd, &v, 3000) && v.ie_code == 0 && v.ie_value == 1;
	k = k && ioctl(fda, FBIOSWITCH, idb) == 0;
	k = k && nextev(kbd, &v, 3000) && v.ie_code == 0 && v.ie_value == 0;
	host("up a");
	t_check("held_key_release", k && drain(kbdb, 800) == 0 && drain(kbd, 300) == 0,
	    "held key: A down/up %d", k);

	/* emergency key: console */
	host("key ctrl+alt+meta_l+esc");
	t_check("emergency_console", waitfront(0L, 5), "front %ld", front());
	drain(kbd, 300);
	drain(kbdb, 300);
	t_check("switch_back_root", ioctl(fda, FBIOSWITCH, ida) == 0 && front() == ida,
	    "%s", T_ERR);

	t_fork(fda, ida, fba, idb, fbb);

	/* closing a hidden session leaves the front alone */
	munmap(fbb, fi.fi_size);
	close(kbdb);
	close(fdb);
	t_check("close_hidden", front() == ida, "front %ld", front());
	close(kbd);
	close(ms);
}

/* as uid nobody: without group display no access; with it, front only from the console */
static void
t_perm(long ida)
{
	struct stat st;
	pid_t pid;
	int status, fd;
	long id;

	t_check("node_mode", stat("/dev/fb0", &st) == 0 && (st.st_mode & 0777) == 0660 &&
	    st.st_uid == 0 && st.st_gid == DISPGID && stat("/dev/kbd", &st) == 0 &&
	    (st.st_mode & 0777) == 0660 && st.st_gid == DISPGID,
	    "mode 0%o uid %ld gid %ld", (int)st.st_mode & 0777, (long)st.st_uid, (long)st.st_gid);
	pid = fork();
	if (pid == 0) {
		setgid(NOBODY);
		setuid(NOBODY);
		_exit(open("/dev/fb0", O_RDWR) < 0 && errno == EACCES ? 0 : 1);
	}
	t_check("perm_other", t_waitchild(pid, &status, 10) == pid && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0, "status 0x%x", status);
	/* group display, not at the console: no front, no switch */
	pid = fork();
	if (pid == 0) {
		setsid();
		setgid(DISPGID);
		setuid(NOBODY);
		fd = open("/dev/fb0", O_RDWR);
		if (fd < 0)
			_exit(2);
		if (ioctl(fd, FBIOSWITCH, 0) == 0 || errno != EPERM)
			_exit(3);
		if (acquire(fd, 1, "nobody") >= 0 || errno != EPERM)
			_exit(4);
		if ((id = acquire(fd, 0, "nobody")) <= 0)
			_exit(5);
		if (ioctl(fd, FBIOSWITCH, id) == 0 || errno != EPERM)
			_exit(6);
		_exit(0);
	}
	t_check("perm_display_group", t_waitchild(pid, &status, 10) == pid &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exit %d",
	    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	/* group display on the console: front and switch, then back to A */
	pid = fork();
	if (pid == 0) {
		setgid(DISPGID);
		setuid(NOBODY);
		if ((fd = open("/dev/tty", O_RDWR)) < 0)
			_exit(2);
		close(fd);
		fd = open("/dev/fb0", O_RDWR);
		if (fd < 0)
			_exit(3);
		if ((id = acquire(fd, 1, "console")) <= 0 || front() != id)
			_exit(4);
		if (ioctl(fd, FBIOSWITCH, 0) != 0 || front() != 0)
			_exit(5);
		if (ioctl(fd, FBIOSWITCH, ida) != 0 || front() != ida)
			_exit(6);
		_exit(0);
	}
	t_check("perm_console_user", t_waitchild(pid, &status, 10) == pid &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exit %d",
	    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	/* root without a controlling terminal */
	pid = fork();
	if (pid == 0) {
		setsid();
		fd = open("/dev/fb0", O_RDWR);
		if (fd < 0)
			_exit(2);
		if ((id = acquire(fd, 1, "root")) <= 0 || front() != id)
			_exit(3);
		if (ioctl(fd, FBIOSWITCH, ida) != 0 || front() != ida)
			_exit(4);
		_exit(0);
	}
	t_check("perm_root_noctty", t_waitchild(pid, &status, 10) == pid &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exit %d",
	    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	t_check("perm_front_kept", front() == ida, "front %ld", front());
}

/* child status for a check: 0 passes */
static void
t_child(char *name, pid_t pid)
{
	int status = 0, ok;

	ok = t_waitchild(pid, &status, 20) == pid;
	t_check(name, ok && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exit %d",
	    ok && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

/* input reaches only the owner's front session; another user's session is closed */
static void
t_inputsec(int fda, long ida)
{
	struct fbcmap cm;
	unsigned short r[2], g[2], b[2];
	struct inev v;
	int go[2], rdy[2], kbd, fd, kb, ub, err;
	pid_t pid;
	long id;
	char c;

	if (hfd < 0) {
		t_skip("inputsec", "no host");
		return;
	}
	kbd = open("/dev/kbd", O_RDONLY);
	if (kbd < 0 || ioctl(kbd, EVIOCBIND, fda) != 0 || pipe(go) < 0 || pipe(rdy) < 0) {
		t_fail("inputsec", "%s", T_ERR);
		return;
	}
	drain(kbd, 300);

	/* group display with root's fd: its session can't be bound, mapped or used */
	pid = fork();
	if (pid == 0) {
		setsid();
		setgid(DISPGID);
		setuid(NOBODY);
		err = 0;
		if ((fd = open("/dev/kbd", O_RDONLY)) < 0)
			_exit(2);
		if (ioctl(fd, EVIOCBIND, fda) == 0 || errno != EACCES)
			err |= 4;
		if (mmap(0, fi.fi_size, PROT_READ, MAP_SHARED, fda, 0) != (void *)-1 ||
		    errno != EACCES)
			err |= 8;
		cm.cm_start = 0;
		cm.cm_count = 2;
		cm.cm_red = r;
		cm.cm_green = g;
		cm.cm_blue = b;
		if (fi.fi_cmapsize >= 2 && ioctl(fda, FBIOGETCMAP, &cm) == 0)
			err |= 16;
		if (ioctl(fda, FBIOSWITCH, 0) == 0 || errno != EPERM)
			err |= 64;
		write(rdy[1], "r", 1);
		if (nextev(fd, &v, 2000))
			err |= 32;
		_exit(err);
	}
	read(rdy[0], &c, 1);
	host("key x");
	t_child("bind_other_refused", pid);
	t_check("root_reads_own", drain(kbd, 2000) == 2, "root's reader lost its keys");

	/* group display: own session in the back, an unbound reader, console in front */
	pid = fork();
	if (pid == 0) {
		setsid();
		setgid(DISPGID);
		setuid(NOBODY);
		err = 0;
		fd = fbopen();
		if (fd < 0 || (id = acquire(fd, 0, "inputsec")) <= 0)
			_exit(2);
		kb = open("/dev/kbd", O_RDONLY | O_NONBLOCK);
		ub = open("/dev/kbd", O_RDONLY | O_NONBLOCK);
		if (kb < 0 || ub < 0 || ioctl(kb, EVIOCBIND, fd) != 0)
			_exit(3);
		write(rdy[1], "r", 1);
		read(go[0], &c, 1);
		if (read(kb, &v, sizeof v) >= 0 || errno != EAGAIN)
			err |= 4;
		if (read(ub, &v, sizeof v) >= 0 || errno != EINVAL)
			err |= 8;
		_exit(err);
	}
	read(rdy[0], &c, 1);
	host("key y");
	err = ioctl(fda, FBIOSWITCH, 0) == 0 && waitfront(0L, 5);
	host("key shift");
	err = err && ioctl(fda, FBIOSWITCH, ida) == 0 && front() == ida;
	write(go[1], "g", 1);
	t_child("background_unbound_nothing", pid);
	t_check("console_round", err, "front %ld", front());
	drain(kbd, 300);

	/* group display at the console: its own front session reads */
	pid = fork();
	if (pid == 0) {
		setgid(DISPGID);
		setuid(NOBODY);
		err = 0;
		fd = fbopen();
		kb = open("/dev/kbd", O_RDONLY);
		if (fd < 0 || kb < 0 || (id = acquire(fd, 1, "inputsec")) <= 0 ||
		    ioctl(kb, EVIOCBIND, fd) != 0)
			_exit(2);
		write(rdy[1], "r", 1);
		if (!nextev(kb, &v, 3000) || v.ie_type != IE_KEY || v.ie_code != 0x06)
			err |= 4;
		if (ioctl(fd, FBIOSWITCH, ida) != 0)
			err |= 8;
		_exit(err);
	}
	read(rdy[0], &c, 1);
	host("key z");
	t_child("own_front_reads", pid);
	t_check("own_front_back", waitfront(ida, 5) && drain(kbd, 300) == 0, "front %ld", front());
	close(go[0]);
	close(go[1]);
	close(rdy[0]);
	close(rdy[1]);
	close(kbd);
}

/* dstest in front, kill -9: the console comes back with its text */
static void
t_kill()
{
	char r1[64], r2[64];
	pid_t pid;
	int status, fd, drawn, back, before;
	long t0;

	if (access("/usr/bin/dstest", X_OK) != 0) {
		t_skip("kill_owner", "no /usr/bin/dstest");
		return;
	}
	if (front() != 0) {
		t_fail("kill_owner", "console not in front (%ld)", front());
		return;
	}
	/* from here to the last dump nothing may reach the console */
	before = host("shot con_before") != 0;
	pid = fork();
	if (pid == 0) {
		fd = open("/dev/null", O_RDWR);
		dup2(fd, 1);
		dup2(fd, 2);
		execl("/usr/bin/dstest", "dstest", "draw", "-s", "3", "-t", "0", (char *)0);
		_exit(127);
	}
	t0 = t_now_ms();
	drawn = 0;
	while (t_now_ms() - t0 < 8000 && !(drawn = front() > 0))
		poll((struct pollfd *)0, 0, 100);
	sleep(2);		/* drawn, its table latched */
	strcpy(r1, "no host");
	if (drawn && host("shot dstest") && hostref("dstest_ref dstest", 3, 0))
		strcpy(r1, hrep);
	kill(pid, SIGKILL);
	t_waitchild(pid, &status, 10);
	back = waitfront(0L, 5);
	sleep(1);
	strcpy(r2, "no host");
	if (before && host("shot con_after") && host("cmp con_before con_after"))
		strcpy(r2, hrep);
	t_check("dstest_front", drawn, "dstest never came to front");
	if (hfd >= 0)
		t_check("shot_dstest", strcmp(r1, "same") == 0, "dstest pattern: %s", r1);
	t_check("kill_owner_console", back && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
	    "front %ld, status 0x%x", front(), status);
	if (hfd >= 0)
		t_check("console_restored", strcmp(r2, "same") == 0, "console screen: %s", r2);
}

int
main()
{
	unsigned char *fba = 0;
	long ida = -1;
	int fd;

	t_init("display", 300);
	fd = fbopen();
	if (fd < 0) {
		t_skip("open", "/dev/fb0: %s", T_ERR);
		return t_done();
	}
	t_pass("open");
	hostopen();
	t_ioctls(fd);
	t_session(fd, &ida, &fba);
	if (fba != 0 && fba != (unsigned char *)-1) {
		t_input(fd, ida, fba);
		t_perm(ida);
		t_inputsec(fd, ida);
		close(fd);
		t_check("close_mapped_kept", front() == ida, "front %ld after close", front());
		munmap(fba, fi.fi_size);
	} else
		close(fd);
	t_check("close_front", waitfront(0L, 5), "front %ld after the last close", front());
	t_kill();
	return t_done();
}
