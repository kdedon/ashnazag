#ifndef _STDDEF_H
#define _STDDEF_H

typedef int	ptrdiff_t;
#ifndef _SIZE_T
#define _SIZE_T
typedef unsigned int	size_t;
#endif
#ifndef _WCHAR_T
#define _WCHAR_T
typedef long	wchar_t;
#endif
#ifndef NULL
#define NULL	0
#endif
#define	offsetof(s, m)	((size_t)&(((s *)0)->m))

#endif
