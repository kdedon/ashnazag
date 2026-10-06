/*
 * utest.c -- the host drives from inside TOS, run from C:\AUTO.  Each
 * check becomes "PASS name" or "FAIL name value" in U:\RESULT.TXT,
 * which t_tos reads on the host.  Expects the trees t_tos makes: U:,
 * G: read-only and I: on one directory, H: on U:\SUB.
 */

extern long trap1(), trap13(), irqtest(), traptest9(), traptest10(), traptest0();

static short w[8];
static int nw;
static char out[2048], buf[256], dta[44], tilde[14];
static int on;

static void
pw(v)
	int v;
{
	w[nw++] = v;
}

static void
pl(v)
	long v;
{
	w[nw++] = v >> 16;
	w[nw++] = v;
}

static long
go()
{
	int n = nw;

	nw = 0;
	return trap1(w, n);
}

static long Fopen(n, m) char *n; { pw(0x3d); pl((long)n); pw(m); return go(); }
static long Fcreate(n, a) char *n; { pw(0x3c); pl((long)n); pw(a); return go(); }
static long Fclose(h) { pw(0x3e); pw(h); return go(); }
static long Fread(h, c, b) long c; char *b; { pw(0x3f); pw(h); pl(c); pl((long)b); return go(); }
static long Fwrite(h, c, b) long c; char *b; { pw(0x40); pw(h); pl(c); pl((long)b); return go(); }
static long Fdelete(n) char *n; { pw(0x41); pl((long)n); return go(); }
static long Frename(a, b) char *a, *b; { pw(0x56); pw(0); pl((long)a); pl((long)b); return go(); }
static long Fsetdta(d) char *d; { pw(0x1a); pl((long)d); return go(); }
static long Fsfirst(n, a) char *n; { pw(0x4e); pl((long)n); pw(a); return go(); }
static long Fsnext() { pw(0x4f); return go(); }
static long Fattrib(n, f, a) char *n; { pw(0x43); pl((long)n); pw(f); pw(a); return go(); }
static long Fdatime(b, h, f) char *b; { pw(0x57); pl((long)b); pw(h); pw(f); return go(); }
static long Dcreate(n) char *n; { pw(0x39); pl((long)n); return go(); }
static long Ddelete(n) char *n; { pw(0x3a); pl((long)n); return go(); }
static long Dsetpath(n) char *n; { pw(0x3b); pl((long)n); return go(); }
static long Dgetpath(b, d) char *b; { pw(0x47); pl((long)b); pw(d); return go(); }
static long Dsetdrv(d) { pw(0x0e); pw(d); return go(); }
static long Dgetdrv() { pw(0x19); return go(); }
static long Dfree(b, d) char *b; { pw(0x36); pl((long)b); pw(d); return go(); }
static long Pexec(m, n, c, e) char *n, *c, *e; { pw(0x4b); pw(m); pl((long)n); pl((long)c); pl((long)e); return go(); }

static long
Drvmap()
{
	w[0] = 10;
	return trap13(w, 1);
}

static int
same(a, b)
	char *a, *b;
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static void
put(s)
	char *s;
{
	while (*s && on < sizeof out - 1)
		out[on++] = *s++;
}

static void
check(name, ok, v)
	char *name;
	int ok;
	long v;
{
	char n[12];
	int i = 11;
	unsigned long u;

	put(ok ? "PASS " : "FAIL ");
	put(name);
	if (!ok) {
		u = v < 0 ? -v : v;
		n[i] = 0;
		do
			n[--i] = '0' + u % 10;
		while ((u /= 10) != 0);
		if (v < 0)
			n[--i] = '-';
		put(" ");
		put(n + i);
	}
	put("\r\n");
}

static void
result()
{
	long h = Fcreate("U:\\RESULT.TXT", 0);

	if (h >= 0) {
		Fwrite((int)h, (long)on, out);
		Fclose((int)h);
	}
}

/* in directory pattern p, a subdirectory whose name starts with s: its path into o */
static int
find(p, s, o)
	char *p, *s, *o;
{
	char d[128];
	long r;
	int i, k;

	for (i = 0; p[i] && i < 100; i++)
		d[i] = p[i];
	d[i] = 0;
	if (i < 4 || !same(d + i - 4, "\\*.*")) {
		d[i++] = '\\'; d[i++] = '*'; d[i++] = '.'; d[i++] = '*'; d[i] = 0;
	}
	Fsetdta(dta);
	for (r = Fsfirst(d, 0x10); r == 0; r = Fsnext()) {
		for (k = 0; s[k] && dta[30 + k] == s[k]; k++)
			;
		if (s[k] == 0 && (dta[21] & 0x10)) {
			i -= 3;
			for (k = 0; dta[30 + k]; k++)
				d[i++] = dta[30 + k];
			d[i] = 0;
			for (k = 0; k <= i; k++)
				o[k] = d[k];
			return 1;
		}
	}
	return 0;
}

