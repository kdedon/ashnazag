# Native C call boundary

A profile can own trap zero while its ported drivers call native AMIX C services. `guest_native.h` defines a host boundary independent of any environment ABI.

Embed one `struct guest_native` in the profile's private allocation. After attaching, clear `GPF_FTRAP13`, reserve vector 45 (trap 13), and call `guest_native_init`. Its control callback calls `guest_native_control`; recognized operations return zero with the result in D0. The user stubs in `nativehost/nativecall.s` expose `nh_native_begin`, `nh_native_end`, and `nh_native_depth`. Begin/end return zero or a positive native error number. Depth returns 0–16 for a valid record.

Each begin must have a matching end, including error paths. Nesting is capped at 16. The outer begin saves QUIET and the A-line, privilege, and trap fast-dispatch bits, clears those fast paths, and sets `GPF_NCALL | GPF_QUIET`. The final end restores the saved bits. Trap 13 must remain reserved, with its fast-reflection bit clear. Place the control stubs outside `gp_hostlo`–`gp_hosthi`, whose exceptions bypass profile dispatch.

While NCALL is set, guestcore sends exceptions other than the boundary control trap to native handlers, uses native user-pointer validation and signal framing, and suppresses private interrupt callbacks. QUIET postpones ordinary guest signals while preserving the process signal mask and allowing termination and synchronous fault signals. A caught termination signal runs its native signal handler. The caller must keep that handler suitable for the active native C scope.

Lifecycle callbacks are mandatory: call `guest_native_fork` to rebind the copied record to the child, and `guest_native_exit` before releasing profile state. A retained-profile exec callback must initialize the record after the core resets the CPU flags. Record addresses are checked against `gp_size`; control requests carry no user pointer. Abrupt termination frees the process's embedded record through its profile exit callback.

This boundary preserves host scheduling and the process address space. Environment-specific argument conversion, stack selection, register preservation, and driver contracts remain the profile's responsibility. Existing profile and gate layouts are unchanged; the new low-word flag uses bit 15.

Host verification:

```sh
sh tests/nativecall/host/run.sh
```

Bounded AMIX fixture:

```sh
AMIX_TC=/path/to/toolchain/amix AMIX_CORE=/path/to/installed/core \
KERNEL=/path/to/unix-mac.elf M68K_OBJDUMP=/path/to/modern/m68k-elf-objdump \
sh tests/nativecall/build.sh /tmp/nativecall-fixture
```

The builder compiles clean guestcore and a synthetic private-trap profile, checks packaged gates and imports, and creates a checked 4 MB root. It starts no emulator. Init runs only `t_nativecall`, with a 35-second runner limit. The test covers private and native trap routing, fast-reflection configuration, nesting, signals, pointer-policy bypass, fork, exec, detach, and module lifetime.

AMIX runtime verification on 2026-10-12 passed all 25 checks in 117 ms, with zero failures or skips, on the mac68k 68040 kernel using a 128 MiB test machine. The deliberate invalid-memory probe terminated through the native bus-error path; the fixture then verified restored private traps, active detach and module unload.
