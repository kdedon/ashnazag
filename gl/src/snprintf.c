/*
 * vsnprintf and snprintf, which AMIX's libc lacks.  The length is measured
 * on /dev/null first: vsprintf writes straight into the buffer only when
 * the result fits.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static FILE *null;

int
vsnprintf(char *str, size_t n, const char *fmt, va_list ap)
{
	char *buf;
	int len;

	if (null == NULL && (null = fopen("/dev/null", "w")) == NULL)
		return -1;
	len = vfprintf(null, fmt, ap);
	if (len < 0)
		return len;
	if ((size_t)len < n)
		return vsprintf(str, fmt, ap);
	if (n == 0)
		return len;
	if ((buf = malloc(len + 1)) == NULL)
		return -1;
	vsprintf(buf, fmt, ap);
	memcpy(str, buf, n - 1);
	str[n - 1] = '\0';
	free(buf);
	return len;
}

int
snprintf(char *str, size_t n, const char *fmt, ...)
{
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(str, n, fmt, ap);
	va_end(ap);
	return len;
}
