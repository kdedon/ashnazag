/*
 * amixEv.c -- keyboard and mouse from the event devices /dev/kbd and
 * /dev/mouse (16-byte inev records), bound to a frame-buffer session.
 *
 * Records are turned into the port's InputEvent form and handed to
 * amixKbdEnqueueEvent and amixMouseEnqueueEvent, so autorepeat and
 * pointer acceleration stay in one place.
 */

#define NEED_EVENTS
#include "amix.h"
#include <sys/ioctl.h>
#include "dsio.h"

extern KeySymsRec amixKeySyms, amixMacKeySyms;
extern CARD8 amixModMap[], amixMacModMap[];
extern void amixKbdEnqueueEvent(), amixMouseEnqueueEvent();
extern int lastEventTime;

#define ADB_OPTION	0x3A
#define ADB_ROPTION	0x7C
#define ADB_LEFT	0x3B
#define ADB_RIGHT	0x3C

static int kbdFd = -1, mouseFd = -1, noteFd = -1;
static int dx, dy;			/* motion since the last IE_SYN */
static struct timeval motionTv;
static int optionDown;
static int mb3Down[2];			/* arrow keys acting as buttons 2, 3 */
Bool amixEvMb3;				/* -mb3: arrows are buttons 2 and 3 */

static void
evKey(code, down, tv)
int code, down;
struct timeval *tv;
{
    struct InputEvent ie;

    ie.type = 1;
    ie.class = 0;
    ie.code = code | (down ? 0 : 0x80);
    ie.qualifiers = 0;
    ie.tv = *tv;
    amixKbdEnqueueEvent(LookupKeyboardDevice(), &ie);
}

static void
evButton(button, down, tv)
int button, down;
struct timeval *tv;
{
    struct InputEvent ie;

    ie.type = MOUSE_BUTTON_EVENT;
    ie.class = 0;
    ie.code = (0x7B + button) | (down ? 0 : 0x80);
    ie.qualifiers = 0;
    ie.tv = *tv;
    amixMouseEnqueueEvent(LookupPointerDevice(), &ie);
}

/* Motion in steps that fit the signed bytes of a move record */
static void
evMotion()
{
    struct InputEvent ie;
    int sx, sy;

    while (dx || dy)
    {
	sx = dx > 127 ? 127 : dx < -127 ? -127 : dx;
	sy = dy > 127 ? 127 : dy < -127 ? -127 : dy;
	dx -= sx;
	dy -= sy;
	ie.type = MOUSE_MOVE_EVENT;
	ie.class = 0;
	ie.code = ((sx & 0xFF) << 8) | (sy & 0xFF);
	ie.qualifiers = 0;
	ie.tv = motionTv;
	amixMouseEnqueueEvent(LookupPointerDevice(), &ie);
    }
}

static void
evKeyRecord(v, tv)
struct inev *v;
struct timeval *tv;
{
    int code = v->ie_code & 0x7F;
    int down = v->ie_value != 0;

    if (code == ADB_OPTION || code == ADB_ROPTION)
	optionDown = down;
    /* Option-arrow stays an arrow; so does the up of an arrow pressed so */
    if (amixEvMb3 && (code == ADB_LEFT || code == ADB_RIGHT))
    {
	int b = code == ADB_LEFT ? 0 : 1;

	if (down ? !optionDown : mb3Down[b])
	{
	    mb3Down[b] = down;
	    evButton(b + 2, down, tv);
	    return;
	}
    }
    evKey(code, down, tv);
}

/*
 * Event time.  Autorepeat and DIX compare it with gettimeofday(), which
 * has tick resolution, while records carry the fraction of the tick: a
 * record can be a few milliseconds "ahead" and would start autorepeat at
 * once.  Such times, and any over a second old, become the time of the
 * read.
 */
static void
evTime(v, tv)
struct inev *v;
struct timeval *tv;
{
    static int warned;
    struct timeval now;
    long d, ds;

    gettimeofday(&now, (struct timezone *)0);
    ds = now.tv_sec - v->ie_sec;
    if (ds > 2 || ds < -2)
	d = ds < 0 ? -1000000L : 1000000L;	/* far off; no overflow */
    else
	d = ds * 1000000L + (now.tv_usec - v->ie_usec);
    if (d >= 0 && d < 1000000L && (unsigned long)v->ie_usec < 1000000UL)
    {
	tv->tv_sec = v->ie_sec;
	tv->tv_usec = v->ie_usec;
	return;
    }
    if (d < -20000L || d >= 1000000L)
	if (!warned++)
	    ErrorF("amixEv: event time %ld.%06ld, gettimeofday %ld.%06ld\n",
		   v->ie_sec, v->ie_usec, (long)now.tv_sec, (long)now.tv_usec);
    *tv = now;
}

