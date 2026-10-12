#ifndef LISA_AUDIO_H
#define LISA_AUDIO_H
#include "lisa_host.h"
int lisa_audio_start(struct lisa_host *, unsigned long, unsigned long,
	unsigned long);
int lisa_audio_stop(struct lisa_host *);
#endif
