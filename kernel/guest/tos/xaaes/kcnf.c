/*
 * kcnf.c -- the configuration file reader modules call: lines of
 * KEYWORD [=] arguments, '#' comments; each item's type says how its
 * arguments read (number, bool, token, rest of line) and whether a
 * callback gets them or a variable takes them.
 */
#include "xamint.h"

#define	LINE	512

union arg {
	long	l;
	short	s;
	bool	b;
	char	*c;
};

void _cdecl
parser_msg(struct parsinf *inf, const char *msg)
{
	if (inf && msg)
		xlog("%s:%d: %s", inf->file ? inf->file : "?", inf->line, msg);
}

static void
spaces(struct parsinf *inf)
{
	while (*inf->src && *inf->src != '\n' && isspace(*inf->src))
		inf->src++;
}

/* a token: quotes group, backslash escapes inside "", ends at space */
static int
token(struct parsinf *inf, char *dst, int upcase)
{
	char *s = inf->src, *d = dst, q = 0, c;

	while ((c = *s) != 0 && c != '\n') {
		s++;
		if ((c == '"' || c == '\'') && (!q || q == c)) {
			q = q ? 0 : c;
			continue;
		}
		if (c == '\\' && q == '"' && *s && *s != '\n') {
			c = *s++;
			if (c == 't')
				c = '\t';
			else if (c == 'n')
				c = '\n';
			else if (c == 'r')
				c = '\r';
			else if (c == 'e')
				c = 27;
		} else if (!q && isspace(c)) {
			s--;
			break;
		}
		if (d < dst + LINE - 1)
			*d++ = upcase ? toupper(c) : c;
	}
	*d = 0;
	if (d > dst && d[-1] == '\r')
		d[-1] = 0;
	if (s == inf->src || q)
		return -1;
	inf->src = s;
	return 0;
}

static int
line(struct parsinf *inf, char *dst)
{
	char *d = dst;

	while (*inf->src && *inf->src != '\n') {
		if (d < dst + LINE - 1)
			*d++ = *inf->src;
		inf->src++;
	}
	while (d > dst && isspace(d[-1]))
		d--;
	*d = 0;
	return 0;
}

static int
number(struct parsinf *inf, long *v, const struct range *r)
{
	char *s = inf->src;
	long n = 0;
	int neg = 0;

	if (*s == '-') {
		neg = 1;
		s++;
	}
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		while (isxdigit(*s)) {
			n = n * 16 + (isdigit(*s) ? *s - '0' : (toupper(*s) - 'A' + 10));
			s++;
		}
	} else
		while (isdigit(*s))
			n = n * 10 + *s++ - '0';
	if (s == inf->src)
		return -1;
	if (toupper(*s) == 'K') {
		n *= 1024;
		s++;
	} else if (toupper(*s) == 'M') {
		n *= 1024L * 1024;
		s++;
	}
	if (*s && !isspace(*s))
		return -1;
	if (neg)
		n = -n;
	if (r && (n < r->a || n > r->b))
		return -1;
	inf->src = s;
	*v = n;
	return 0;
}

