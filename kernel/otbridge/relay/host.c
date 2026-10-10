#include "host.h"
#include <sys/types.h>
#include <sys/stropts.h>
#include <fcntl.h>
#include <errno.h>

#if defined(__m68k__)
#define OTR_NONBLOCK 0x4000
typedef char otr_attach_size[(sizeof(struct otx_attach) == 12) ? 1 : -1];
typedef char otr_strioctl_size[(sizeof(struct strioctl) == 16) ? 1 : -1];
#else
#define OTR_NONBLOCK O_NONBLOCK
#endif

#define OTR_AUX_WOULDBLOCK 55

extern long otr_sys_open(const char *, int);
extern long otr_sys_close(int);
extern long otr_sys_ioctl(int, int, long);
extern long otr_sys_read(int, unsigned char *, unsigned long);
extern long otr_sys_write(int, const unsigned char *, unsigned long);

long otr_host_step, otr_host_result;

static long
result(step, value)
	long step, value;
{
	otr_host_step = step; otr_host_result = value;
	return value;
}

static int
session(arg)
	void *arg;
{
	(void)arg;
	return result(1, otr_sys_open("/dev/otbridge", O_RDWR | OTR_NONBLOCK));
}
static long
session_id(arg, fd)
	void *arg;
	int fd;
{
	(void)arg;
	return result(2, otr_sys_ioctl(fd, OTB_GETID, 0L));
}
static int
endpoint(arg, protocol)
	void *arg;
	int protocol;
{
	(void)arg;
	return result(3, otr_sys_open(protocol == OTR_TCP ? "/dev/tcp" : "/dev/udp",
	    O_RDWR | OTR_NONBLOCK));
}
static int
push(arg, fd)
	void *arg;
	int fd;
{
	long r;
	(void)arg;
	r = result(4, otr_sys_ioctl(fd, I_PUSH, (long)"otxti"));
	if (r < 0) return r;
	return result(5, otr_sys_ioctl(fd, I_SRDOPT, (long)RMSGN));
}
static int
attach(arg, fd, id, cookie, protocol)
	void *arg;
	int fd, protocol;
	unsigned long id, cookie;
{
	struct otx_attach at;
	struct strioctl ioc;
	(void)arg;
	at.at_session = id; at.at_cookie = cookie; at.at_proto = protocol;
	ioc.ic_cmd = OTX_ATTACH; ioc.ic_timout = 0;
	ioc.ic_len = sizeof(at); ioc.ic_dp = (char *)&at;
	return result(6, otr_sys_ioctl(fd, I_STR, (long)&ioc));
}
void
otr_host_close(arg, fd)
	void *arg;
	int fd;
{
	(void)arg;
	(void)otr_sys_close(fd);
}
long
otr_host_read(arg, fd, bytes, length)
	void *arg;
	int fd;
	unsigned char *bytes;
	unsigned long length;
{
	long n;
	(void)arg;
	n = otr_sys_read(fd, bytes, length);
	if (n == -EAGAIN || n == -OTR_AUX_WOULDBLOCK || n == -EINTR) return 0;
	return n ? n : OTR_HOST_EOF;
}
long
otr_host_write(arg, fd, bytes, length)
	void *arg;
	int fd;
	const unsigned char *bytes;
	unsigned long length;
{
	long n;
	(void)arg;
	n = otr_sys_write(fd, bytes, length);
	return n == -EAGAIN || n == -OTR_AUX_WOULDBLOCK || n == -EINTR ? 0 : n;
}
const struct otr_boot_ops otr_host_boot = {
	session, session_id, endpoint, push, attach, otr_host_close
};

int
otr_host_flush(fd, flags)
	int fd, flags;
{
	return otr_sys_ioctl(fd, I_FLUSH, (long)flags);
}

extern long otr_sys_select(int, void *, void *, void *, void *);
int otr_host_wait(int fd, int events, unsigned long milliseconds)
{
    unsigned int mask[32];
    struct { long seconds, microseconds; } timeout;
    long n;int i;
    if (fd < 0 || fd >= 1024 || events < 1 || events > 3 || milliseconds > 1000)
        return -EINVAL;
    for (i = 0; i < 32; i++) mask[i] = 0;
    mask[fd / 32] = 1U << (fd % 32);
    timeout.seconds = milliseconds / 1000;
    timeout.microseconds = (milliseconds % 1000) * 1000;
    n = otr_sys_select(fd + 1, events & 1 ? mask : 0,
        events & 2 ? mask : 0, 0, &timeout);
    return n == -EINTR ? 0 : n;
}
