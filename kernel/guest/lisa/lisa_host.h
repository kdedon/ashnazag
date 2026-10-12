#ifndef LISA_HOST_H
#define LISA_HOST_H

#include "nativehost.h"
#include "nativehost_ui.h"

struct lisa_host {
	struct nh_display display;
	struct nh_surface surface;
	struct evinfo keyboard_info;
	unsigned long white, black;
	unsigned long view_x, view_y, view_width, view_height;
	unsigned char *snapshot;
	unsigned long snapshot_size;
	int input_turn;
	pid_t audio_pid;
	int audio_fd;
	unsigned long audio_generation, audio_started, audio_budget;
};

void lisa_host_init(struct lisa_host *);
int lisa_host_open(struct lisa_host *, const char *, int);
int lisa_host_close(struct lisa_host *);
int lisa_host_present(struct lisa_host *, const unsigned char *,
	unsigned long, unsigned long, unsigned long);
int lisa_host_input(struct lisa_host *, struct inev *);
int lisa_host_tone(struct lisa_host *, unsigned int, unsigned int);
int lisa_host_audio_poll(struct lisa_host *);
int lisa_host_publish(struct lisa_host *, unsigned char *);
int lisa_host_render(struct lisa_host *, const unsigned char *);
int lisa_host_audio(struct lisa_host *, unsigned char *);

#endif
