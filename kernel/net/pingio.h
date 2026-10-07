/*
 * pingio.h -- the echo service: ICMP echo for programs without raw
 * sockets.  PINGPATH is a pipe with connld, so each open is a private
 * stream.  A client writes whole struct pingreq records and reads whole
 * struct pingrep records: the echo replies to its own requests.
 */
#ifndef PINGIO_H
#define PINGIO_H

#define PINGPATH	"/dev/ping"
#define PING_DATA	512		/* echo data after identifier and sequence */

struct pingreq {
	unsigned long	pq_dst;		/* IPv4 address */
	unsigned short	pq_id, pq_seq;	/* returned in the reply */
	unsigned short	pq_len;		/* bytes of pq_data */
	unsigned short	pq_pad;
	char		pq_data[PING_DATA];
};

struct pingrep {
	unsigned long	pr_src;		/* who answered */
	unsigned short	pr_id, pr_seq;
	unsigned short	pr_len;
	unsigned char	pr_ttl, pr_pad;
	char		pr_data[PING_DATA];
};

#endif
