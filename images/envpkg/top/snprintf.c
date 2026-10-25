/* snprintf, which this libc lacks; top's strings are a screen line or less */
#include <stdio.h>
#include <string.h>
#include <varargs.h>

int
snprintf(va_alist)
	va_dcl
{
	static char b[2048];
	va_list ap;
	char *s, *f;
	int n, l;

	va_start(ap);
	s = va_arg(ap, char *);
	n = va_arg(ap, int);
	f = va_arg(ap, char *);
	l = vsprintf(b, f, ap);
	va_end(ap);
	if (n > 0) {
		strncpy(s, b, n - 1);
		s[n - 1] = '\0';
	}
	return l;
}
