#ifndef LISA_SERVICES_H
#define LISA_SERVICES_H
int lisa_services_init(const char *, const char *);
int lisa_os_dispatch(unsigned int, unsigned char *, unsigned long [16]);
int lisa_services_close(void);
#endif
