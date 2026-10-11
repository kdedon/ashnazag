#include <stdio.h>
#include <errno.h>
#include "nativehost_audio.h"

int
main(void)
{
    if (nh_audio_tone(440, 250) < 0) {
        perror("native-audio: 440 Hz 250 ms");
        return 1;
    }
    puts("native-audio: service completed unmuted 440 Hz 250 ms");
    return 0;
}
