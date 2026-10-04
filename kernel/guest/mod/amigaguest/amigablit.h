#ifndef AMIGABLIT_H
#define AMIGABLIT_H

int amigablit_run(unsigned short *, int, unsigned long,
    int (*)(void *, unsigned long, unsigned short *),
    int (*)(void *, unsigned long, unsigned short), void *, int *);

#endif
