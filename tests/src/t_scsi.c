/*
 * t_scsi -- 53C96 data modes: block-stamped write and read-back on a
 * scratch disk at SCSI ID 1 in modes 2 and 4, raw and through the
 * buffer cache, with the driver's 64 KB raw commands, merging and
 * read-ahead on and off; the first 10 MB of the disk at ID 0 must read
 * the same in every setting.  Writes go only to a disk whose block 0 carries
 * the scratch marker.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mkdev.h>
#include "t.h"

#define	MARK	"ASHNAZAG SCSI SCRATCH"
#define	BS	512
#define	REG	2048			/* blocks per region */
#define	ROOTMB	10
#define	R0	"/tmp/scsi_r0"
#define	R1	"/tmp/scsi_r1"
#define	B1	"/tmp/scsi_b1"

extern int getksym();

static char *xname[] = { "ncr_xcmd", "ncr_xbytes", "ncr_xphase", "ncr_xresume",
	"ncr_xfall", "ncr_ncmd", "ncr_nerr", "ncr_nchunk", "ncr_nfault",
	"sd_nraw", "sd_nmerge", "sd_nra", "sd_nhit", "sd_nsplit" };
#define	NX	(sizeof xname / sizeof xname[0])

static long *buf;

static int
kset(char *name, long v)
{
	unsigned long a = 0, info;
	int fd, ok;

	if (getksym(name, &a, &info) < 0 || (fd = open("/dev/kmem", O_RDWR)) < 0)
		return -1;
	ok = lseek(fd, (off_t)a, 0) != -1 && write(fd, (char *)&v, 4) == 4;
	close(fd);
	return ok ? 0 : -1;
}

static long
kget(char *name)
{
	unsigned long a = 0, info;
	long v = -1;
	int fd;

	if (getksym(name, &a, &info) < 0 || (fd = open("/dev/kmem", O_RDONLY)) < 0)
		return -1;
	if (lseek(fd, (off_t)a, 0) == -1 || read(fd, (char *)&v, 4) != 4)
		v = -1;
	close(fd);
	return v;
}

static void
snap(long *v)
{
	int i;

	for (i = 0; i < NX; i++)
		v[i] = kget(xname[i]);
}

static void
stamp(long *p, long blk, long seed)
{
	int i;

	p[0] = blk;
	p[1] = seed;
	for (i = 2; i < BS / 4; i++)
		p[i] = blk * 2654435761UL + seed * 40503 + i * 977;
}

/* number of the first bad block in n blocks from blk, or -1 */
static long
verify(long *p, long blk, long n, long seed)
{
	long w[BS / 4];
	long b;

	for (b = 0; b < n; b++) {
		stamp(w, blk + b, seed);
		if (memcmp(w, p + b * (BS / 4), BS) != 0)
			return blk + b;
	}
	return -1;
}

static int bsw[] = { 1, 3, 8, 128, 7, 64, 2, 33 };	/* write sizes, blocks */
static int bsr[] = { 128, 5, 1, 16, 127, 2, 64, 9 };	/* read sizes */

/* Write REG stamped blocks from blk through path; 0 or -1. */
static int
wregion(char *path, long blk, long seed, int *sz)
{
	long b, n, k;
	int fd, i, ok = 1;

	if ((fd = open(path, O_WRONLY)) < 0)
		return -1;
	for (b = 0, i = 0; ok && b < REG; b += n, i++) {
		n = sz[i % 8];
		if (n > REG - b)
			n = REG - b;
		for (k = 0; k < n; k++)
			stamp(buf + k * (BS / 4), blk + b + k, seed);
		ok = lseek(fd, (off_t)(blk + b) * BS, 0) != -1
		    && write(fd, (char *)buf, n * BS) == n * BS;
	}
	close(fd);
	return ok ? 0 : -1;
}

/* Read REG blocks from blk through path and check them: -1 good, else bad block or -2 */
static long
rregion(char *path, long blk, long seed, int *sz)
{
	long b, n, bad = -1;
	int fd, i;

	if ((fd = open(path, O_RDONLY)) < 0)
		return -2;
	for (b = 0, i = 0; bad == -1 && b < REG; b += n, i++) {
		n = sz[i % 8];
		if (n > REG - b)
			n = REG - b;
		memset(buf, 0x5A, n * BS);
		if (lseek(fd, (off_t)(blk + b) * BS, 0) == -1
		|| read(fd, (char *)buf, n * BS) != n * BS)
			bad = -2;
		else
			bad = verify(buf, blk + b, n, seed);
	}
	close(fd);
	return bad;
}