static void
evRead(fd)
int fd;
{
    struct inev ev[32];
    struct timeval tv;
    int n, i;

    while ((n = read(fd, (char *)ev, sizeof ev)) > 0)
    {
	n /= sizeof ev[0];
	for (i = 0; i < n; i++)
	{
	    evTime(&ev[i], &tv);
	    lastEventTime = TVTOMILLI(tv);
	    switch (ev[i].ie_type)
	    {
	    case IE_KEY:
		evKeyRecord(&ev[i], &tv);
		break;
	    case IE_BTN:
		evMotion();
		evButton((int)ev[i].ie_code, ev[i].ie_value != 0, &tv);
		break;
	    case IE_REL:
		if (ev[i].ie_code == IE_RELX)
		    dx += ev[i].ie_value;
		else if (ev[i].ie_code == IE_RELY)
		    dy += ev[i].ie_value;
		motionTv = tv;
		break;
	    case IE_SYN:
		evMotion();
		break;
	    case IE_DROP:
		ErrorF("amixEv: input queue overflow\n");
		break;
	    }
	}
	if (n < sizeof ev / sizeof ev[0])
	    return;
    }
    /* a dead device would stay readable and spin the server */
    if (n < 0 && (errno == EIO || errno == ENXIO || errno == ENODEV ||
		  errno == EBADF))
    {
	ErrorF("amixEv: input read: %s; device dropped\n", strerror(errno));
	RemoveEnabledDevice(fd);
	if (fd == kbdFd)
	    kbdFd = -1;
	else if (fd == mouseFd)
	    mouseFd = -1;
    }
}

/* Session notes: the kernel restores VRAM and CLUT itself */
static void
evNotes()
{
    struct fbnote n;

    while (read(noteFd, (char *)&n, sizeof n) == sizeof n)
	if (n.fn_type == FBN_MODE)
	    ErrorF("amixEv: display mode changed; the screen keeps its size\n");
	else if (n.fn_type == FBN_HIDDEN || n.fn_type == FBN_SHOWN)
	{
	    optionDown = 0;
	    mb3Down[0] = mb3Down[1] = 0;
	}
}

static void
evEnable()
{
    if (kbdFd >= 0)
	AddEnabledDevice(kbdFd);
    if (mouseFd >= 0)
	AddEnabledDevice(mouseFd);
    if (noteFd >= 0)
	AddEnabledDevice(noteFd);
}

static void
evDisable()
{
    if (kbdFd >= 0)
	RemoveEnabledDevice(kbdFd);
    if (mouseFd >= 0)
	RemoveEnabledDevice(mouseFd);
    if (noteFd >= 0)
	RemoveEnabledDevice(noteFd);
}

static void
evReadAll(pReadmask)
pointer pReadmask;
{
    fd_set *m = (fd_set *)pReadmask;

    if (kbdFd >= 0 && FD_ISSET(kbdFd, m))
	evRead(kbdFd);
    if (mouseFd >= 0 && FD_ISSET(mouseFd, m))
	evRead(mouseFd);
    if (noteFd >= 0 && FD_ISSET(noteFd, m))
	evNotes();
}

static amixInputOpsRec evOps = { evEnable, evDisable, evReadAll };

static int
evOpen(name, fbfd)
char *name;
int fbfd;
{
    int fd = open(name, O_RDONLY | O_NDELAY);

    if (fd < 0)
    {
	ErrorF("amixEv: %s: %s\n", name, strerror(errno));
	return -1;
    }
    if (ioctl(fd, EVIOCBIND, fbfd) < 0)
    {
	ErrorF("amixEv: EVIOCBIND %s: %s\n", name, strerror(errno));
	close(fd);
	return -1;
    }
    return fd;
}

/*
 * Open the event devices for the session on fbfd.  Returns FALSE when
 * neither opens; the server then runs without input.
 */
Bool
amixEvOpen(fbfd)
int fbfd;
{
    struct evinfo ei;

    if (kbdFd >= 0 || mouseFd >= 0)
	return TRUE;
    kbdFd = evOpen("/dev/kbd", fbfd);
    mouseFd = evOpen("/dev/mouse", fbfd);
    if (kbdFd < 0 && mouseFd < 0)
	return FALSE;
    noteFd = fbfd;
    (void) fcntl(fbfd, F_SETFL, fcntl(fbfd, F_GETFL, 0) | O_NDELAY);

    if (kbdFd >= 0 && ioctl(kbdFd, EVIOCGINFO, &ei) == 0 && ei.ei_kset == EVK_AMIGA)
    {
	amixKeySymsPtr = &amixKeySyms;
	amixModMapPtr = amixModMap;
	amixKeyOffset = 1;
    }
    else
    {
	amixKeySymsPtr = &amixMacKeySyms;
	amixModMapPtr = amixMacModMap;
	amixKeyOffset = 8;
    }
    amixInput = &evOps;
    return TRUE;
}

void
amixEvClose()
{
    if (kbdFd >= 0)
	close(kbdFd);
    if (mouseFd >= 0)
	close(mouseFd);
    kbdFd = mouseFd = noteFd = -1;
    amixInput = (amixInputOpsPtr) 0;
}
