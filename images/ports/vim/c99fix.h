/* C99 math and BSD calls the system headers lack. */
#define isinf(x)	__builtin_isinf(x)
#define isnan(x)	__builtin_isnan(x)
struct rlimit;
extern int getrlimit(int, struct rlimit *);
extern int setrlimit(int, const struct rlimit *);
