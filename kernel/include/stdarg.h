#ifndef _STDARG_H
#define _STDARG_H

/* Arguments sit in ascending int slots; a short one at its slot's end. */
typedef char *va_list;
#define	__va_size(t)	((sizeof (t) + sizeof (int) - 1) & ~(sizeof (int) - 1))
#define	va_start(ap, last)	((ap) = (va_list)__builtin_next_arg(last))
#define	va_arg(ap, t)	((ap) += __va_size(t), \
	*(t *)((ap) - (sizeof (t) < sizeof (int) ? sizeof (t) : __va_size(t))))
#define	va_end(ap)	((void)0)

#endif