/* Sum of the first ROOTMB MB of path, read 64 KB at a time; ms in *ms. */
static unsigned long
rootsum(char *path, long *ms, int *ok)
{
	unsigned long s = 0;
	long t, i, j;
	int fd;

	*ok = 0;
	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	t = t_now_ms();
	for (i = 0; i < ROOTMB * 16; i++) {
		if (read(fd, (char *)buf, 65536) != 65536) {
			close(fd);
			return 0;
		}
		for (j = 0; j < 65536 / 4; j++)
			s = s * 31 + buf[j];
	}
	*ms = t_now_ms() - t;
	close(fd);
	*ok = 1;
	return s;
}

static void
counters(char *name, long *a, long *b)
{
	t_info(name, "xcmd %ld xbytes %ld xphase %ld xresume %ld xfall %ld ncmd %ld nerr %ld chunk %ld fault %ld",
	    b[0] - a[0], b[1] - a[1], b[2] - a[2], b[3] - a[3], b[4] - a[4],
	    b[5] - a[5], b[6] - a[6], b[7] - a[7], b[8] - a[8]);
	t_info(name, "raw %ld merge %ld ra %ld hit %ld split %ld",
	    b[9] - a[9], b[10] - a[10], b[11] - a[11], b[12] - a[12], b[13] - a[13]);
}

/* the driver's request shaping: 1 on, 0 the stock paths */
static void
shaping(int on)
{
	kset("sd_bigraw", (long)on);
	kset("sd_clust", (long)on);
	kset("ncr_xipl", (long)on);
}

/* Sequential 8 KB block reads of n MB from blk; SCSI commands per MB. */
static long
cmdsper(char *path, long blk, int n)
{
	long a, i;
	int fd;

	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	a = kget("ncr_ncmd");
	lseek(fd, (off_t)blk * BS, 0);
	for (i = 0; i < n * 128; i++)
		if (read(fd, (char *)buf, 8192) != 8192) {
			close(fd);
			return -1;
		}
	close(fd);
	return (kget("ncr_ncmd") - a) / n;
}

static int
scratchok()
{
	int fd;

	if ((fd = open(R1, O_RDONLY)) < 0 || read(fd, (char *)buf, BS) != BS) {
		t_skip("scratch", "no disk at ID 1");
		if (fd >= 0)
			close(fd);
		return 0;
	}
	close(fd);
	if (memcmp(buf, MARK, sizeof MARK - 1) != 0) {
		t_skip("scratch", "ID 1 is not a scratch disk");
		return 0;
	}
	return 1;
}

/* Modes 2 and 4 with shaping on (s 1) or off (s 0), regions 3s+1 .. 3s+3. */
static void
test_scratch(int s)
{
	static int modes[] = { 2, 4 };
	char name[64];
	long a[NX], b[NX], bad, r0 = 1 + 3 * s * REG;
	int fd, m, i;

	shaping(s);
	for (i = 0; i < 2; i++) {
		m = modes[i];
		kset("ncr_pdmamode", (long)m);
		snap(a);
		sprintf(name, "s%d_m%d_raw_write", s, m);
		t_check(name, wregion(R1, r0 + i * REG, m, bsw) == 0, "%s", T_ERR);
		sprintf(name, "s%d_m%d_raw_read", s, m);
		bad = rregion(R1, r0 + i * REG, m, bsr);
		t_check(name, bad == -1, "block %ld", bad);
		sprintf(name, "s%d_m%d_block_read", s, m);
		bad = rregion(B1, r0 + i * REG, m, bsw);
		t_check(name, bad == -1, "block %ld", bad);
		snap(b);
		sprintf(name, "s%d_m%d_counters", s, m);
		counters(name, a, b);
		sprintf(name, "s%d_m%d_paths", s, m);
		if (s)
			t_check(name, b[9] > a[9] && b[11] > a[11] && b[12] > a[12],
			    "raw %ld ra %ld hit %ld", b[9] - a[9], b[11] - a[11], b[12] - a[12]);
		else
			t_check(name, b[9] == a[9] && b[11] == a[11] && b[12] == a[12],
			    "raw %ld ra %ld hit %ld", b[9] - a[9], b[11] - a[11], b[12] - a[12]);
		if (m == 4)
			t_check(s ? "s1_m4_used" : "s0_m4_used", b[0] > a[0] && b[4] == a[4] && b[6] == a[6],
			    "xcmd %ld xfall %ld nerr %ld", b[0] - a[0], b[4] - a[4], b[6] - a[6]);
		else
			t_check(s ? "s1_m2_used" : "s0_m2_used", b[0] == a[0] && b[7] > a[7] && b[6] == a[6],
			    "xcmd %ld chunk %ld nerr %ld", b[0] - a[0], b[7] - a[7], b[6] - a[6]);
	}
	/* each mode reads what the other wrote */
	kset("ncr_pdmamode", 4L);
	bad = rregion(R1, r0, 2, bsw);
	t_check(s ? "s1_m4_reads_m2" : "s0_m4_reads_m2", bad == -1, "block %ld", bad);
	kset("ncr_pdmamode", 2L);
	bad = rregion(R1, r0 + REG, 4, bsw);
	t_check(s ? "s1_m2_reads_m4" : "s0_m2_reads_m4", bad == -1, "block %ld", bad);
	/* through the buffer cache in mode 4, back raw in both */
	kset("ncr_pdmamode", 4L);
	snap(a);
	t_check(s ? "s1_block_write" : "s0_block_write",
	    wregion(B1, r0 + 2 * REG, 7, bsr) == 0, "%s", T_ERR);
	sync();
	if ((fd = open(B1, O_RDONLY)) >= 0)
		close(fd);
	snap(b);
	counters(s ? "s1_block_write_counters" : "s0_block_write_counters", a, b);
	bad = rregion(R1, r0 + 2 * REG, 7, bsw);
	t_check(s ? "s1_block_write_raw4" : "s0_block_write_raw4", bad == -1, "block %ld", bad);
	kset("ncr_pdmamode", 2L);
	bad = rregion(R1, r0 + 2 * REG, 7, bsr);
	t_check(s ? "s1_block_write_raw2" : "s0_block_write_raw2", bad == -1, "block %ld", bad);
}