static void
parse(char *text, struct parsinf *inf, struct parser_item *tab)
{
	static const char *const bools[] = { "00", "11", "0N", "1Y", "0NO", "1YES", "0OFF", "1ON",
		"0FALSE", "1TRUE", 0 };
	char key[64], buf[2][LINE];
	struct parser_item *it;
	union arg a[2];
	int n, t, i;

	inf->src = text;
	while (*inf->src) {
		while (*inf->src && (isspace(*inf->src) || *inf->src == '#')) {
			if (*inf->src == '#')
				while (*inf->src && *inf->src != '\n')
					inf->src++;
			if (*inf->src == '\n')
				inf->line++;
			if (*inf->src)
				inf->src++;
		}
		if (!*inf->src)
			break;
		for (i = 0; *inf->src && *inf->src != '=' && !isspace(*inf->src); inf->src++)
			if (i < (int)sizeof key - 1)
				key[i++] = toupper(*inf->src);
		key[i] = 0;
		for (it = tab; it->key && strcmp(it->key, key); it++)
			;
		spaces(inf);
		if (*inf->src == '=')
			inf->src++;
		if (!it->key || !it->cb) {
			parser_msg(inf, "unknown keyword");
			goto skip;
		}
		t = it->type & 0xff;
		for (n = 0; n < 2 && (t & 0x0f); n++, t >>= 4) {
			const struct range *r = it->dat.dat ? &it->dat.range : 0;
			long v;

			spaces(inf);
			switch (t & 0x0f) {
			case PI_C_S: case PI_C_L: case PI_C_US:
				if (number(inf, &v, (t & 0x0f) == PI_C_US ? 0 : r)) {
					parser_msg(inf, "bad number");
					goto skip;
				}
				a[n].l = v;
				a[n].s = v;
				if ((t & 0x0f) == PI_C_L)
					a[n].l = v;
				break;
			case PI_C_B:
				if (token(inf, buf[n], 1))
					goto skip;
				for (i = 0; bools[i] && strcmp(bools[i] + 1, buf[n]); i++)
					;
				if (!bools[i]) {
					parser_msg(inf, "not a bool");
					goto skip;
				}
				a[n].b = bools[i][0] - '0';
				break;
			case PI_C_T:
				if (token(inf, buf[n], 0))
					goto skip;
				a[n].c = buf[n];
				break;
			case PI_C_A:
				line(inf, buf[n]);
				a[n].c = buf[n];
				break;
			default:
				goto skip;
			}
		}
		switch (it->type & 0xf7ff) {
		case PI_C_L: case PI_V_L:
			((PCB_Lx *)it->cb)(a[0].l, inf);
			break;
		case PI_C_S:
			((PCB_Lx *)it->cb)(a[0].s, inf);
			break;
		case PI_C_B: case PI_V_B:
			((PCB_Bx *)it->cb)(a[0].b, inf);
			break;
		case PI_C_T: case PI_V_T: case PI_C_A: case PI_V_A:
			((PCB_Tx *)it->cb)(a[0].c, inf);
			break;
		case PI_C_TT: case PI_C_TA:
			((PCB_TTx *)it->cb)(a[0].c, a[1].c, inf);
			break;
		case PI_C_0TT:
			((PCB_0TT *)it->cb)(0, a[0].c, a[1].c);
			break;
		case PI_V_ATK:
			((PCB_ATK *)it->cb)(a[0].c, a[1].c, it->dat.dat);
			break;
		case PI_R_S:
			*(short *)it->cb = a[0].s;
			break;
		case PI_R_US:
			*(unsigned short *)it->cb = a[0].s;
			break;
		case PI_R_L:
			*(long *)it->cb = a[0].l;
			break;
		case PI_R_B:
			*(bool *)it->cb = a[0].b;
			break;
		case PI_R_T:
			if ((long)strlen(a[0].c) < it->dat.dat)
				strcpy(it->cb, a[0].c);
			break;
		case PI_C__:
			((void (*)(struct parsinf *))it->cb)(inf);
			break;
		default:
			parser_msg(inf, "unknown item type");
		}
skip:
		while (*inf->src && *inf->src != '\n')
			inf->src++;
	}
}

static long
parse_file(const char *path, struct parsinf *inf, struct parser_item *tab)
{
	struct file *f;
	XATTR x;
	char *text;
	long r;

	f = kernel_open(path, O_RDONLY, &r, &x);
	if (!f)
		return r;
	text = kmalloc_o(x.size + 1, 0);
	if (!text) {
		kernel_close(f);
		return ENOMEM;
	}
	r = kernel_read(f, text, x.size);
	kernel_close(f);
	text[r > 0 ? r : 0] = 0;
	inf->file = path;
	inf->line = 1;
	parse(text, inf, tab);
	kfree_any(text);
	return 0;
}

long _cdecl
parse_include(const char *path, struct parsinf *inf, struct parser_item *tab)
{
	struct parsinf in = *inf;

	return parse_file(path, &in, tab);
}

long _cdecl
parse_cnf(const char *path, struct parser_item *tab, void *data, unsigned long options)
{
	struct parsinf inf;
	long r;

	bzero(&inf, sizeof inf);
	inf.opt = options;
	inf.data = data;
	r = parse_file(path, &inf, tab);
	xlog("config %s: %ld", path, r);
	return r;
}
