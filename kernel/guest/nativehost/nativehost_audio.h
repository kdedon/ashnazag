#ifndef NATIVEHOST_AUDIO_H
#define NATIVEHOST_AUDIO_H

/* 20..10000 Hz, 1..2000 ms; zero only after unmuted service completion. */
int nh_audio_tone(unsigned int frequency_hz, unsigned int duration_ms);

#endif
