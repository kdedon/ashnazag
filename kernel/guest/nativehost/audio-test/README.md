# Bounded host audio

`nh_audio_tone(frequency_hz, duration_ms)` accepts 20–10000 Hz and 1–2000 ms. It generates a modest-amplitude U8 square wave at 22050 samples/second through the public `/dev/sound` service. A successful return requires a drain reply and nonzero heard counters, with no muted or queued frames and the client bound to the front session. It returns `-1` with native `errno` for unavailable service, protocol errors, muted output, or timeout.

The operation forks a worker, leaving the caller's signal dispositions intact. The deadline is the requested duration plus 2000 ms, measured with host elapsed ticks. Timeout kills and reaps the worker; process cleanup closes its private sound channel. Use from a single-threaded process and outside signal handlers. The host C library must provide fork, waitpid, kill, poll, times, and STREAMS ioctl.

The interface is in `nativehost_audio.h`; compile `nativehost_audio.c` with `kernel/mac/sound` on the include path and link the host C library. It uses the public `sndio.h` FMT/PCM/DRAIN/STAT records and `I_NREAD` to reject a stale regular file at the device path. It accesses no chip registers.

Build the target fixture:

```sh
AMIX_TC=/path/to/toolchain/amix sh kernel/guest/nativehost/audio-test/build.sh /tmp/native-audio-clean
```

The output `/tmp/native-audio-clean/t_native_audio` requests 440 Hz for 250 ms. Stage it in an AMIX root with the shared host libc. The Quadra kernel needs the public ASC sound module and `/dev/asc` (major 46), plus STREAMS pipe/connld/fattach support. Register major 46 with `modadmin -r cdev asc 46` before loading the ASC module (`modadmin -l /path/to/asc`), or before its first device open with a configured module search path. The module basename must be `asc`. The generated `misc asc` registration does not register a character major. Start the public `sndd` service before the fixture; it owns `/dev/asc` and mounts `/dev/sound`. The fixture's user must be eligible for the front display session, and that session must retain sound ownership. `sndaux` and `/dev/snd` are unnecessary for this interface.

Run host tests:

```sh
sh kernel/guest/nativehost/audio-test/run.sh
```

Tests substitute the public sound stream calls and validate generated samples, frame count, partial I/O, EINTR, validation, missing/stale service, muted output, malformed replies, close errors, and worker timeout. They do not establish physical audibility. The build starts no emulator and creates no disk image.
