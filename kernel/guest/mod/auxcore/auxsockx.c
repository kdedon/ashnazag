/*
 * auxsockx.c -- A/UX socket numbering: types and ioctl commands.
 * Pure; also built on the host for tests.
 *
 * K&R C.
 */

/* A/UX STREAM, DGRAM, RAW, RDM, SEQPACKET (1..5) -> SVR4 */
static char tin[] = { 0, 2, 1, 4, 5, 6 };

int
aux_socktype_in(t)
	long t;
{
	return t > 0 && t < sizeof tin ? tin[t] : -1;
}

int
aux_socktype_out(t)
	long t;
{
	int i;

	for (i = 1; i < sizeof tin; i++)
		if (tin[i] == t)
			return i;
	return -1;
}

/* socket ioctls: A/UX command, SVR4 command */
static unsigned long sockioc[][2] = {
	{ 0x8004667e, 0x8004667e },	/* FIONBIO */
	{ 0x8004667d, 0x8004667d },	/* FIOASYNC */
	{ 0x4004667f, 0x4004667f },	/* FIONREAD */
	{ 0x80047308, 0x80047308 },	/* SIOCSPGRP */
	{ 0x40047309, 0x40047309 },	/* SIOCGPGRP */
	{ 0x8004667c, 0x80047308 },	/* FIOSETOWN */
	{ 0x4004667b, 0x40047309 },	/* FIOGETOWN */
	{ 0xc0086914, 0xc0086914 },	/* SIOCGIFCONF */
	{ 0xc020690d, 0xc020690d },	/* SIOCGIFADDR */
	{ 0xc020690f, 0xc020690f },	/* SIOCGIFDSTADDR */
	{ 0xc0206911, 0xc0206911 },	/* SIOCGIFFLAGS */
	{ 0xc0206917, 0xc0206919 },	/* SIOCGIFNETMASK */
	{ 0xc0206919, 0xc020691b },	/* SIOCGIFMETRIC */
	{ 0xc020691b, 0xc0206917 },	/* SIOCGIFBRDADDR */
	{ 0, 0 }
};

unsigned long
aux_sockioc(cmd)
	unsigned long cmd;
{
	int i;

	for (i = 0; sockioc[i][0]; i++)
		if (sockioc[i][0] == cmd)
			return sockioc[i][1];
	return 0;
}