/* the whole file into buf; its length */
static long
slurp(n)
	char *n;
{
	long h = Fopen(n, 0), r;

	if (h < 0)
		return h;
	r = Fread((int)h, (long)sizeof buf - 1, buf);
	Fclose((int)h);
	buf[r > 0 ? r : 0] = 0;
	return r;
}

int
main()
{
	static char wr[] = "written by tos\n";
	long r, h;
	int found = 0, d;

	if (!(Drvmap() & (1L << 20)))
		return 0;
	check("drvmap", 1, 0L);

	Fsetdta(dta);
	for (r = Fsfirst("U:\\*.*", 0x10); r == 0; r = Fsnext()) {
		if (same(dta + 30, "README.TXT"))
			found |= 1;
		if (same(dta + 30, "HELLO.PRG"))
			found |= 2;
		if (same(dta + 30, "SUB") && (dta[21] & 0x10))
			found |= 4;
		if (dta[33] == '~' && !(dta[21] & 0x10)) {
			found |= 8;
			for (d = 0; d < 13; d++)
				tilde[d] = dta[30 + d];
		}
	}
	check("list", found == 15 && r == -49, (long)found);

	r = slurp("U:\\README.TXT");
	check("read", r == 16 && same(buf, "hello from unix\n"), r);
	buf[0] = 'U'; buf[1] = ':'; buf[2] = '\\';
	for (d = 0; tilde[d]; d++)
		buf[3 + d] = tilde[d];
	buf[3 + d] = 0;
	r = slurp(buf);
	check("long_name", r == 5 && same(buf, "long\n"), r);

	h = Fcreate("U:\\SUB\\NEW.TXT", 0);
	r = h < 0 ? h : Fwrite((int)h, (long)sizeof wr - 1, wr);
	if (h >= 0)
		Fclose((int)h);
	check("write", r == sizeof wr - 1 && slurp("U:\\SUB\\NEW.TXT") == r && same(buf, wr), r);

	r = Frename("U:\\SUB\\NEW.TXT", "U:\\SUB\\RENAMED.TXT");
	check("rename", r == 0 && Fopen("U:\\SUB\\NEW.TXT", 0) == -33 &&
	    slurp("U:\\SUB\\RENAMED.TXT") == sizeof wr - 1, r);

	h = Fcreate("U:\\GONE.TXT", 0);
	if (h >= 0)
		Fclose((int)h);
	r = Fdelete("U:\\GONE.TXT");
	check("delete", h >= 0 && r == 0 && Fopen("U:\\GONE.TXT", 0) == -33, r);

	r = Dcreate("U:\\NEWDIR");
	check("mkdir", r == 0 && Fsfirst("U:\\NEWDIR", 0x10) == 0 && Ddelete("U:\\NEWDIR") == 0 &&
	    Fsfirst("U:\\NEWDIR", 0x10) < 0, r);

	d = Dgetdrv();
	Dsetdrv(20);
	r = Dsetpath("\\SUB");
	Dgetpath(buf, 0);
	h = same(buf, "\\SUB") ? slurp("RENAMED.TXT") : -1;
	Dsetpath("\\");
	Dsetdrv(d);
	check("cwd", r == 0 && h == sizeof wr - 1, h);

	r = Fattrib("U:\\README.TXT", 0, 0);
	h = Fopen("U:\\README.TXT", 0);
	buf[2] = buf[3] = 0;
	if (h >= 0) {
		Fdatime(buf, (int)h, 0);
		Fclose((int)h);
	}
	check("attrib", r == 0 && (buf[2] | buf[3]) != 0, r);

	r = Dfree(buf, 21);
	check("dfree", r == 0, r);

	r = Pexec(0, "U:\\HELLO.PRG", "\0", (char *)0);
	check("pexec", r == 42, r);

	r = slurp("U:\\IN\\RENAMED.TXT");
	h = slurp("U:\\ABS\\RENAMED.TXT");
	check("links_inside", r == sizeof wr - 1 && h == r, r);
	r = Fopen("U:\\..\\..\\ETC\\PASSWD", 0);
	check("escape_dotdot", r < 0, r);
	r = Fopen("U:\\OUT\\PASSWD", 0);
	check("escape_link", r < 0, r);
	r = Fopen("U:\\UP\\ETC\\PASSWD", 0);
	check("escape_uplink", r < 0, r);
	Dsetpath("U:\\..\\..");
	Dgetpath(buf, 21);
	check("escape_cwd", buf[0] == 0, (long)buf[0]);

	/* U:\DXX~...\EXX~...: 201-character names below, a 250-character one inside */
	r = -1;
	if (find("U:\\*.*", "DXX~", buf) && find(buf, "EXX~", buf)) {
		for (d = 0; buf[d]; d++)
			;
		buf[d] = '\\'; buf[d + 1] = '*'; buf[d + 2] = '.'; buf[d + 3] = '*'; buf[d + 4] = 0;
		found = 0;
		Fsetdta(dta);
		for (r = Fsfirst(buf, 0); r == 0; r = Fsnext())
			found |= same(dta + 30, "OK.TXT");
	}
	check("long_path", found && r == -49 && slurp("U:\\README.TXT") == 16, r);

	/* C:: writable, or the read-only system folder */
	check("c_auto", Fsfirst("C:\\AUTO\\UTEST.PRG", 0) == 0, 0L);
	h = Fcreate("C:\\CTEST.TXT", 0);
	if (h >= 0) {
		r = Fwrite((int)h, (long)sizeof wr - 1, wr);
		Fclose((int)h);
		check("c_rw", r == sizeof wr - 1 && slurp("C:\\CTEST.TXT") == r, r);
	} else
		check("c_ro", h == -36 && Fdelete("C:\\AUTO\\UTEST.PRG") == -36 &&
		    Dcreate("C:\\NEW") == -36 && Fopen("C:\\AUTO\\UTEST.PRG", 2) == -36, h);

	/* C:'s files opened and closed, as a copy does; .env, the session's lock, is not one */
	found = Fopen("C:\\.ENV", 0) < 0;
	Fsetdta(dta);
	for (r = Fsfirst("C:\\*.*", 0x07); r == 0; r = Fsnext()) {
		buf[0] = 'C'; buf[1] = ':'; buf[2] = '\\';
		for (d = 0; d < 13 && dta[30 + d]; d++)
			buf[3 + d] = dta[30 + d];
		buf[3 + d] = 0;
		found &= !(dta[30] == 'E' && dta[31] == 'N' && dta[32] == 'V' && dta[33] == '~');
		if ((h = Fopen(buf, 0)) >= 0)
			Fclose((int)h);
	}
	check("env_hidden", found, r);

	/* G: read-only, I: the same directory writable */
	r = slurp("G:\\README.TXT");
	check("drive_g", (Drvmap() & 1L << 6) && r == 6 && same(buf, "drive\n") &&
	    Fcreate("G:\\W.TXT", 0) == -36, r);
	if (Drvmap() & 1L << 8) {
		h = Fcreate("I:\\W.TXT", 0);
		if (h >= 0)
			Fclose((int)h);
		r = Fsfirst("G:\\W.TXT", 0);
		check("drive_d", h >= 0 && r == 0 && Fdelete("I:\\W.TXT") == 0, h);
	}
	if (Drvmap() & 1L << 7)
		check("drive_h", slurp("H:\\RENAMED.TXT") == sizeof wr - 1, 0L);

	/* each drive its own current directory */
	d = Dgetdrv();
	Dsetdrv(20);
	r = Dsetpath("\\SUB");
	h = Dsetpath("C:\\AUTO");
	Dgetpath(buf, 3);
	found = same(buf, "\\AUTO");
	Dgetpath(buf, 21);
	found += same(buf, "\\SUB");
	found += slurp("U:RENAMED.TXT") == sizeof wr - 1;
	found += Fsfirst("C:UTEST.PRG", 0) == 0;
	Dsetpath("C:\\");
	Dsetpath("\\");
	Dsetdrv(d);
	check("drive_cwd", r == 0 && h == 0 && found == 4, (long)found);

	check("irq_rte", (r = irqtest(1)) == 0, r);
	check("irq_movesr", (r = irqtest(2)) == 0, r);
	check("irq_mask", (r = irqtest(3)) == 0, r);
	check("irq_nest", (r = irqtest(4)) == 0, r);

	check("trap9", (r = traptest9()) == 0, r);
	check("trap10", (r = traptest10()) == 0, r);
	check("trap0", (r = traptest0()) == 0, r);
	result();
	return 0;
}
