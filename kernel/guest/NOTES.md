# Guest processes

Design: `docs/guest-container-design.md` (§2, §3, §13, §15 G0), `docs/aux-kernel-design.md` (§1–§5, §9 M0/M1), `docs/aux-syscall-translation.md`, `docs/dlm-impl-spec.md` §9.

## Pieces

```
 static (relinked)                     modules (/tests/aux/mod.d, auto-loaded)
 --------------------------------      ----------------------------------------
 35 vector gates  -> guest_gate_c      guestcore  MOD_HOOK_WRAPPER: guest_proc life,
 hooksw[6], guest_trap                            dispositions, sendsig/vur dispatch
 ev_config/fork/exec/exit              auxcore    A/UX profile: trap #0/#15 translator,
 sendsig, valid_usr_range                         errno/signals, SVR3 + BSD frames
 dlm: execsw[11], exec slots, hooks    auxexec    MOD_EXEC_WRAPPER 0x150 EXF_FIRST
```

| File | Role |
|---|---|
| `include/guest.h` | `guest_proc`, `guest_profile`, dispositions (`GD_*`), vCPU page offsets, frame accessors (`GR_*`), shim hooks |
| `include/guest_rpage.h`, `guest_rom.h` | register-page and ROM-descriptor interfaces (G0; currently unused) |
| `guest_shim.c` | events stubs, `sendsig`/`valid_usr_range` wrappers, `hooksw`, `guest_trap` |
| `guest_gate.s`, `mkgates.py` | gate common path; per-vector gates and `guest_chain[]` generated from the base's vector table |
| `patch_vec.py`, `elfrel.py` | fail-closed retarget of the gated `M68Kvec` slots |
| `chkguest.py` | image checks (run by `kernel/build.sh`) |
| `mod/guestcore`, `mod/auxcore`, `mod/auxexec` | modules |
| `test/chktab.py` | call table vs A/UX `sysent` argument counts, AMIX targets, signal maps |

