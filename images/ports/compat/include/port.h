/* Declarations every port gets: libport's and libc's undeclared ones. */
#include <amixcompat.h>

#include <sys/types.h>

extern long random(void);
extern void srandom(unsigned);
extern long nap(long);
extern int fchmod(int, mode_t);
extern int grantpt(int);
extern int unlockpt(int);
extern char *ptsname(int);
extern int select();
extern int ftruncate(int, off_t);
struct utimbuf;
extern int utime(const char *, const struct utimbuf *);
extern int tgetent();
extern int tgetflag();
extern int tgetnum();
extern char *tgetstr();
extern char *tgoto();
extern int tputs();
