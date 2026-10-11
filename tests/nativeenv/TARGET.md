# Native launcher target fixture

Build serially with `AMIX_TC=/path/to/toolchain/amix sh tests/nativeenv/build-target.sh OUTDIR`.
Append `OUTDIR/fixture.manifest` to a private AMIX test root containing libc,
`/tmp`, and the usual console/init tools. Run `/tests/t_natlaunch`.
The three fixture policy files belong only in that private root.

The synthetic payload verifies caller arguments, working directory, all
`NATIVEHOST_*` values, malloc storage, and the session lock inherited across
actual exec. The runner checks all three family roots, competing launches,
family isolation, named roots, failed exec, lock release, and retry.
It prints `TESTS DONE pass=20 fail=0 skip=0` on success and uses a 45-second alarm.
The fixture creates and removes its own `/tmp/natlaunch` directory;
it refuses to start if that directory already exists.
