/*
 * sprintf with the %lld, %llu, %llo, %llx and %llX that libc lacks.  Vim
 * formats each 64-bit number on its own, so only a bare "%ll?" is done
 * here.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int
llsprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	unsigned long long v;
	const char *dig = fmt[3] == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
	char t[24], *p = buf;
	int base, k = 0, n;

	va_start(ap, fmt);
	if (strncmp(fmt, "%ll", 3) != 0 || fmt[3] == '\0' || fmt[4] != '\0' ||
	    strchr("duoxX", fmt[3]) == NULL) {
		n = vsprintf(buf, fmt, ap);
		va_end(ap);
		return n;
	}
	v = va_arg(ap, unsigned long long);
	va_end(ap);
	if (fmt[3] == 'd' && (long long)v < 0) {
		*p++ = '-';
		v = -v;
	}
	base = fmt[3] == 'o' ? 8 : fmt[3] == 'x' || fmt[3] == 'X' ? 16 : 10;
	do
		t[k++] = dig[v % base];
	while ((v /= base) != 0);
	while (k > 0)
		*p++ = t[--k];
	*p = '\0';
	return p - buf;
}