/*
 * Shaping on: block reads issue few commands; raw writes and stock-path
 * writes over read-ahead data are seen by later block reads.
 */
static void
test_shaping()
{
	long r = 1 + 6 * REG, bad, n2, n4;
	int fd;

	shaping(1);
	kset("ncr_pdmamode", 4L);
	t_check("ra_fill", wregion(R1, r, 11, bsw) == 0, "%s", T_ERR);
	n4 = cmdsper(B1, r, 1);
	shaping(0);
	n2 = cmdsper(B1, r + REG, 1);
	shaping(1);
	t_info("cmds_per_mb", "block 8 KB reads: %ld commands per MB shaped, %ld stock", n4, n2);
	t_check("cmds_per_mb", n4 > 0 && n4 <= 20, "%ld", n4);
	/* fill the read-ahead, then overwrite raw and read through the block device */
	bad = rregion(B1, r, 11, bsr);
	t_check("ra_read", bad == -1, "block %ld", bad);
	t_check("ra_rawover", wregion(R1, r, 12, bsr) == 0, "%s", T_ERR);
	if ((fd = open(B1, O_RDONLY)) >= 0)
		close(fd);
	bad = rregion(B1, r, 12, bsw);
	t_check("ra_sees_raw", bad == -1, "block %ld", bad);
	/* the same with the stock raw path doing the write */
	kset("sd_bigraw", 0L);
	t_check("ra_stockover", wregion(R1, r, 13, bsw) == 0, "%s", T_ERR);
	kset("sd_bigraw", 1L);
	bad = rregion(B1, r, 13, bsr);
	t_check("ra_sees_stock", bad == -1, "block %ld", bad);
}

/* Longest IPL-2 stretch of the SCSI handler during 10 MB raw reads, data at IPL 1 and 2. */
static void
test_ipl()
{
	long ms, v1, v2;
	int ok;

	kset("ncr_pdmamode", 4L);
	kset("ncr_xipl", 1L);
	kset("ncr_ipl2max", 0L);
	(void)rootsum(R0, &ms, &ok);
	if (!ok) {
		t_skip("ipl2max", "no %d MB disk at ID 0", ROOTMB);
		return;
	}
	v1 = kget("ncr_ipl2max");
	kset("ncr_xipl", 0L);
	kset("ncr_ipl2max", 0L);
	(void)rootsum(R0, &ms, &ok);
	v2 = kget("ncr_ipl2max");
	kset("ncr_xipl", 1L);
	t_info("ipl2max", "longest IPL-2 stretch: %ld us data at IPL 1, %ld us at IPL 2 (nested %ld)",
	    v1, v2, kget("ncr_nnest"));
	t_check("ipl2max", ok && v1 >= 0 && v2 >= 0, "%ld %ld", v1, v2);
}

