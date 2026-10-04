/*
 * ikbd.c -- IKBD keyboard on the 6850 ACIA: scancodes to ASCII (US
 * layout, VT100 cursor keys) into a ring read by the console.
 * Mouse, joystick and clock packets are skipped, except that the display
 * service, once it starts, takes key bytes and relative mouse packets.
 */
#define VOL	__volatile__

#define ACIA_CTL	(*(VOL unsigned char *)0xFFFFFC00)
#define ACIA_DAT	(*(VOL unsigned char *)0xFFFFFC02)
#define ACIA_RDRF	0x01
#define ACIA_TDRE	0x02
#define ACIA_IRQ	0x80
#define MIDI_CTL	(*(VOL unsigned char *)0xFFFFFC04)

#define RINGSZ	64

#define K_LSHIFT	0x2A
#define K_RSHIFT	0x36
#define K_CTRL		0x1D
#define K_ALT		0x38
#define K_CAPS		0x3A

extern int ata_spltty();
extern void ata_splx(), atacons_wake(), ata_dsrec();
extern char *panicstr;

static unsigned char kb_ring[RINGSZ] = { 0 };
static int kb_put = 1, kb_get = 1;
static int kb_shift = 1, kb_ctrl = 1, kb_caps = 1;
static int kb_skip = 1;		/* bytes left of a non-key packet */
static int kb_ds = 1;		/* keys to the display service */
static int kb_mhdr = 1;		/* header of the mouse packet being read */
static unsigned char kb_mb[2] = { 1 };
unsigned long ikbd_nbytes = 1;

static char kb_lo[0x73] =
	"\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0\0\0 ";
static char kb_hi[0x73] =
	"\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0\0\0 ";
static char kb_pad[0x73 - 0x60] = "<\0\0()/*7894561230.\r";

static void
kb_in(c)
int c;
{
	register int n;

	n = (kb_put + 1) % RINGSZ;
	if (n == kb_get)
		return;
	kb_ring[kb_put] = c;
	kb_put = n;
}

static void
kb_str(s)
register char *s;
{
	while (*s)
		kb_in(*s++);
}

static void
kb_scan(b)
register int b;
{
	register int k, c;

	k = b & 0x7F;
	if (k == K_LSHIFT || k == K_RSHIFT) {
		kb_shift = !(b & 0x80);
		return;
	}
	if (k == K_CTRL) {
		kb_ctrl = !(b & 0x80);
		return;
	}
	if (b & 0x80)
		return;
	switch (k) {
	case K_CAPS:	kb_caps = !kb_caps; return;
	case 0x48:	kb_str("\033[A"); return;
	case 0x50:	kb_str("\033[B"); return;
	case 0x4D:	kb_str("\033[C"); return;
	case 0x4B:	kb_str("\033[D"); return;
	case 0x53:	kb_in(0x7F); return;
	case 0x4A:	kb_in('-'); return;
	case 0x4E:	kb_in('+'); return;
	}
	c = 0;
	if (k < 0x3A)
		c = kb_shift ? kb_hi[k] : kb_lo[k];
	else if (k >= 0x60 && k < 0x73)
		c = kb_pad[k - 0x60];
	if (c == 0)
		return;
	if (kb_caps && c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	if (kb_ctrl && (c & 0x40))
		c &= 0x1F;
	kb_in(c);
}

/* One byte from the IKBD: a key, or part of a status/mouse/clock packet. */
static void
kb_byte(b)
register int b;
{
	ikbd_nbytes++;
	if (kb_skip > 0) {
		kb_skip--;
		if (kb_mhdr) {
			kb_mb[1 - kb_skip] = b;
			if (kb_skip == 0) {
				ata_dsrec(kb_mhdr, kb_mb[0], kb_mb[1]);
				kb_mhdr = 0;
			}
		}
		return;
	}
	if (b >= 0xF6) {
		switch (b) {
		case 0xF6:	kb_skip = 7; break;	/* status */
		case 0xF7:	kb_skip = 5; break;	/* absolute mouse */
		case 0xFC:	kb_skip = 6; break;	/* time of day */
		case 0xFE: case 0xFF: kb_skip = 1; break; /* joystick */
		default:	/* relative mouse; 0xFD joystick report */
			kb_skip = 2;
			if (kb_ds && b <= 0xFB)
				kb_mhdr = b;
			break;
		}
		return;
	}
	/* after a panic getchar polls with the tick stopped */
	if (kb_ds && panicstr == 0)
		ata_dsrec(0, b, 0);
	else
		kb_scan(b);
}

static void
ikbd_send(c)
int c;
{
	register long n;

	for (n = 0; n < 100000; n++)
		if (ACIA_CTL & ACIA_TDRE)
			break;
	ACIA_DAT = c;
}

/* Reset the ACIA (divide by 64, 8N1, receive interrupt), quiet the mouse. */
void
ikbd_init()
{
	kb_put = kb_get = 0;
	kb_shift = kb_ctrl = kb_caps = kb_skip = 0;
	kb_ds = kb_mhdr = 0;
	ikbd_nbytes = 0;
	ACIA_CTL = 0x03;
	ACIA_CTL = 0x96;
	MIDI_CTL = 0x03;	/* MIDI shares GPIP4: reset, no interrupts */
	MIDI_CTL = 0x15;
	ikbd_send(0x12);	/* mouse off */
	ikbd_send(0x1A);	/* joystick off */
}

/* From the GPIP4 interrupt: drain the ACIA, so its line rises again. */
void
ikbd_intr()
{
	register int n;

	for (n = 0; n < 64 && (ACIA_CTL & ACIA_IRQ); n++) {
		if (ACIA_CTL & ACIA_RDRF)
			kb_byte(ACIA_DAT);
		else
			(void)ACIA_DAT;		/* overrun: clear */
	}
	if (kb_get != kb_put)
		atacons_wake();
}

/* Next key byte, -1 if none; polls the ACIA when interrupts are masked. */
int
ikbd_getc()
{
	register int c, s;

	s = ata_spltty();
	if (kb_get == kb_put && (ACIA_CTL & ACIA_RDRF))
		kb_byte(ACIA_DAT);
	if (kb_get == kb_put)
		c = -1;
	else {
		c = kb_ring[kb_get];
		kb_get = (kb_get + 1) % RINGSZ;
	}
	ata_splx(s);
	return c;
}

/* Route keys to the display service; mouse on in relative mode. */
void
ikbd_dsmode(keys, mouse)
int keys, mouse;
{
	kb_ds = keys;
	ikbd_send(mouse ? 0x08 : 0x12);
}

/* A scancode the display service hands back to the console. */
void
ikbd_cons(b)
int b;
{
	register int s;

	s = ata_spltty();
	kb_scan(b);
	ata_splx(s);
	if (kb_get != kb_put)
		atacons_wake();
}

/* Terminal answers from the renderer go where keys go. */
void
fbcons_input(c)
int c;
{
	register int s;

	s = ata_spltty();
	kb_in(c & 0xFF);
	ata_splx(s);
	atacons_wake();
}
