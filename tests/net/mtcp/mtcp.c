/*
 * mtcp.c -- a Mac application for t_mactcp: MacTCP's .IPP driver
 * (address, TCPCreate, TCPActiveOpen to the echo service, send,
 * receive the echo, close, release).  Started by the Finder as a
 * startup item; lines go to startmac's standard output through A/UX write(2):
 * "mtcp P name", "mtcp F name: detail", "mtcp I name value", "mtcp done".
 *
 * Freestanding, position independent, no globals: one CODE segment.
 */

#define	ECHOHOST	0x0a000264	/* 10.0.2.100 */
#define	ECHOPORT	7
#define	MSGLEN		24

extern long auxwrite(), trapopen(), trapctl(), newptr();
extern void idle(), probe0(), probe15();

typedef struct {
	char	fill[12];
	long	compl;		/* 12 */
	short	result;		/* 16 */
	char	*name;		/* 18 */
	short	vref;		/* 22 */
	short	cref;		/* 24 */
	short	code;		/* 26 */
	long	stream;		/* 28 */
	char	p[80];		/* 32: csParam */
} pb_t;

static void
out(s)
char *s;
{
	int n = 0;

	while (s[n])
		n++;
	auxwrite(1L, s, (long)n);
}

static char *
num(p, v)
char *p;
unsigned long v;
{
	char t[12];
	int n = 0;

	do
		t[n++] = '0' + v % 10;
	while ((v /= 10) != 0);
	while (n)
		*p++ = t[--n];
	return p;
}

static void
line(kind, name, what, v)
char *kind, *name, *what;
long v;
{
	char b[120], *p = b, *s;

	for (s = "mtcp "; *s; )
		*p++ = *s++;
	for (s = kind; *s; )
		*p++ = *s++;
	for (s = name; *s; )
		*p++ = *s++;
	if (what) {
		for (s = what; *s; )
			*p++ = *s++;
		if (v < 0) {
			*p++ = '-';
			v = -v;
		}
		p = num(p, (unsigned long)v);
	}
	*p++ = '\n';
	*p = 0;
	out(b);
}

static void
ip(name, a)
char *name;
unsigned long a;
{
	char b[80], *p = b, *s;
	int i;

	for (s = "mtcp I "; *s; )
		*p++ = *s++;
	for (s = name; *s; )
		*p++ = *s++;
	*p++ = ' ';
	for (i = 24; i >= 0; i -= 8) {
		p = num(p, (a >> i) & 0xff);
		*p++ = i ? '.' : '\n';
	}
	*p = 0;
	out(b);
}

static int
check(name, err)
char *name;
long err;
{
	line(err ? "F " : "P ", name, err ? ": error " : (char *)0, err);
	return err == 0;
}

static void
clear(pb, ref, code)
pb_t *pb;
short ref, code;
{
	char *c = (char *)pb;
	int i;

	for (i = 0; i < 12; i++)
		c[i] = 0;
	for (i = 12; i < (int)sizeof *pb; i++)
		if (i < 28 || i >= 32)
			c[i] = 0;
	pb->cref = ref;
	pb->code = code;
}

static void
test()
{
	pb_t pb;
	char name[6], msg[MSGLEN], got[MSGLEN];
	long wds[3], rbuf, n;
	short ref;
	int i;

	for (i = 0; i < (int)sizeof pb; i++)
		((char *)&pb)[i] = 0;
	name[0] = 4;
	name[1] = '.'; name[2] = 'I'; name[3] = 'P'; name[4] = 'P';
	pb.name = name;
	if (!check("open_ipp", trapopen(&pb)))
		return;
	ref = pb.cref;
	clear(&pb, ref, 15);			/* ipctlGetAddr */
	if (check("get_addr", trapctl(&pb))) {
		ip("our_address", (unsigned long)pb.stream);	/* csParam at 28 */
		ip("our_netmask", *(unsigned long *)&pb.p[0]);
	}
	if ((rbuf = newptr(16384L)) == 0) {
		check("rcv_buffer", -108L);
		return;
	}
	clear(&pb, ref, 30);			/* TCPCreate */
	pb.stream = 0;
	*(long *)&pb.p[0] = rbuf;
	*(long *)&pb.p[4] = 16384;
	if (!check("tcp_create", trapctl(&pb)))
		return;
	clear(&pb, ref, 32);			/* TCPActiveOpen */
	pb.p[3] = 30;				/* commandTimeoutValue */
	*(long *)&pb.p[4] = ECHOHOST;
	*(short *)&pb.p[8] = ECHOPORT;
	if (check("tcp_active_open", trapctl(&pb))) {
		ip("local_host", *(unsigned long *)&pb.p[10]);
		for (i = 0; i < MSGLEN; i++)
			msg[i] = 'A' + i;
		wds[0] = (long)MSGLEN << 16 | ((unsigned long)msg >> 16);
		wds[1] = (long)msg << 16;
		wds[2] = 0;
		clear(&pb, ref, 34);		/* TCPSend */
		pb.p[3] = 1;			/* pushFlag */
		*(long *)&pb.p[6] = (long)wds;
		check("tcp_send", trapctl(&pb));
		for (n = 0, i = 0; n < MSGLEN && i < 10; i++) {
			clear(&pb, ref, 37);	/* TCPRcv */
			pb.p[0] = 10;		/* commandTimeoutValue */
			*(long *)&pb.p[4] = (long)got + n;
			*(short *)&pb.p[8] = MSGLEN - n;
			if (!check("tcp_rcv", trapctl(&pb)))
				break;
			n += *(unsigned short *)&pb.p[8];
		}
		for (i = 0; i < n && got[i] == msg[i]; i++)
			;
		line(n == MSGLEN && i == MSGLEN ? "P " : "F ", "echo",
		    n == MSGLEN && i == MSGLEN ? (char *)0 : ": bytes ", n);
		clear(&pb, ref, 38);		/* TCPClose */
		check("tcp_close", trapctl(&pb));
	}
	clear(&pb, ref, 42);			/* TCPRelease */
	check("tcp_release", trapctl(&pb));
}

void
start()
{
	probe0("/mtcp-trap0", 0L);
	probe15("/mtcp-trap15");
	out("mtcp I started 1\n");
	test();
	out("mtcp done\n");
	for (;;)
		idle();
}