Gated vectors: 2–11, 32–41, 43–47, 48–55, 60, 61.  Fault vectors send supervisor-mode exceptions straight to the stock handler.  A native process pays `move, movea, tst, movea, bne, jmp` (plus `btst, beq` on fault vectors).  A guest process goes to `guest_gate_c`, which saves the frame like the stock trap path and calls the `guest_trap` hook; declined exceptions restore everything and jump to the previous handler with the frame untouched; handled ones leave through `ureturn`.  Vector 42 (trap #10, same system-call path as trap #0) is left ungated as the timing reference.

A/UX exec: `auxexec` claims 0x150 first.  SVR3 COFF (`STYP_LIB`) → `ENOEXEC` → stock `coffexec`.  A/UX images are loaded by stock `coffexec` with `guest_loading` naming the process; `ev_exec` inside it creates the `guest_proc`.  Shared libraries named by the A/UX `.lib` section (64-byte absolute path records) are opened and checked before the old image goes, then mapped at their link addresses after `coffexec` (see M2).  A/UX execs are serialized on `guest_loading`, with a `u_qsav` guard.

## Tests

- `kernel/build.sh` runs `chkguest.py` on the linked image; `kernel/dlm/build.sh` runs the DLM state harness, which covers the exec and hook linkages.
- `test/chktab.py` checks the call table against A/UX `sysent` argument counts, AMIX targets and signal maps.
- `t_aux` in `tests/` (needs a local A/UX root): auxexec auto-loads with auxcore and guestcore; A/UX `/bin/sh` runs `-c` and scripts (loops, case, command substitution, redirection, `read`, `test`, subshell status, native and A/UX children, pipes), `cp`, `mv`, `rm`, `rmdir`, `ls`, `awk`; traps with `kill` (BSD frames, `sigcleanup`); a child killed by SIGKILL gives `$?` 137; with all A/UX processes gone auxcore is not held. The `abi` program and library, `sleep`, `more` and `vi` checks are listed under M2.
- The gates add no measurable cost to a native `getpid` (about 1.66 µs per call through trap #0 and #10 on QEMU q800, gated and stock).

## Deviations

1. **`hat_map`.**  The retained 030 `hat_map` → `hat_growsdt` writes a legacy segment-table descriptor for each 1 GB section into root words `8*s`, which for sections 0 and 1 overwrites 040 root entries 0–3 (VA 0–0x07ffffff) and corrupts page tables.  `mac/vtop/patch_vtop.py` stops `hat_map` growing the legacy table for any segment.
2. **BSD signals are in M1.**  A/UX `/bin/sh` calls `setcompat(COMPAT_BSDSIGNALS)` at start, so `sigvec`, `sigblock`, `sigsetmask`, `sigpause`, `sigstack`, the BSD frame and `sigcleanup` are implemented.  The frame's stub is written to the user stack, so delivery pushes the caches (`dlm_cacheflush`).  The SVR3 frame (`ssig`, `sysm68k(2)`) is implemented but no static program in the set uses it.
3. **Open in the static part:** the `fsig` override and the `usrxmemflt` wrapper (G1/M3); gates for vectors 2–11 and 48–61 are installed but every profile declines them.
4. **DLM:** `MOD_HOOK_WRAPPER` has only the hook linkage (the misc one does nothing).  An exec slot's `exec_core` is the shadowed row's (`coffcore`), not `nodev`, so core dumps of COFF processes still work.  `MOD_TY_SYS` (syscall slots) is open.  `mkmod` builds these modules as they are (`$modtype` unset).
5. **`execsw` is initialised** (`{ { 0 } }`) so it sits in `.data`, as the relink's data-override check requires.
6. **Calls marked `AE_TODO`** (EINVAL + one console line): ptrace, msg/sem, phys, locking, xinfo, fidop, csop, asio, sema, memlock, host id/name setting, `settimeofday`, mount, getcterm, AppleTalk, slot manager, reboot/powerdown.  Sockets return ENETDOWN, as A/UX does without its network module.

## M2

| Piece | What |
|---|---|
| `auxexec` | `.lib` loader: up to 4 libraries; each must be VREG, have an x bit and pass `VEXEC`, and be 0x150 with text, data and bss and a.out 0410/0413 (F_EXEC not required).  Failures (`ELIBACC`, `ELIBBAD`, `ELIBSCN`, `ELIBMAX`) come back to the caller with the old image intact.  After `coffexec`: `execmap` at the link addresses (copied, offsets not page-congruent), then a cache push; a mapping failure is SIGKILL, as in the stock loader |
| profile | `gpf_vur`: quadrant 1 allowed (no wrap, no quadrant crossing); `gpf_fork`/`gpf_exit` disarm the itimer and alarm (`GPF_EXEC`: exit for an exec of another profile) |
| `auxmisc.c` | `select` (on `poll`), `wait3`/`waitpid` (`waitid`; A/UX status word; rusage never written, as on A/UX), `fcntl` F_GETLK/SETLK/SETLKW (SVR3 16-byte `flock`; conflict EACCES), F_GETOWN/SETOWN, `flock` (whole-file record lock; the fd's mode does not limit it, but the caller must own the file or have the access the lock kind needs, and not with mandatory locking; else EBADF; EWOULDBLOCK), `statfs`/`fstatfs` (64-byte BSD form), `truncate`/`ftruncate` (F_FREESP), `utimes` (whole seconds), ITIMER_REAL (at most a quarter of the callout table armed; else EAGAIN), `alarm` (a callout of `n` s plus a tick, never early; the stock whole-second alarm, given 1 s more, when no callout is free and for what is left at exec of a native program), `setreuid`/`setregid` (BSD rules; the saved id follows the new effective one when the real id is given or the effective one differs from the real one), `shmsys` (`IPC_*` renumbered, 44-byte `shmid_ds`), `sigpending`, TIOCPKT (`pckt` on a pty master; `read` turns each message into one BSD packet) |
| `auxconv.c`, `auxsig.c` | TCSETA* convert into scratch below the user sp (the caller's buffer stays untouched); `sigvec` with handler 3 (A/UX SIG_HOLD) blocks the signal |

Arguments an AMIX handler takes as user pointers (poll array, `utimbuf`, `shmid_ds`, termio) are converted in a scratch area 512 bytes below the caller's sp (`aux_gap`); for a handler on the signal stack, below the sp that stack was entered from.  Only the result fields are read back.

`t_aux` covers: `abi` (the calls above through trap #0 and #15, including a handler on a small signal stack calling `select`, `setreuid(r, r)` dropping for good, `flock` needing access, and raw disk and `/proc` transfers into quadrant 1), library checks (missing or relative → `ELIBACC`, not COFF → `ELIBBAD`, bss leaving the quadrant → SIGKILL), `sleep 1` (libc1_s, SVR3 frame), `more` (page, `--More--`, next page, `q`) and `vi` (append a line, `:wq`, file checked) on a pty with ptem/ldterm/ttcompat and TERM=vt100.

Deviations:

- ITIMER_VIRTUAL and ITIMER_PROF read zero and can only be cleared.  ITIMER_REAL has clock-tick resolution and is separate from `alarm`; it survives A/UX→A/UX exec and is dropped on exec of a native program.
- `flock` locks are per process (record locks), not per open file.
- TIOCPKT: a data message longer than the read buffer leaves its tail for the next read without a packet byte; other message types than data, flush, stop and start are skipped.
- `select`: at most 1024 descriptors, clamped to the descriptor limit.
- A program that switches stacks itself (not through `sigstack`) gets the scratch area below its own sp.

## Open (M3)

- `sgttyb` flag remap for TIOCGETP/TIOCSETP and TIOCGCOMPAT/TIOCSCOMPAT.
- msg/sem, `ptrace`, the `fsig` override and `usrxmemflt` wrapper; ITIMER_VIRTUAL/PROF if a program needs them.
- Library text shared read-only between processes instead of copied per exec.

## Sources

The docs above, the AMIX headers, the relink kit's `amiga/ml/vec.s`/`ttrap.s`, the kernel image's `systrap`, `u_trap`, `psig`, `gexec`, `procdup`, `remove_proc`, `exit`, `hat_map`, `sigsuspend`, `coffexec`, `fcntl`, `poll`, `waitsys`, `strioctl`, and, from the shipped A/UX binaries, `/bin/sh` (`signal`, `setcompat`, `_sigcode`), the libc system-call stubs, `/usr/include` (`compat.h`, `fcntl.h`, `sys/vfs.h`, `sys/shm.h`, `sys/ioctl.h`) and the `sysent` table.
