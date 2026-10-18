/* Ethernet driver status and configuration ioctls. */
#ifndef _AMIGA_AENUSER_H
#define _AMIGA_AENUSER_H

#define AENIOC			('A'<<8)
#define AEN_CLEAR_STATUS	(AENIOC|1)
#define AEN_GET_STATUS		(AENIOC|2)
#define AEN_GET_CONFIG		(AENIOC|3)
#define AEN_SET_CONFIG		(AENIOC|4)
#define AEN_NUMBER_OF_BOARDS	(AENIOC|5)

/* mode */
#define AEN_MODE_PROM		0x8000

/* flags */
#define AEN_RAW_DATA		1

enum BOARD_STATE { BOARD_RESET, BOARD_IN_INIT_CODE, BOARD_RUNNING };

typedef struct {
	enum BOARD_STATE	board_state;
	unsigned int		packets_sent;
	unsigned int		packets_received;
	unsigned int		allocbs_failed;
	unsigned int		couldnt_put;
	unsigned int		memory_error;
	unsigned int		miss_error;
	unsigned int		collision_error;
	unsigned int		babble_error;
	unsigned int		trailers;
	unsigned int		bad_start;
	unsigned int		buffer_error;
	unsigned int		overflow;
	unsigned int		framming;
	unsigned int		crc;
	unsigned int		board_debug;
	unsigned int		late_collisions;
	unsigned int		loss_of_carrier;
	unsigned int		retry_errors;
} aen_status_t;

typedef struct {
	long		board_base;
	int		board_debug;
	unsigned char	paddress[6];	/* physical */
	unsigned char	laddress[8];	/* logical filter */
	int		mode;
	int		flags;
} aen_config_t;

#endif
