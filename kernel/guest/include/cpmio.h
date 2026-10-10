#ifndef CPMIO_H
#define CPMIO_H
#include "guest_lone.h"
#define CPM_MAJOR 59
#define CPMIOC(n) (('C' << 8) | (n))
#define CPMIOC_ENTER CPMIOC(1)
#define CPMIOC_LEAVE CPMIOC(2)
#define CPMIOC_STAT CPMIOC(3)
#endif
