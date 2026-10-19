/* Transport provider interface primitives. */
#ifndef _SYS_TIHDR_H
#define _SYS_TIHDR_H

/* user to provider */
#define T_CONN_REQ	0
#define T_CONN_RES	1
#define T_DISCON_REQ	2
#define T_DATA_REQ	3
#define T_EXDATA_REQ	4
#define T_INFO_REQ	5
#define T_BIND_REQ	6
#define T_UNBIND_REQ	7
#define T_UNITDATA_REQ	8
#define T_OPTMGMT_REQ	9
#define T_ORDREL_REQ	10

/* provider to user */
#define T_CONN_IND	11
#define T_CONN_CON	12
#define T_DISCON_IND	13
#define T_DATA_IND	14
#define T_EXDATA_IND	15
#define T_INFO_ACK	16
#define T_BIND_ACK	17
#define T_ERROR_ACK	18
#define T_OK_ACK	19
#define T_UNITDATA_IND	20
#define T_UDERROR_IND	21
#define T_OPTMGMT_ACK	22
#define T_ORDREL_IND	23

#endif
