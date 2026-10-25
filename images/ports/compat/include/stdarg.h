/* stdarg.h for gcc with __STDC__ 0, where the system one uses a cc-only form */
#ifndef _STDARG_H
#define _STDARG_H
#ifndef _VA_LIST
#define _VA_LIST
typedef void *va_list;
#endif
#define	__va_size(t)	((sizeof (t) + sizeof (int) - 1) & ~(sizeof (int) - 1))
#define	va_start(ap, last)	((ap) = (va_list)__builtin_next_arg(last))
#define	va_arg(ap, t)	((ap) = (char *)(ap) + __va_size(t), \
	*(t *)((char *)(ap) - (sizeof (t) < sizeof (int) ? sizeof (t) : __va_size(t))))
#define	va_end(ap)	((void)0)
#endif
