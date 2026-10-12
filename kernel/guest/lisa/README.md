# Lisa native Office adapter

The optional adapter runs supplied 68k Office code directly on AMIX. The
frontend owns a display session, keyboard, mouse and audio workers. Its child
owns the original execution context and real files, memory, clocks and queues.
The `lplatform` module binds major 64 and admits the reviewed service sites and
compiler extensions. Native host calls run through the public guest scopes.

The `lisaenv` package (images/envpkg/mkenvpkgs.sh) installs `startlisa`,
`/usr/lib/lisa/native` (`sh kernel/guest/lisa/build.sh OUTDIR`), the module
(guest/build.sh -m), its registration (S05lisa) and `/dev/lplatform`. Display access follows
the host display policy. `startlisa` supplies `NATIVEHOST_ROOT` and
`NATIVEHOST_MEDIA`; the native executable belongs in the selected environment.
`NATIVEHOST_DISPLAY` selects a framebuffer, defaulting to `/dev/fb0`.

An environment needs the exact supplied containers under `segments/`, the
original `resources/41.raw`, immutable resources, and imported writable state.
The loader verifies the media, containers and immutable resource hashes. It
reconstructs stack/shared initialization from the unchanged original jump
table and reviewed scalar data pointers. Original assets are external runtime
inputs. Existing writable state is retained.

Five paired low/high mappings share real backing pages. Fixed placement is
checked against existing mappings. The bridge locks owned pages and applies
data protection changes to both views with rollback. Service outputs use
validated original buffers and actual host results. Unknown bindings stop with
a diagnostic. Additional original process admission currently returns error
135 and PID -1; no Print-ready event is fabricated.

SIGTERM invokes the original registered termination veneer with the captured
frame and original A5. The original desktop saves state, closes catalogs and
displays its external-termination alert (1033/3). Acknowledge that original
alert to complete recursive Terminate and release the environment lock.
Frontend exit alone does not establish successful original cleanup.

`GROUP=lisa sh tests/run-qemu.sh` runs t_lisa: pixel packing, cursor composition,
input records and interruptible waits, and the module's registration and load.
Original desktop and cleanup verification requires the supplied assets.

AMIX verification used ordinary UID 103: the unchanged Office and libraries
produced the original 14 icons, Profile desktop and menus with actual mouse
input. Two consecutive launches used the same persisted state. Each completed
original recursive Terminate after acknowledging the external-termination
alert, exited zero and released the real environment lock.
