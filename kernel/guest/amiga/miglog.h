#ifndef MIG_LOG_H
#define MIG_LOG_H
extern int miglog_fd;
void miglog_start(int);
long miglog_ms(void);
void miglog(int, const char *, ...);
#endif
