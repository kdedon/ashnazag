# Voyage 200 native adapter

This adapter runs the admitted original image as native m68k user instructions. The image supplies its calculator UI, arithmetic, parser, graph and error handling. The loader supplies AMIX memory, display, keyboard and timing services.

The reviewed profile admits exactly 4 MiB with SHA256 `acf4db4ca0b3d9551facd6fad1dc0c6cbdd5b39fb552af18c3890cf9160f365a`. Admission copies its bytes into private memory, checks image/page CRCs and the declared status-save words, converts only `0x221102` and `0x22115c` from `0x40c1` to `0x42c1`, verifies both resulting CRCs and restores read/execute protection. The source file stays unchanged. The read-only first-page copy at address zero is provisional compatibility data for this exact profile; its original storage semantics remain unresolved.

Build against the installed AMIX toolchain, kernel and matching guestcore module:

```sh
AMIX_TC=/path/to/toolchain/amix KERNEL=/path/to/unix-mac.elf \
GUESTCORE=/path/to/guestcore sh kernel/guest/voyage200/build.sh OUTDIR
```

The output includes the normal `mod.d/tiplatform` and `mod_register.d/tiplatform` layout. Optional second argument `DESTDIR` stages the module under `/etc/conf/mod.d` and executable under `/usr/lib/voyage200`. This builds no OS image and supplies no media. The existing native launchers remain separate host dependencies.

The `v200env` package installs `startvoyage200`, `/usr/lib/voyage200/native`, `tiplatform`, its registration (S05v200, major 65) and `/dev/tiplatform`. Grant the intended user access to that device and the normal native display service. The application performs no module registration and needs an 8-bit framebuffer of at least 760×492 pixels.

Install `OUTDIR/native` and the admitted read-only image as `~/Voyage200/default/native` and `~/Voyage200/default/media`. Run the existing `startvoyage200` launcher to obtain the user's environment lock. The executable reads `NATIVEHOST_MEDIA`; an explicit first argument overrides the image path for bounded tests. Ctrl-Q requests exit and queues original ESC to release modal waits. Ctrl-I inserts the original integral glyph; Ctrl-L emits original Clear. Remaining key mappings follow the reviewed ADB layout.

Original lifecycle/event calls use the owned low stack. Asynchronous keyboard queue calls preserve the current native signal stack; the idle continuation preserves all registers and CCR on the existing original stack. Per-process service masking postpones SIGALRM while preserving the native signal mask. Detach and exit cancel the owned timer; a forked child inherits no timer.

Validation requires real native input and the original bitmap: Home `12+34`, integral glyph workflows, original error/ESC/Clear recovery, decimal `approx(1/3)`, Graph `x^2`, F3 trace and Ctrl-Q followed by relaunch. Cross-compilation and hash checks alone do not establish calculator compatibility. Persistence and other applications remain outside this profile.

Independent AMIX verification on 2026-10-12 ran the clean adapter as UID 103, displayed original Home `12+34=46`, and exited through Ctrl-Q with the environment lock released: two checks passed, zero failures/skips, 7783 ms. `GROUP=v200 sh tests/run-qemu.sh` checks the hashes and the module. A separate ordinary-user run passed two symbolic/error/Clear checks in 19700 ms, displaying original `x^3/3` and `-cos(x)` bitmaps. The final artifact pair passed two Graph/numeric/Trace checks in 17566 ms, showing original `approx(1/3)=.333333`, the `x^2` parabola and trace coordinates `x=.252101`, `y=.063555`.