static void
test_root()
{
	unsigned long s2, s4, sb;
	long ms2 = 0, ms4 = 0, msb = 0, a[NX], b[NX];
	int ok2, ok4, okb;

	shaping(0);
	kset("ncr_pdmamode", 2L);
	s2 = rootsum(R0, &ms2, &ok2);
	if (!ok2) {
		t_skip("root_sum", "no %d MB disk at ID 0", ROOTMB);
		return;
	}
	shaping(1);
	kset("ncr_pdmamode", 4L);
	snap(a);
	s4 = rootsum(R0, &ms4, &ok4);
	snap(b);
	counters("root_m4_counters", a, b);
	t_check("root_sum_m4", ok4 && s4 == s2, "mode 2 %08lx, mode 4 %08lx", s2, s4);
	t_info("root_time", "%d MB raw: mode 2 stock %ld ms, mode 4 shaped %ld ms", ROOTMB, ms2, ms4);
	sb = 0;
	okb = 0;
	if (mknod("/tmp/scsi_b0", S_IFBLK | 0600, makedev(18, 0)) == 0 || errno == EEXIST)
		sb = rootsum("/tmp/scsi_b0", &msb, &okb);
	t_check("root_sum_block", okb && sb == s2, "raw %08lx, block %08lx", s2, sb);
	t_info("root_time_block", "%d MB block, mode 4 shaped: %ld ms", ROOTMB, msb);
	if (okb) {
		shaping(0);
		sb = rootsum("/tmp/scsi_b0", &msb, &okb);
		shaping(1);
		t_check("root_sum_block_stock", okb && sb == s2, "raw %08lx, block %08lx", s2, sb);
		t_info("root_time_block_stock", "%d MB block, mode 4 stock: %ld ms", ROOTMB, msb);
	}
	unlink("/tmp/scsi_b0");
}

/* scsibench on the disk at ID 0: it must restore the settings and find the data the same */
static void
test_bench()
{
	static char *node[] = { "/dev/rdsk/c0d0s0", "/dev/rdsk/c0d0s1", "/dev/dsk/c0d0s0", "/dev/dsk/c0d0s1" };
	static int mn[] = { 0, 16, 0, 16 };
	char line[200], *v[4];
	long before[4];
	FILE *f;
	int i, st, same = 0, set = 1, mode, wide = 0, made[4];

	if (access("/tests/scsibench", 0) != 0 || access("/usr/bin/sum", 0) != 0) {
		t_skip("bench", "no scsibench in this root");
		return;
	}
	for (i = 0; i < 4; i++)
		if (!(made[i] = mknod(node[i], (i < 2 ? S_IFCHR : S_IFBLK) | 0600,
		    makedev(i < 2 ? 40 : 18, mn[i])) == 0) && errno != EEXIST) {
			t_fail("bench_nodes", "%s: %s", node[i], T_ERR);
			return;
		}
	/* odd settings, to see them restored */
	v[0] = "ncr_pdmamode"; v[1] = "sd_bigraw"; v[2] = "sd_clust"; v[3] = "ncr_xipl";
	kset("ncr_pdmamode", 4L);
	kset("sd_bigraw", 1L);
	kset("sd_clust", 0L);
	kset("ncr_xipl", 1L);
	for (i = 0; i < 4; i++)
		before[i] = kget(v[i]);
	mode = 0;
	if ((f = popen("PATH=/tests:/usr/bin:/sbin sh /tests/scsibench -v c0d0s1 2>&1", "r")) == 0) {
		t_fail("bench", "popen: %s", T_ERR);
		return;
	}
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		t_info("bench", "%s", line);
		if (strlen(line) > 80)
			wide++;
		if (strcmp(line, "data: same in every setting") == 0)
			same = 1;
		if (strncmp(line, "mode 4, shaping on ", 19) == 0)
			mode = 1;
	}
	st = pclose(f);
	for (i = 0; i < 4; i++)
		if (kget(v[i]) != before[i])
			set = 0;
	t_check("bench_runs", st == 0 && mode, "status 0x%x", st);
	t_check("bench_data", same, "no 'data: same' line");
	t_check("bench_restores", set, "settings changed");
	t_check("bench_width", wide == 0, "%d lines over 80 columns", wide);
	/* other tests use the nodes that were already there */
	for (i = 0; i < 4; i++)
		if (made[i])
			unlink(node[i]);
}

main()
{
	long mode;
	int fd;

	t_init("scsi", 600);
	buf = (long *)malloc(65536);
	if ((mknod(R0, S_IFCHR | 0600, makedev(40, 0)) && errno != EEXIST)
	|| (mknod(R1, S_IFCHR | 0600, makedev(40, 1)) && errno != EEXIST)
	|| (mknod(B1, S_IFBLK | 0600, makedev(18, 1)) && errno != EEXIST)) {
		t_fail("nodes", "%s", T_ERR);
		return t_done();
	}
	/* the first open loads the driver */
	if ((fd = open(R1, O_RDONLY)) >= 0 || (fd = open(R0, O_RDONLY)) >= 0)
		close(fd);
	mode = kget("ncr_pdmamode");
	if (mode < 0 || kget("ncr_xcmd") < 0) {
		t_skip("modes", "no 53C96 driver");
		return t_done();
	}
	if (scratchok()) {
		test_scratch(1);
		test_scratch(0);
		test_shaping();
	}
	test_root();
	test_bench();
	test_ipl();
	shaping(1);
	kset("ncr_pdmamode", mode);
	t_check("mode_restored", kget("ncr_pdmamode") == mode, "%ld", kget("ncr_pdmamode"));
	unlink(R0);
	unlink(R1);
	unlink(B1);
	return t_done();
}
