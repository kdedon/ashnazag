/* Transport interface module ioctls. */
#ifndef _SYS_TIMOD_H
#define _SYS_TIMOD_H

#define TIMOD		('T'<<8)
#define TI_GETINFO	(TIMOD|140)
#define TI_OPTMGMT	(TIMOD|141)
#define TI_BIND		(TIMOD|142)
#define TI_UNBIND	(TIMOD|143)
#define TI_GETMYNAME	(TIMOD|144)
#define TI_GETPEERNAME	(TIMOD|145)
#define TI_SETMYNAME	(TIMOD|146)
#define TI_SETPEERNAME	(TIMOD|147)

#endif
