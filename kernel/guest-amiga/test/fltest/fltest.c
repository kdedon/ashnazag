/*
 * fltest: trackdisk.device on units 0-3.  Writes SYS:T/fltest.txt (the
 * status of each unit), T:rawN.bin and T:rauN.bin (raw reads of track 2,
 * word-synced and not)
 * and T:secN.bin (track 3's sectors).  "fltest watch": DF0's change
 * count and state each second for four minutes, to SYS:T/watch.txt.
 */
typedef unsigned int U;
typedef unsigned char B;
extern U call(void *, int, U *);
#define EXEC execbase()
static void *execbase(void) { void *b; __asm__("move.l 4.w,%0" : "=a"(b)); return b; }
#define D0 0
#define D1 1
#define D2 2
#define D3 3
#define A0 4
#define A1 5
static U r[7];
static B ior[56], port[34], line[80];
static void *dos;
static U out;

static U ex(void *base, int lvo) { U v = call(base, lvo, r); int i; for (i = 0; i < 7; i++) r[i] = 0; return v; }
static void put(const char *s) { U n = 0; while (s[n]) n++; r[D1] = out; r[D2] = (U)s; r[D3] = n; ex(dos, -48); }
static void hex(const char *label, U v)
{
	int i, n = 0;
	while (label[n]) { line[n] = label[n]; n++; }
	for (i = 7; i >= 0; i--) line[n++] = "0123456789abcdef"[v >> (4 * i) & 15];
	line[n++] = '\n'; line[n] = 0;
	put((char *)line);
}
static U io(U cmd, U len, void *data, U off, B flags)
{
	*(unsigned short *)(ior + 28) = cmd; ior[30] = flags; ior[31] = 0;
	*(U *)(ior + 32) = 0; *(U *)(ior + 36) = len; *(U *)(ior + 40) = (U)data; *(U *)(ior + 44) = off;
	/* BeginIO: DoIO would replace io_Flags */
	r[A1] = (U)ior; ex(*(void **)(ior + 20), -30);
	r[A1] = (U)ior; ex(EXEC, -474);
	return ior[31] | *(U *)(ior + 32) << 8;
}
static void save(const char *name, void *data, U len)
{
	U f;
	r[D1] = (U)name; r[D2] = 1006; f = ex(dos, -30);
	if (!f) { put("open failed\n"); return; }
	r[D1] = f; r[D2] = (U)data; r[D3] = len; ex(dos, -48);
	r[D1] = f; ex(dos, -36);
}

static void watch(void)
{
	U i, n, st, last = ~0U;
	r[D1] = (U)"SYS:T/watch.txt"; r[D2] = 1006; out = ex(dos, -30);
	r[A0] = (U)"trackdisk.device"; r[D0] = 0; r[A1] = (U)ior; r[D1] = 0;
	if (!out || ex(EXEC, -444))
		return;
	for (i = 0; i < 240; i++) {
		n = io(13, 0, 0, 0, 0) >> 8;
		st = io(14, 0, 0, 0, 0) >> 8;
		if ((n << 1 | st) != last) {
			hex("second ", i);
			hex("changenum ", n);
			hex("nodisk ", st);
			last = n << 1 | st;
		}
		r[D1] = 50; ex(dos, -198);
	}
	r[A1] = (U)ior; ex(EXEC, -450);
	r[D1] = out; ex(dos, -36);
}

void fltest(char *args)
{
	static char rawname[] = "SYS:T/raw0.bin", secname[] = "SYS:T/sec0.bin";
	B *buf;
	U unit, v;
	r[A1] = (U)"dos.library"; r[D0] = 36; dos = (void *)ex(EXEC, -552);
	if (!dos) return;
	r[D0] = (U)-1; v = ex(EXEC, -330);
	port[8] = 4; port[15] = v;
	r[A1] = 0; *(U *)(port + 16) = ex(EXEC, -294);
	*(U *)(port + 20) = (U)(port + 24); *(U *)(port + 28) = (U)(port + 20);
	ior[8] = 5; *(U *)(ior + 14) = (U)port; *(unsigned short *)(ior + 18) = 56;
	if (args[0] == 'w') {
		watch();
		return;
	}
	r[D1] = (U)"SYS:T/fltest.txt"; r[D2] = 1006; out = ex(dos, -30);
	if (!out) return;
	r[D0] = 16000; r[D1] = 0x10003; buf = (B *)ex(EXEC, -198);
	for (unit = 0; unit < 4; unit++) {
		hex("unit ", unit);
		r[A0] = (U)"trackdisk.device"; r[D0] = unit; r[A1] = (U)ior; r[D1] = 0;
		v = ex(EXEC, -444);
		hex("open ", v);
		if (v)
			continue;
		hex("changenum ", io(13, 0, 0, 0, 0));
		hex("changestate ", io(14, 0, 0, 0, 0));
		hex("protstatus ", io(15, 0, 0, 0, 0));
		hex("drivetype ", io(18, 0, 0, 0, 0));
		hex("rawread ", io(16, 12000, buf, 2, 0x20));
		rawname[9] = '0' + unit;
		save(rawname, buf, 12000);
		hex("rawunsynced ", io(16, 12000, buf, 2, 0));
		rawname[8] = 'u';
		save(rawname, buf, 12000);
		rawname[8] = 'w';
		hex("read ", io(2, 5632, buf, 3 * 5632, 0));
		secname[9] = '0' + unit;
		save(secname, buf, 5632);
		hex("motor ", io(9, 0, 0, 0, 0));
		r[A1] = (U)ior; ex(EXEC, -450);
	}
	r[D1] = out; ex(dos, -36);
}
