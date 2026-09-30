# A/UX → AMIX (SVR4) system-call translation

Specification for the translator inside the kernel's A/UX personality: A/UX 3.1 Mac-environment programs (68k COFF) running on the SVR4.0 kernel built from the AMIX 2.1 relink kit.

Sources: the shipped A/UX `/unix` and user binaries, A/UX headers and man pages, AMIX headers, AMIX kernel objects (`os/exp`, `fs/fs.exp`, …) and AMIX `libc`/`libsocket`. *(uncertain)* marks inferences not fully traced. Tools: `tools/auxsysent.py` (decodes A/UX `sysent`), `tools/coffobjdis.py` (disassembles COFF archive members), `tools/m68disk.py` (m68dis that resyncs past jump tables), `tools/callers.py` (lists references to a function).

Numbers are decimal unless written 0x…. "A/UX kernel" addresses are `/unix` addresses.

## 1. Entry and exit conventions

### A/UX (from `syscall0`/`syscall1` in `/unix`)

| | `trap #0` (`syscall0`) | `trap #15` (`syscall1`) |
|---|---|---|
| Number | `d0 & 0xff`; 0 = indirect: number at `usp+4`, args from `usp+8` | `d0 & 0xff`; **150 is handled first** (`sigcleanup`, BSD signal return) |
| Args | `sysent[n].argc` longs copied from `usp+4` | a0, d1, a1, d2, a2, d3 (6th arg in **d3**: `recvfrom`, `sendto`) |
| Success | carry clear; d0 = rval1; d1 = rval2 (initialised to the caller's d1, so calls that don't set it leave d1 unchanged) | same |
| Error | carry set, d0 = errno | same |
| Restart | kernel backs PC up by 2 (re-executes the trap; d0 and args are intact) | same |
| Other regs | preserved | preserved |

- `wait` (7) with **CCR = 0x1f** on entry is `wait3`: options in a0, `rusage *` in d1 (libc `wait3.o`: `ori #$1f,ccr; trap #15`).
- Any number can be reached through either trap; the translator gathers args by trap type, then dispatches by number.
- Per-proc flag word for compat modes: proc+0x72 (`getcompat`/`setcompat`).

### AMIX (from `systrap` in `os/exp` and AMIX `libc`)

- `trap #0`, d0 = number, args from `usp+4` (`lfuword` × `sysent[n].sy_narg`), handler `int h(void *uap, rval_t *rvp)` returns errno. Success: d0 = r_val1, d1 = r_val2 (r_val2 preloaded with the caller's d1). Error: carry set, d0 = errno. EINTR becomes **ERESTART (91)** when the signal allows restart; AMIX libc stubs retry on 91 (`cmpi.b #$5b,d0`). EFBIG (27) posts SIGXFSZ.
- So A/UX `trap #0` args arrive exactly where AMIX expects them. Differences are numbers, structures, errno values, restart and signals.
- AMIX register frame (`u+0x864` = `u_ar0`): +0 usp, +4 d0, +8 d1, …, +0x40 SR (word), +0x42 PC. Needed by the trap-#15 argument fetch and the signal code.
- AMIX `sysent` has 142 entries; numbers 64–77, 82–83, 105, 140 are `nosys`. A/UX numbers ≥ 64 mostly collide with unrelated AMIX calls, so **nothing is passed through by number**: every A/UX number goes through the translator's own table.

### Translator structure

- Per-process personality flag, set at exec of an A/UX image, cleared at exec of a native image; inherited by `fork`. The personality owns per-process A/UX state (`struct aux_proc`): compat flags, BSD signal state (`sigstack`, `SV_INTERRUPT`/`SV_ONSTACK` masks, `u_code`), signal-frame flag word, `tz`, `hostid`.
- `trap #0` and `trap #15` vectors of an A/UX process go to `aux_systrap`, which: fetches args (stack or registers), looks up the A/UX table, runs the entry, maps errno, applies A/UX restart (PC −= 2) instead of returning ERESTART, sets carry/d0/d1, then does the normal AMIX exit work (`issig`/`psig`, preemption, profiling) as `systrap` does.
- Implementation classes used in the tables:

| Class | Meaning |
|---|---|
| **N** | call the AMIX handler with the A/UX `uap` unchanged (same arg order and meaning) |
| **C** | convert args (numbers, flags) and/or structures, then call the AMIX handler. Structures are converted in kernel buffers; where the AMIX handler insists on user pointers, the converted copy goes into a scratch area below the user sp (a "stack gap", within the stack's grown region) and its user address is passed |
| **K** | personality code calling AMIX kernel internals directly (vnode ops, `cred`, callouts, `psignal`, …) |
| **U** | upcall to a user-space helper |
| **S** | stub with a fixed result |

- Errno: every error goes through the SVR4→A/UX table (§3) on the way out; ERESTART never reaches A/UX code.
- Restart rule *(uncertain in detail)*: after a caught signal interrupts a call (AMIX returns EINTR or ERESTART), restart (PC −= 2) when the process has `COMPAT_BSDSIGNALS`, the signal's A/UX handler was not set with `SV_INTERRUPT`, and either `COMPAT_SYSCALLS` is set or the call is one BSD restarts (read, write, readv, writev, ioctl, wait/wait3/waitpid, recv*/send*). Otherwise return A/UX EINTR (4).
- Nonblocking: A/UX default compat is `COMPAT_BSDPROT|COMPAT_BSDNBIO` (0x403). With `COMPAT_BSDNBIO`, an A/UX `O_NDELAY`/`FNDELAY` descriptor behaves like SVR4 `O_NONBLOCK` and EAGAIN is reported as A/UX **EWOULDBLOCK (55)**; without it, map to SVR4 `O_NDELAY` (read returns 0) and EAGAIN → 11.
- Clock ticks: A/UX `HZ` = 60; AMIX is 60 in the normal configuration but `PAL ? 50 : 60` in one header branch (§5.6). If the built kernel runs at 50 Hz, values in ticks (`times`) are rescaled ×60/HZ.
- Device numbers: A/UX `dev_t` is 16-bit (major<<8 | minor). AMIX `dev_t` is 32-bit (expanded). Stat results, `mknod` and `ustat` translate through a table (personality drivers such as `uinter` get fixed A/UX-visible majors); unknown devices get a hashed 16-bit value.

## 2. Call table

Columns: A/UX number, name, trap, args (A/UX `sysent` argc) → AMIX number/handler, class, transformation. "Used by" is in [aux-syscalls.md](aux-syscalls.md). Rows marked **(+)** are not in that list but are judged likely needed.

### 2.1 SVR2-style calls (0–67)

| A/UX | Call | Trap/args | AMIX | Class | Transformation and notes |
|---|---|---|---|---|---|
| 1 | rexit | 0 / 1 | 1 rexit | N | |
| 2 | fork | 0 / 0 | 2 fork | N | AMIX returns ppid in d0 and **d1 = 1 in the child**, child pid and d1 = 0 in the parent (`fork1`); A/UX libc tests `tst.b d1` — compatible |
| 3 | read | 0 / 3 | 3 read | N | errno/NBIO rules (§1); restartable |
| 4 | write | 0 / 3 | 4 write | N | same |
| 5 | open | 0 / 3 | 5 open | C | open flags translated (§5.4); `O_NDELAY` per NBIO rule |
| 6 | close | 0 / 1 | 6 close | N | |
| 7 | wait | 0 / 1 (trap 15 = wait3) | 7 wait / internals of 107 waitsys | C / K | plain wait: status in d1, **signal numbers inside the status translated** (termsig, stopsig). wait3 (CCR = 0x1f): K — call the AMIX `waitid` internals with `WEXITED\|WTRAPPED` (+`WNOHANG` 0100 → A/UX `WNOHANG`, `WUNTRACED` → `WSTOPPED`), build the A/UX status word (exit: code<<8; signalled: sig \| 0x80 core; stopped: sig<<8 \| 0x7f), rusage zero-filled *(or from child times)* |
| 8 | creat | 0 / 2 | 8 creat | N | |
| 9 | link | 0 / 2 | 9 link | N | |
| 10 | unlink | 0 / 1 | 10 unlink | N | |
| 11 | exec **(+)** | 0 / 2 | 11 exec | N | as exece |
| 12 | chdir | 0 / 1 | 12 chdir | N | |
| 13 | gtime | 0 / 1 | 13 gtime | N | time in d0 |
| 14 | mknod | 0 / 3 | 14 mknod (or 126 xmknod) | C | `dev` translated A/UX→AMIX via the device table; FIFOs (dev 0) direct |
| 15 | chmod | 0 / 2 | 15 chmod | N | |
| 16 | chown | 0 / 3 | 16 chown | N | `COMPAT_BSDCHOWN` ≈ AMIX `rstchown` behaviour; not per-process on AMIX *(accept)* |
| 17 | sbreak | 0 / 1 | 17 brk | N | address-space layout is the loader's concern |
| 19 | seek | 0 / 3 | 19 lseek | N | |
| 20 | getpid | 0 / 0 | 20 getpid | N | d1 = ppid in both |
| 22 | unmount | 0 / 1 | 22 umount | N | |
| 23 | setuid **(+)** | 0 / 1 | 23 setuid | N/K | `COMPAT_BSDSETUGID` (no saved-id handling) → K if it matters |
| 24 | getuid | 0 / 0 | 24 getuid | N | d1 = euid |
| 25 | stime | 0 / 1 | 25 stime | N | |
| 26 | ptrace | 0 / 4 | 26 ptrace | C/S | request codes match SVR; `PEEKUSER`/`POKEUSER` offsets refer to the A/UX u-area → emulate the few register offsets A/UX debuggers use, else EIO. Low priority (libc only) |
| 27 | alarm | 0 / 1 | 27 alarm | N | |
| 29 | pause | 0 / 0 | 29 pause | N | |
| 30 | utime | 0 / 2 | 30 utime | N | `utimbuf` = 2 longs in both |
| 33 | access | 0 / 2 | 33 access | N | |
| 34 | nice | 0 / 1 | 34 nice | N | |
| 36 | sync | 0 / 0 | 36 syssync | N | |
| 37 | kill | 0 / 2 | 37 kill | C | signal number A/UX→SVR4 (§4.1) |
| 38, 50 | sysm68k | 0 / 5 | — | K | never passed to AMIX 50 (different subcommands); see §6 |
| 39 | setpgrp | 15 / 3 | 39 pgrpsys | C | a0 = cmd: 0 → `pgrpsys(0)` (getpgrp); 1 → with `COMPAT_BSDTTY`: BSD `setpgrp(pid, pgrp)` → `pgrpsys(5, pid?pid:self, pgrp)` (setpgid; SVR4 is stricter: only self/children in the same session — accept), without it: SVR `setpgrp()` → `pgrpsys(1)`. Result pgrp in d0 |
| 41 | dup | 0 / 1 | 41 dup | N | |
| 42 | pipe | 0 / 0 | 42 pipe | N | fds in d0/d1 in both; AMIX pipes are STREAMS (bidirectional) — harmless |
| 43 | times | 0 / 1 | 43 times | N/C | tick rescale only if AMIX HZ ≠ 60 (§1) |
| 44 | profil | 0 / 4 | 44 profil | N | scale semantics same *(uncertain)* |
| 45 | lock | 0 / 1 | 45 lock_mem (plock) | N | `UNLOCK/PROCLOCK/TXTLOCK/DATLOCK` = 0/1/2/4 in both |
| 46 | setgid **(+)** | 0 / 1 | 46 setgid | N/K | as setuid |
| 47 | getgid | 0 / 0 | 47 getgid | N | d1 = egid |
| 48 | ssig | 0 / 2 | 48 ssig | C/K | only when `COMPAT_BSDSIGNALS` is clear (A/UX returns EINVAL otherwise); sig 1–31, not 9; number translated; no SIGDEFER-style high bits in A/UX. Returns old handler. SVR3 semantics (reset on delivery except SIGILL/SIGTRAP) = AMIX `signal()` semantics. Frame per §4.3 |
| 49 | msgsys **(+?)** | 0 / 6 | 49 msgsys | C | `IPC_RMID/SET/STAT` 0/1/2 → 10/11/12; `msqid_ds` conversion. Not used by the Mac environment |
| 51 | sysacct | 0 / 1 | 51 sysacct | N | |
| 52 | shmsys | 0 / 4 | 52 shmsys | C | ops 0 shmat, 1 shmctl, 2 shmdt, 3 shmget in both. shmctl: `IPC_RMID/SET/STAT` **0/1/2 → 10/11/12** (AMIX `shmctl` accepts only 3–12), `SHM_LOCK/UNLOCK` 3/4 same; `shmid_ds` conversion (§5.9). **shmat must allow VA 0**: Mac RAM is attached with `shmat(id, 1, SHM_RND)` (rounds to 0), ROM with `shmat(id, 0x40800000, 0)` |
| 53 | semsys **(+?)** | 0 / 5 | 53 semsys | C | as msgsys |
| 54 | ioctl | 0 / 3 | 54 ioctl | C | per-descriptor classification and command translation (§5.3) |
| 55 | phys | 0 / 4 | — | K | §6 |
| 56 | locking | 0 / 3 | `locking` (in `fs.exp`, reached via cxenix 40) | K | call AMIX `locking` directly; XENIX modes same *(uncertain)* |
| 57 | utssys | 0 / 3 | 57 utssys | C | type 0 (uname): §5.7; type 2 (ustat): dev translated, `ustat` copied field-wise (A/UX 18 bytes, AMIX 20) |
| 59 | exece | 0 / 3 | 59 exece | N | personality decided by the new image's type |
| 60 | umask | 0 / 1 | 60 umask | N | |
| 61 | chroot | 0 / 1 | 61 chroot | N | |
| 62 | fcntl | 0 / 3 | 62 fcntl | C | §5.4 |
| 63 | ulimit | 0 / 2 | 63 ulimit | N | |
| 64 | reboot | 0 / 1 | 55 uadmin | C | arg `0x4a53xxxx` (`RB_MAGIC` \| flags; kernel substitutes 0x4a530060 when the magic is missing): `RB_HALT` → `uadmin(A_SHUTDOWN, AD_HALT)`, else `AD_BOOT`; `RB_NOSYNC` skips sync. Root only |
| 65 | powerdown **(+)** | 0 / 1 | 55 uadmin | C | = reboot 0x4a530061 (halt/power off); possibly used by the Finder's Shut Down *(uncertain)* |
| 66 | sysslotmanager | 0 / 2 | — | K | §6 |
| 67 | swapmmumode | 0 / 1 | — | K | §6 |

### 2.2 BSD-style calls (70–169)

| A/UX | Call | Trap/args | AMIX | Class | Transformation and notes |
|---|---|---|---|---|---|
| 70–72, 75–81, 83–85, 90–93 | sockets | 15 / see [aux-syscalls.md](aux-syscalls.md) | — | K | §7 |
| 82 | select | 15 / 5 | 87 poll internals | K | §7.3 |
| 73 | gethostid | 15 / 0 | 139 systeminfo | K | `SI_HW_SERIAL` string → number; or a personality `hostid` set by sethostid |
| 74 | gethostname | 15 / 2 | 139 systeminfo | K | copy `utsname.nodename`, truncate to len, NUL-terminate if room |
| 86 | sethostid | 15 / 1 | — | K | root only; stored in personality state |
| 87 | sethostname | 15 / 2 | 139 systeminfo `SI_SET_HOSTNAME` (258) | K | root only |
| 88 | setregid | 15 / 2 | — | K | AMIX 4.0 has no setre*id: edit the `cred` (`crdup`, `cr_rgid/cr_gid`, saved gid) under BSD rules; −1 = unchanged |
| 89 | setreuid | 15 / 2 | — | K | same for uids |
| 100 | getdomainname | 15 / 2 | 139 systeminfo `SI_SRPC_DOMAIN` (9) | K | |
| 101 | setdomainname | 15 / 2 | 139 systeminfo `SI_SET_SRPC_DOMAIN` (265) | K | |
| 102 | getgroups | 15 / 2 | 92 getgroups | N | `gid_t` 4 bytes in both |
| 103 | setgroups | 15 / 2 | 91 setgroups | N | |
| 104 | getdtablesize | 15 / 0 | — | K | current `RLIMIT_NOFILE` of the process |
| 105 | flock | 15 / 2 | fcntl-lock internals (`VOP_FRLOCK`) | K | whole-file record lock: `LOCK_SH` → `F_RDLCK`, `LOCK_EX` → `F_WRLCK`, `LOCK_UN` → `F_UNLCK`, `LOCK_NB` → `F_SETLK` else `F_SETLKW`; EAGAIN/EACCES → EWOULDBLOCK. Differences: fcntl locks are per process and dropped on any close of the file; `F_WRLCK` needs a writable fd (flock doesn't) — for read-only fds take the lock through a personality-private lock table *(decide)* |
| 106 | readv | 15 / 3 | 121 readv | N | `iovec` = {base, len}, 8 bytes in both |
| 107 | writev | 15 / 3 | 122 writev | N | |
| 108 | mkdir | 15 / 2 | 80 mkdir | N | |
| 109 | rmdir | 15 / 1 | 79 rmdir | N | |
| 110 | getdirentries | 15 / 4 | `VOP_READDIR` (as 81 getdents) | K | §5.2 |
| 111 | lstat | 15 / 2 | 124 lxstat internals | C | §5.1 |
| 112 | symlink | 15 / 2 | 89 symlink | N | |
| 113 | readlink | 15 / 3 | 90 readlink | N | |
| 114 | truncate | 15 / 2 | — | K | `lookupname` + `VOP_SETATTR(AT_SIZE)` (AMIX 4.0 has no truncate) |
| 115 | ftruncate | 15 / 2 | — (62 fcntl `F_FREESP` equivalent) | K | `VOP_SETATTR(AT_SIZE)` on the file's vnode; requires `FWRITE` |
| 116 | fsync | 15 / 1 | 58 fsync | N | |
| 117 | statfs | 15 / 2 | 103 statvfs / 35 statfs internals | C | §5.8 |
| 118 | fstatfs | 15 / 2 | 104 fstatvfs / 38 fstatfs | C | §5.8 |
| 123 | rename | 15 / 2 | 134 rename | N | |
| 124 | fstat | 15 / 2 | 125 fxstat internals | C | §5.1 |
| 125 | stat | 15 / 2 | 123 xstat internals | C | §5.1 |
| 126 | (nosys) | 15 / 2 | — | S | A/UX `nosys` posts **SIGSYS (12)** and returns without error; do the same |
| 127 | getcompat | 15 / 0 | — | K | return personality compat word |
| 128 | setcompat | 15 / 1 | — | K | §4.5 |
| 129 | sigvec | 15 / 3 | 98 sigaction internals | K | §4.2 |
| 130 | sigblock | 15 / 1 | 95 sigprocmask internals | K | §4.2 |
| 131 | sigsetmask | 15 / 1 | 95 sigprocmask internals | K | §4.2 |
| 132 | sigpause | 15 / 1 | 96 sigsuspend internals | K | §4.2 |
| 133 | sigstack | 15 / 2 | — | K | §4.2 |
| 134 | getitimer | 15 / 2 | 109 hrtsys internals | K | `ITIMER_REAL`: personality callout posting SIGALRM (tick resolution); `VIRTUAL`/`PROF`: AMIX hrt alarms on the user/process virtual clocks (as AMIX libc `getsetitimer` does: which 0/1/2 → clock 1/2/4) or EINVAL *(libc only)* |
| 135 | setitimer | 15 / 3 | same | K | same |
| 136 | _gettimeofday | 15 / 1 | — | K | `timeval` from `hrestime` (usec = nsec/1000) |
| 137 | _settimeofday | 15 / 1 | — | K | root only; set `hrestime` as `stime` does, with µs |
| 138 | adjtime | 15 / 2 | 138 adjtime | N | same number, same `timeval` |
| 141 | mount | 15 / 4 | 21 mount | C | BSD-style (type, dir, flags, data) → SVR4 (spec, dir, `MS_DATA\|MS_FSS\|MS_RDONLY`, fstype name, data, len); type numbers → names. Libc only; low priority |
| 142 | umount | 0 / 1 | 22 umount | N | |
| 143 | fchmod | 15 / 3 | 93 fchmod | N | third arg ignored |
| 144 | fchown | 15 / 3 | 94 fchown | N | |
| 145 | utimes | 15 / 2 | — | K | `VOP_SETATTR` with `va_atime/va_mtime` (ns = µs × 1000); NULL → now |
| 146 | setsid | 15 / 0 | 39 pgrpsys(3) | C | |
| 147 | setpgid | 15 / 2 | 39 pgrpsys(5, pid, pgid) | C | |
| 148 | getcterm | 15 / 0 | — | K | ENOTTY (25) without a controlling tty, else the ctty's A/UX 16-bit dev (session `s_dev`, translated) |
| 149 | sigpending | 15 / 1 | 99 sigpending(1, set) | C | 16-byte SVR4 `sigset_t` → A/UX 32-bit mask, renumbered |
| 150 | sigcleanup | 15 only | — | K | BSD signal return, §4.4 |
| 151 | waitpid | 15 / 3 | 107 waitsys internals | K | a0 = pid, d1 = status ptr (libc stores d1 itself), a1 = options; A/UX rejects \|pid\| ≥ 30000 with ECHILD. Status built as for wait3, returned in d1 |
| 152–167 | fidop, asio*, sema*, memlock, chnod, csop, x*stat, *setxinfo, atp_control | | — | K/S | §6 |
| 168 | gettimeofday | 15 / 2 | — | K | as 136; `tz` (8 bytes: minuteswest, dsttime) from personality state |
| 169 | settimeofday | 15 / 2 | — | K | as 137; stores `tz` |

A/UX `sysent` entries not listed and not judged needed: 18 ostat, 28 ofstat, 139 errsys, 155 asiowait (see §6), 162/164/166 (see §6).

## 3. errno mapping

Identical 1–45 (EPERM … EDEADLK). Different:

| A/UX | name | AMIX | | A/UX | name | AMIX |
|---|---|---|---|---|---|---|
| 55 | EWOULDBLOCK | 11 (EAGAIN) | | 79 | ETOOMANYREFS | 144 |
| 56 | EINPROGRESS | 150 | | 80 | ETIMEDOUT | 145 |
| 57 | EALREADY | 149 | | 81 | ECONNREFUSED | 146 |
| 58 | ENOTSOCK | 95 | | 82 | ELOOP | 90 |
| 59 | EDESTADDRREQ | 96 | | 83 | ENAMETOOLONG | 78 |
| 60 | EMSGSIZE | 97 | | 84 | EHOSTDOWN | 147 |
| 61 | EPROTOTYPE | 98 | | 85 | EHOSTUNREACH | 148 |
| 62 | ENOPROTOOPT | 99 | | 86 | ENOTEMPTY | 93 |
| 63 | EPROTONOSUPPORT | 120 | | 87 | ENOSTR | 60 |
| 64 | ESOCKTNOSUPPORT | 121 | | 88 | ENODATA | 61 |
| 65 | EOPNOTSUPP | 122 | | 89 | ETIME | 62 |
| 66 | EPFNOSUPPORT | 123 | | 90 | ENOSR | 63 |
| 67 | EAFNOSUPPORT | 124 | | 95 | ESTALE | 151 |
| 68 | EADDRINUSE | 125 | | 96 | EREMOTE | 66 |
| 69 | EADDRNOTAVAIL | 126 | | 97 | EPROCLIM | — |
| 70 | ENETDOWN | 127 | | 98 | EUSERS | 94 |
| 71 | ENETUNREACH | 128 | | 99 | EDQUOT | — |
| 72 | ENETRESET | 129 | | 100 | EDEADLOCK | 56 |
| 73 | ECONNABORTED | 130 | | 101 | ENOLCK | 46 |
| 74 | ECONNRESET | 131 | | 102 | ENOSYS | 89 |
| 75 | ENOBUFS | 132 | | | | |
| 76 | EISCONN | 133 | | | | |
| 77 | ENOTCONN | 134 | | | | |
| 78 | ESHUTDOWN | 143 | | | | |

AMIX-only values on the way out (SVR4 → A/UX): EAGAIN 11 → 11, or 55 under the NBIO rule and for sockets; ERESTART 91 → restart or EINTR (§1); EOVERFLOW 79 → EFBIG 27 *(or EINVAL)*; EPROTO 71, EBADMSG 77 → EIO; EILSEQ 88 → EINVAL; ENOLINK/ECOMM/EMULTIHOP/EREMCHG/ENONET → EIO; ELIB* → ENOEXEC; EBADFD 81 → EBADF; ENOTUNIQ 80 → EINVAL; ESTRPIPE 92, EUCLEAN 135 and anything else unknown → EIO. A/UX `nosys`-type failures are SIGSYS, not an errno.

## 4. Signals

### 4.1 Numbers and masks

| A/UX | name | AMIX | | A/UX | name | AMIX |
|---|---|---|---|---|---|---|
| 1–19 | HUP … PWR | same | | 26 | VTALRM | 28 |
| 20 | TSTP | 24 | | 27 | PROF | 29 |
| 21 | TTIN | 26 | | 28 | WINCH | 20 |
| 22 | TTOU | 27 | | 29 | CONT | 25 |
| 23 | STOP | 23 | | 30 | URG | 21 |
| 24 | XCPU | 30 | | 31 | IO | 22 (POLL) |
| 25 | XFSZ | 31 | | | | |

- Masks: A/UX mask = 32-bit `int`, bit (sig−1), A/UX numbering. AMIX user `sigset_t` = 4 longs, kernel `k_sigset_t` = 1 long, bit (sig−1), SVR4 numbering. Every mask crossing the boundary is renumbered bit by bit.
- A/UX strips SIGKILL and SIGSTOP from masks with `& 0xffbffeff` (bits 8 and 22).
- The Mac environment's signals ([aux-interrupts-and-gateways.md](aux-interrupts-and-gateways.md)): 6 SIGIOT (tick), 31 SIGIO (I/O completion) → **AMIX 22**, 30 SIGURG → 21, 7 SIGEMT, 16 SIGUSR1, 18 SIGCLD, 3 SIGQUIT, 13 SIGPIPE. Kernel code of the personality (uinter tick, sound, AppleTalk shim) posts **AMIX** numbers with `psignal`; only the frame and syscalls expose A/UX numbers.
- Wait status, `kill`, `sigvec`, `ssig`, `sigpending`, signal frames and `siginfo`-derived codes are all renumbered.

### 4.2 BSD signal calls (require `COMPAT_BSDSIGNALS`, else EINVAL)

Behaviour from `/unix` (`sigvec` 0x10027904, `setsigvec`, `sigblock`, `sigsetmask`, `sigpause`, `sigstack`):

| Call | A/UX behaviour | Implementation (K) |
|---|---|---|
| `sigvec(sig, nsv, osv)` | sig 1–31; setting 9 or 23 → EINVAL; `struct sigvec` {handler, mask, flags} 12 bytes; `osv.flags`: bit0 `SV_ONSTACK`, bit1 `SV_INTERRUPT`, bit4 `SV_NOCLDSTOP` (only reported for 18); mask stored `& ~(SIGKILL\|SIGSTOP)`; SIG_IGN clears pending; SIG_DFL on SIGCLD with `SNOCLDSTOP`-like proc flag | Set AMIX disposition for the translated signal as `sigaction` does: `sa_mask` = renumbered mask, flags: `SA_RESTART` unless `SV_INTERRUPT`, `SA_NOCLDSTOP` from bit 4, **no** `SA_RESETHAND`, no `SA_NODEFER` (signal held during handler, as BSD). Keep `SV_ONSTACK`/`SV_INTERRUPT` bits in `aux_proc` (the frame builder uses them) |
| `sigblock(mask)` | old mask in d0; `p_sigmask \|= mask`; for a Mac-layer process also ORs into `ui_sigmask` | `sigprocmask(SIG_BLOCK)` on renumbered mask; return old mask renumbered back; uinter hook for the layer mask |
| `sigsetmask(mask)` | old mask in d0; set; Mac layer: call `ui_setsched` hook | `SIG_SETMASK`; same hook |
| `sigpause(mask)` | set mask, sleep until a signal; returns EINTR | `sigsuspend` internals with the renumbered mask |
| `sigstack(nss, oss)` | 8-byte `struct sigstack` {sp (top of stack), onstack}; stored in u-area | kept in `aux_proc` (A/UX gives only a top pointer, no size, so it can't map to `sigaltstack`); used by the A/UX frame builder |

The Mac environment runs with BSD signals: `startmac` calls `set42sig()` (= `setcompat(getcompat() | 0x1c)`: `BSDSIGNALS|BSDTTY|SYSCALLS`), and `Patch.067C` installs all handlers with `sigvec(..., SV_ONSTACK)` on a 64 KB `sigstack`.

### 4.3 Delivery: the two A/UX frame formats

A/UX `sendsig(handler, sig, mask)` (0x10000000) picks the format from `COMPAT_BSDSIGNALS` (proc+0x75 bit 2). The personality replaces AMIX `sendsig(sig, siginfo, handler)` (weak-override wrapper: A/UX process → `aux_sendsig`, else the stock function). AMIX's own frame (ucontext 0x400 bytes + optional siginfo 0x80 + a 16-byte header with a **null** return address, returned through libc and `setcontext`) is useless to A/UX code.

Both formats start with private state pushed first (highest addresses) by `copyoutframe`: an extended exception frame if the signal interrupted a faulting instruction, and FP state (`fp_copyout`: fsave frame, FP control regs and fp0/fp1) — **skipped for SIGIOT and SIGIO in Mac processes**. Pushing FP state sets bit 2 of the frame flag word; bit 0/1 re-raise SIGSEGV/SIGBUS on return; bit 3 is an internal frame lock. The layout of this private area is read back only by the kernel, so the translator may use its own format (it must validate everything restored: user-mode SR only, no supervisor bits, known frame formats).

**SVR3 format** (ssig handlers; handler = libc `_sigcode`), from the new sp upwards:

| Offset | Size | Content |
|---|---|---|
| +0 | 4 | A/UX signal number |
| +4 | 4 | signal mask at delivery (second arg to the handler) |
| +8 | 2 | flag word (0 = simple frame) |
| +10 | 2 | SR (only the CCR byte is used on return) |
| +12 | 4 | PC |
| +16 | 4 | old usp — **only if flag ≠ 0** |
| … | | private area |

`_sigcode` calls the real handler through `_sigcall`, then: flag = 0 → pop 10 bytes and `rtr`; flag ≠ 0 → `sysm68k(2, d0)` (§4.4).

**BSD format** (sigvec handlers; handler called directly; on the `sigstack` if `SV_ONSTACK` and not already on it). 0x40 bytes, from the new sp F:

| Offset | Size | Content |
|---|---|---|
| F+0x00 | 4 | return address = F+0x24 (the stub below) |
| F+0x04 | 4 | signal number (A/UX) |
| F+0x08 | 4 | code (`u_code`; BSD codes, e.g. SIGFPE `KINTDIV` 2) |
| F+0x0c | 4 | `struct sigcontext *` = F+0x2c |
| F+0x10 | 1 | Mac interrupt level to restore (proc+0x87) |
| F+0x11 | 1 | new Mac interrupt level |
| F+0x12 | 2 | frame flag word |
| F+0x14 | 16 | saved d0, d1, a0, a1 |
| F+0x24 | 8 | stub `move.l #150,d0; trap #15` (copied from kernel `siglude`) |
| F+0x2c | 20 | `struct sigcontext` {onstack, mask, sp, pc, ps} |
| F+0x40 | | private area |

- `sc_ps` = hardware SR (low word) | **virtual SR** (high word, u+0x548 on A/UX); the virtual SR's trace bits are cleared (`& 0x3fff`) during the handler.
- For Mac processes (SMAC), every signal except SIGALRM (14) and SIGQUIT (3) raises the global Mac interrupt level (`ui_curlevel`) and the process waits until it's the current level (nested interrupt levels across the layer). This belongs to the uinter part of the personality, called from `aux_sendsig`.
- After building the frame: usp = F, PC = handler, u_code cleared.

### 4.4 Return paths

| Path | Entry | Restores |
|---|---|---|
| BSD | `trap #15`, d0 = 150 → `sigcleanup` (also `sysm68k(2)` when BSD signals are on) | reads 0x24 bytes at usp−4 (= F) and the `sigcontext` through the pointer at F+0x0c: onstack; mask (minus SIGKILL/SIGSTOP; renumbered to AMIX); d0/d1/a0/a1; usp = `sc_sp`; PC = `sc_pc`; SR = `sc_ps & 0xc0ff` (CCR + trace only); virtual SR = high word `& 0x700` (or `& 0xc700` if traced); Mac level byte; if flag ≠ 0 the private area at `scp+0x14` (FP state, exception frame); re-raise SIGBUS/SIGSEGV per flag bits. Returns without touching d0/d1 (they come from the frame) |
| SVR3 | `sysm68k(2, d0)` via `trap #0` from `_sigcode` (stack: ret, 2, saved d0, flag\|SR, PC, [old usp]) | flag from usp+0xc high word; PC from usp+0x10; CCR from usp+0xc low byte; **d0 = saved d0** (rval1 = arg at usp+8); if flag: old usp from usp+0x14, private area from usp+0x18 (else usp+0x14); if PC changed, clear the pending exception-frame format |

Handlers that `longjmp` out simply abandon the frame; the mask is then whatever `sigsetmask`/`sigblock` set (A/UX libc `longjmp` restores it via `sigsetmask` in BSD mode *(uncertain)*).

With `COMPAT_BSDSIGNALS` set (as `startmac` does), Mac handlers return through the on-stack `siglude` stub and `sigcleanup`, not `_sigcode`. `_sigcode`/`sysm68k(2)` is the path for SVR3-mode (`ssig`) handlers; both must be supported.

### 4.5 setcompat / getcompat

- `getcompat()` → d0 = compat word. `setcompat(flags)` → d0 = old word; stores `flags & ~COMPAT_CLRPGROUP`. EINVAL if the `COMPAT_BSDSIGNALS` bit would change while any signal is caught, held or pending (proc+0x1c ≠ 0), or if `BSDTTY` or `SYSCALLS` is set without `BSDSIGNALS`. `CLRPGROUP` clears the proc's 4.2-pgroup bit; `BSDTTY` sets it.
- Personality effects: `BSDSIGNALS` selects sigvec vs ssig semantics and frame format; `SYSCALLS` feeds the restart rule; `BSDNBIO` the nonblocking rule; `BSDTTY` job control (setpgrp semantics; on SVR4 job control is always on — accept); `BSDGROUPS` (new files take the directory's group) ≈ SVR4 default with setgid dirs *(accept)*; `BSDSETUGID`, `BSDCHOWN`, `BSDNOTRUNC` (ENAMETOOLONG instead of truncation — AMIX UFS already errors) best-effort; `POSIXFUS` ignored; `COMPAT_EXEC` keeps flags across exec (else reset to the default 0x403).
- `SLINK_ACCESS` (0x80000000): `Patch.067C`'s `vfs_trap_handler` sets it around File Manager operations ("treat the process as if it had no user identity when a path contains a symlink"). Implement in the personality's lookup path or ignore *(decide; affects only permission checks through symlinks)*.
- Default for a new A/UX image: 0x403 (`COMPAT_BSDPROT|COMPAT_BSDNBIO`).

### 4.6 Virtual interrupt level (uinter interaction)

- A/UX `P_SIGMASK`: for a Mac-layer process with a non-zero virtual IPL, the effective mask is `ui_sigmask | 0xffffbefb` (everything except SIGQUIT, SIGKILL, SIGTERM held). On AMIX this needs a hook where `issig`/`fsig` compute deliverability (override those functions, or have the privileged-instruction emulator adjust `p_hold` when it emulates SR writes). Held signals are released when the virtual IPL drops to 0 (`UI_sigpending`).
- The tick (SIGIOT) must arrive at 60 Hz regardless of AMIX `HZ` (50 on PAL): the uinter timer should use a 60 Hz source, or accumulate.

## 5. Structures crossing the boundary

Alignment: **A/UX cc aligns int/long/pointers to 2 bytes** (A/UX `struct stat` is 58 bytes, `st_size` at 0xe); **AMIX (SVR4 m68k ABI) aligns them to 4** (AMIX old stat is 32 bytes, `st_size` at 0x10). Structures with a short before a long therefore differ even when the fields match.

### 5.1 stat

A/UX `stat` (58 bytes; two header views of one binary layout, filled by `vno_stat` 0x10035ec2):

| Off | Size | Field (POSIX view / old view) | Source on AMIX (`vattr`) |
|---|---|---|---|
| 0x00 | 2 | st_dev | `va_fsid` → A/UX 16-bit dev |
| 0x02 | 2 | st_spare0 / st_ino (16-bit) | low 16 bits of `va_nodeid` |
| 0x04 | 2 | st_mode | `va_mode` \| type bits (`vttoif`) |
| 0x06 | 2 | st_nlink | `va_nlink` (clamp 0x7fff) |
| 0x08 | 2 | (st_spare1) / st_uid (16-bit) | `va_uid` |
| 0x0a | 2 | (st_spare1) / st_gid (16-bit) | `va_gid` |
| 0x0c | 2 | st_rdev | `va_rdev` → A/UX 16-bit dev |
| 0x0e | 4 | st_size | `va_size` |
| 0x12 | 4 | st_atime | `va_atime.tv_sec` |
| 0x16 | 4 | st_ino (POSIX) / st_inol (32-bit) | `va_nodeid` |
| 0x1a | 4 | st_mtime | `va_mtime.tv_sec` |
| 0x1e | 4 | st_spare2 (0) | |
| 0x22 | 4 | st_ctime | `va_ctime.tv_sec` |
| 0x26 | 4 | st_spare3 (0) | |
| 0x2a | 4 | st_blksize | `va_blksize` |
| 0x2e | 4 | st_blocks | `va_nblocks` (units: 512-byte blocks in both *(check AMIX UFS)*) |
| 0x32 | 4 | st_uid (POSIX, int) / spare4[0] | `va_uid` |
| 0x36 | 4 | st_gid (POSIX, int) / spare4[1] | `va_gid` |

AMIX `stat` (SVR4, `_STAT_VER` 2, via `xstat` 123/`lxstat` 124/`fxstat` 125): `st_dev` 0 (4), `st_pad1[3]`, `st_ino` 0x10, `st_mode` 0x14, `st_nlink` 0x18, `st_uid` 0x1c, `st_gid` 0x20, `st_rdev` 0x24, `st_pad2[2]`, `st_size` 0x30, `st_pad3`, `st_atim` 0x38 (timestruc 8), `st_mtim` 0x40, `st_ctim` 0x48, `st_blksize` 0x50, `st_blocks` 0x54, `st_fstype[16]` 0x58, `st_pad4[8]` → 0x88 bytes. Recommended: build the A/UX record straight from `VOP_GETATTR` (as AMIX `cstat`/`xcstat` do) rather than converting a user-level SVR4 stat.

A/UX `xstat` (98 bytes) = `stat` + `st_xerror` (4, at 0x3a) + `st_xinfo[9]` (36, at 0x3e); see §6.

### 5.2 Directory entries (getdirentries)

| A/UX `struct direct` | Off | Size | AMIX `struct dirent` | Off | Size |
|---|---|---|---|---|---|
| d_fileno | 0 | 4 | d_ino | 0 | 4 |
| d_reclen | 4 | 2 | d_off (cookie of next entry) | 4 | 4 |
| d_namlen | 6 | 2 | d_reclen | 8 | 2 |
| d_name[namlen+1] | 8 | pad to 4 | d_name (NUL-terminated) | 10 | pad to 4 |

Record size: A/UX `8 + ((namlen+1+3) & ~3)`; SVR4 `(10 + namlen + 1 + 3) & ~3`, so a converted record is never larger than its source. Implementation: `VOP_READDIR` into a kernel buffer (`UIO_SYSSPACE`, size = nbytes), convert each record, copy out; `*basep` = file offset before the read; afterwards set the file offset to the `d_off` of the last record converted (never split a record). A/UX `getdirentries` needs an fd open for reading (else EBADF) and returns the byte count. The man page requires `nbytes` ≥ the block size.

### 5.3 ioctl

**Encoding.** A/UX uses BSD encoding for **every** group: `IOC_VOID` 0x20000000, `IOC_OUT` 0x40000000, `IOC_IN` 0x80000000, size in bits 16–22 (max 127), group bits 8–15, number bits 0–7. AMIX uses BSD encoding only for groups `f s r i p`; `T t D S X d` are plain `(g<<8)|n`. Several A/UX `'T'` low words collide with unrelated AMIX commands, so **translate by matching the full 32-bit A/UX value, never by stripping the high bits**.

**Per-descriptor classification.** Before translating, classify the fd: (a) personality drivers (`uinter`, the AppleTalk shim, sound, nvram…) get the A/UX command **unchanged**; (b) sockets (`sockmod` on the stream, §7); (c) ttys (stream with `ldterm`, or pty); (d) other streams (`I_*`); (e) regular files (`FIO*`). Unknown A/UX commands on (c)–(e) → EINVAL (A/UX returns EINVAL/ENOTTY; use ENOTTY for non-ttys).

✓ = seen in A/UX binaries (`tools/ioctlscan.py`).

| Name | A/UX | AMIX | Translation |
|---|---|---|---|
| TCGETA ✓ | 0x40125401 | 0x5401 | + termio flag remap (§5.5) |
| TCSETA/W/F ✓ | 0x80125402/3/4 | 0x5402/3/4 | + remap |
| TCSBRK / TCXONC / TCFLSH ✓ | 0x20005405/6/7 | 0x5405/6/7 | arg by value, same |
| TCSBRKM / TCCBRKM | 0x20005408/9 | — | → TIOCSBRK 0x747b / TIOCCBRK 0x747a (AMIX 0x5408/9 = TIOCKBON/OF) |
| TCRESET | 0x2000540b | — | EINVAL |
| TCSETSTP / TCSETSTA | 0x8001540c/d | — | set VSTOP/VSTART through TCGETS/TCSETS (AMIX 0x540d = TCGETS) |
| TCSETDTR / TCCLRDTR | 0x2000540e/f | — | TIOCMBIS/TIOCMBIC 0x741b/0x741c with TIOCM_DTR (AMIX 0x540e/f = TCSETS/TCSETSW) |
| TCGETSTAT ✓ | 0x40105410 | — | emulate 16-byte `serstat` (3 ulong + 4 uchar) from TIOCMGET 0x741d (AMIX 0x5410 = TCSETSF) |
| TCSETRAWQ / TCSETBAUD / TCSETEXT | 0x80025411 / 0x80025412 / 0x80015413 | — | TCSETBAUD via c_cflag; others EINVAL *(check Serial driver use)* |
| TIOCGETP / TIOCSETP | 0x7408 / 0x7409 | 0x7408 / 0x7409 | `sg_flags` remap (§5.5); needs `ttcompat` or emulate |
| TIOCGPGRP ✓ / TIOCSPGRP ✓ | 0x40047477 / 0x80047476 | 0x7414 / 0x7415 | SVR4 needs same session |
| TC_PX_GETPGRP ✓ / SETPGRP ✓ | 0x40047466 / 0x80047465 | 0x7414 / 0x7415 | |
| TIOCGLTC ✓ / TIOCSLTC ✓ | 0x40067474 / 0x80067475 | 0x7474 / 0x7475 | `ltchars` identical; emulate through termios VSUSP/VDSUSP if `ttcompat` is absent; 0xff ↔ 0 disabled |
| TIOCNOTTY ✓ | 0x20007471 | 0x7471 | |
| TIOCGWINSZ ✓ / TIOCSWINSZ ✓ | 0x40087468 / 0x80087467 | **0x5468 / 0x5467** | `winsize` identical |
| TIOCPKT ✓ | 0x80047470 | — | pty packet mode: emulate in the pty driver or EINVAL *(CommandShell uses ptys; check)* |
| TIOCSETCONS / TIOCSETRDSIG | 0x80047469 / 0x2000746a | — | A/UX pty-specific; EINVAL |
| TIOCGCOMPAT ✓ / TIOCSCOMPAT ✓ | 0x40047479 / 0x80047478 | — | emulate (bit 0x1 = TOSTOP); AMIX 0x7479/8 = TIOCSDTR/CDTR |
| FIOCLEX ✓ / FIONCLEX ✓ | 0x20006601/2 | same | do as fcntl F_SETFD 1/0 |
| FIONREAD ✓ | 0x4004667f | same | streams: I_NREAD semantics differ (first message only) — sum queue if needed |
| FIONBIO ✓ | 0x8004667e | same | set/clear FNONBLOCK (0x80) in `f_flag` directly |
| FIOASYNC ✓ | 0x8004667d | same | streams: I_SETSIG S_INPUT\|S_OUTPUT (sockets §7) |
| FIOSETOWN / FIOGETOWN | 0x8004667c / 0x4004667b | same | caller/pgrp only (§7) |
| LDOPEN/LDCLOSE/LDCHG | 0x20004400/1/2 | 0x4400/1/2 | |
| LDGETT ✓ / LDSETT ✓ | 0x40064408 / 0x80064409 | 0x4408 / 0x4409 | `termcb` 6 bytes, same |
| LDGETU ✓ | 0x4004440a | — | EINVAL (AMIX 0x440a = LDSMAP) |
| SXTIOC* ✓ | 0x200062xx, 0x40026207 | — | no sxt on AMIX: EINVAL |
| UIOC* (disk format, disktune, modem) | 0x80245500 … | — | Apple-only; modem control → termiox TCGETX/TCSETX 0x5801/2 |
| I_NREAD … I_FIND (streams) | 0x40045301, 0x80095302 (I_PUSH ✓), 0x20005303 (I_POP ✓), 0x40095304, 0x20005305, 0x20005306 (I_SRDOPT ✓), 0x40045307, 0xc0105308 (I_STR ✓), 0x8009530b (I_FIND ✓) | 0x5301 … 0x530b | low word; `strioctl` 16 bytes on both; **I_STR's inner `ic_cmd` translated recursively** |
| I_MNAME | 0x20005340 | — | EINVAL |
| SIOC* (hiwat, pgrp, routes, if addr/flags/conf/mtu, ARP, multicast add/del) | 0x8004730x, 0x8030720a/b, 0x8020690c…0xc0206916, 0x8024691e…, 0x80206931/2 | same | pass value through to the socket code |
| SIOCGIFNETMASK ✓ / SIOCSIFNETMASK ✓ | 0xc0206917 / 0x80206918 | **0xc0206919 / 0x8020691a** | renumber |
| SIOCGIFMETRIC ✓ / SIOCSIFMETRIC | 0xc0206919 / 0x8020691a | **0xc020691b / 0x8020691c** | renumber |
| SIOCGIFBRDADDR ✓ / SIOCSIFBRDADDR ✓ | 0xc020691b / 0x8020691c | **0xc0206917 / 0x80206918** | renumber |
| SIOCSMAR/UMAR/GMAR | 0x8020692a/b, 0xc020692c | — | EINVAL (AMIX 'i' 44 = SIOCSETSYNC) |

### 5.4 open / fcntl / flock

| Constant | A/UX | AMIX | Note |
|---|---|---|---|
| O_RDONLY/WRONLY/RDWR, O_NDELAY, O_APPEND | 0/1/2, 0x4, 0x8 | same | O_NDELAY per the NBIO rule (§1) |
| FSHLOCK / FEXLOCK | 0x10 / 0x20 | — | kernel-internal; strip (AMIX 0x10 = O_SYNC) |
| FASYNC | 0x40 | — | F_SETFL FASYNC → I_SETSIG; not passed as a flag |
| O_SYNC | 0x80 | 0x10 | remap |
| O_CREAT / O_TRUNC / O_EXCL | 0x100 / 0x200 / 0x400 | same | |
| O_NONBLOCK | 0x4000 | 0x80 | remap |
| O_NOCTTY | 0x8000 | 0x800 | remap |
| O_GETCTTY, O_NOHUP, O_LOCKOUT, O_GLOBAL | 0x10000, 0x20000000, 0x40000000, 0x80000000 | — | drop *(O_GETCTTY: acquire ctty on open? uncertain)* |
| F_DUPFD … F_SETFL | 0–4 | 0–4 | F_GETFL/F_SETFL flag words remapped both ways |
| F_GETLK | 5 | **14** | AMIX 5 = old `o_flock` form |
| F_SETLK / F_SETLKW | 6 / 7 | 6 / 7 | struct converted |
| F_GETOWN / F_SETOWN | 8 / 9 | **23 / 24** | AMIX 8 = F_CHKFL; sockets per §7 |
| F_RDLCK / F_WRLCK / F_UNLCK | 1 / 2 / 3 | same | |

`struct flock`: A/UX {l_type 0/2, l_whence 2/2, l_start 4/4, l_len 8/4, l_pid 12/4} = 16 bytes; AMIX {l_type 0/2, l_whence 2/2, l_start 4/4, l_len 8/4, l_sysid 12/4, l_pid 16/4, pad 20/16} = 36 bytes. Copy fields; on F_GETLK return l_pid (l_sysid dropped).

flock(2) (A/UX 105): `LOCK_SH` 1, `LOCK_EX` 2, `LOCK_NB` 4, `LOCK_UN` 8; AMIX has no flock anywhere (libc, libucb, libsocket) — emulate as in §2.2.

### 5.5 termio, termios, sgttyb, ltchars

`termio` is 18 bytes on both with identical offsets (iflag 0, oflag 2, cflag 4, lflag 6 — shorts; c_line 8; c_cc[8] 9). Indices of c_cc[0..7] are the same. **A/UX termios never reaches the kernel**: `libposix` builds it from TCGETA + TIOCGLTC and writes it back with TCSETA* + TIOCSLTC (VSTART/VSTOP fixed at ^Q/^S), so only termio and ltchars need kernel translation.

Flag bits that differ (all others equal, including every oflag bit and all baud codes B0–B38400/EXTA/EXTB):

| Flag | A/UX | AMIX | Note |
|---|---|---|---|
| lflag IEXTEN | 0x200 | 0x8000 | AMIX 0x200 = ECHOCTL |
| lflag TOSTOP | 0x8000 | 0x100 | also bit 0x1 of the TIOCSCOMPAT word |
| cflag LOBLK | 010000 | 040000 | AMIX 010000 = RCV1EN |
| cflag CSTOPB15 | 0100000 | — | drop (AMIX 0100000 = XCLUDE) |
| iflag IMAXBEL, DOSMODE; lflag ECHOCTL, ECHOPRT, ECHOKE, DEFECHO, FLUSHO, PENDIN | — | AMIX only | clear when writing, hide when reading |

Disabled character: A/UX 0377, AMIX 0 (`_POSIX_VDISABLE`). Translate in character slots only (not c_cc[4]/[5] when ICANON is off — they are VMIN/VTIME). A/UX termios c_cc indices (library-only): VSUSP 8, VDSUSP 9, VSTART 10, VSTOP 11 (AMIX 10, 11, 8, 9).

`sgttyb` (8 bytes, identical layout); `sg_flags` bits differ: A/UX 0x1 O_HUPCL / AMIX O_TANDEM; 0x2 O_XTABS / O_CBREAK; 0x400 O_TBDELAY / O_TAB1; 0x800 O_NOAL / O_TAB2; XTABS 0x2 vs 0xc00. Same: LCASE, ECHO, CRMOD, RAW, ODDP, EVENP, delay fields. `ltchars` (6 bytes) and `winsize` (8 bytes) identical. A/UX has no `tchars`.

### 5.6 Time, limits

`timeval`, `timezone`, `itimerval` (16), `tms` (16), `utimbuf` (8): identical. `ITIMER_*` 0/1/2 same. A/UX has no getrlimit/setrlimit syscall and no `rusage` contents (wait3's rusage is a placeholder), so neither is translated. AMIX gettimeofday/itimers exist only as `hrtsys` (109) library wrappers (`HRT_TOFD`; `HRT_BSD` 12, `HRT_BSD_REP` 15, `HRT_BSD_CANCEL` 16, `HRT_BSD_PEND` 13; `it_interval` kept in user space by libc), so the personality implements them itself (§2.2).

HZ: A/UX 60. AMIX `sys/param.h` gives 60 in the configuration used (`CLOCK==1`), `PAL ? 50 : 60` in another branch *(verify the built kernel; if 50, rescale `times` and drive the Mac tick from a 60 Hz source)*.

### 5.7 utsname

A/UX: 5 × char[9] = 45 bytes (sysname 0, nodename 9, release 18, version 27, machine 36), through `utssys(buf, 0, 0)`. AMIX `utssys` type 0 is the obsolete 9-byte-field form *(verify output)* → N; otherwise build from `uname` 135 (5 × 257) truncating to 8 chars + NUL. `utssys` type 2 (ustat): A/UX 18 bytes, AMIX 20 (tail pad) — copy 18.

### 5.8 statfs

A/UX `statfs`/`fstatfs` (117/118, BSD style, 64 bytes): f_type 0, f_bsize 4, f_blocks 8, f_bfree 12, f_bavail 16, f_files 20, f_ffree 24, f_fsid[2] 28, f_spare[4] 36, f_fname[6] 52, f_fpack[6] 58. Build from AMIX `statvfs` (103/104, 156 bytes: f_bsize 0, f_frsize 4, f_blocks 8, f_bfree 12, f_bavail 16, f_files 20, f_ffree 24, f_favail 28, f_fsid 32, f_basetype[16] 36 …): f_bsize = f_frsize (block counts are in f_frsize units), f_type from f_basetype name (A/UX `MOUNT_*` numbers: ufs, svfs, nfs…), f_fsid = {f_fsid, 0}, fname/fpack empty. AMIX old `statfs` (35/38, 40 bytes) lacks f_bavail — don't use.

### 5.9 IPC

`ipc_perm`: A/UX 6 × ushort + key = 16 bytes; AMIX uid/gid/cuid/cgid/mode/seq as longs + key + pad[4] = 44 bytes. `shmid_ds`: A/UX 44 bytes (perm 0, segsz 16, reg 20, lpid 24 (ushort), cpid 26, nattch 28, cnattch 30, atime 32, dtime 36, ctime 40); AMIX (perm 0/44, segsz 44, amp 48, lkcnt 52, lpid 56, cpid 60, nattch 64, cnattch 68, atime 72, pad, dtime 80, pad, ctime 88, pad, pad[4]) = 112 bytes *(computed from headers)*. Convert field by field; `IPC_*` commands renumbered (§2.1).

### 5.10 Sockets

Identical: `sockaddr` (16), `sockaddr_in` (16), `sockaddr_un` (110), `linger` (8), `iovec` (8), `msghdr` (24, 4.3BSD-style with `msg_accrights`), `SOL_SOCKET` 0xffff, `SO_*` 0x1–0x100 and 0x1001–0x1008, `MSG_OOB/PEEK/DONTROUTE` 1/2/4, `AF_*` 0–16 (AF_APPLETALK 16), `IPPROTO_*`, `IP_OPTIONS`, `TCP_NODELAY/MAXSEG`.

Different: **`SOCK_STREAM`/`SOCK_DGRAM` are swapped** (A/UX 1/2, AMIX 2/1); `SOCK_RDM` 4 vs 5, `SOCK_SEQPACKET` 5 vs 6; AF 17 (A/UX ETHERLINK, AMIX NIT); A/UX multicast `IP_MULTICAST_*`/`IP_*_MEMBERSHIP` 2–6 have no AMIX equivalent (ENOPROTOOPT). `fd_set`: same bit layout, A/UX `FD_SETSIZE` 128 vs 1024 — use nfds. `fstat` on a socket: AMIX gives S_IFCHR, A/UX S_IFSOCK — the stat builder reports S_IFSOCK for `sockmod` streams.

### 5.11 Other

`sigvec` (12 bytes) vs `sigaction` (32: flags 0, handler 4, mask 8/16, resv 24/8): `SV_ONSTACK` 1 = `SA_ONSTACK`; `SV_INTERRUPT` 2 = absence of `SA_RESTART` 4; `SV_NOCLDSTOP` 0x10 → `SA_NOCLDSTOP` 0x20000 (AMIX 0x10 = `SA_NODEFER`). `SIG_HOLD` A/UX 3, AMIX 2. A/UX POSIX `sigaction` is a libposix wrapper over `sigvec`; `sigprocmask` over `sigsetmask` — neither reaches the kernel. Wait status encoding identical (signal numbers inside renumbered); options WNOHANG A/UX 1 / AMIX 0100, WUNTRACED 2 / 04. Mode bits S_IF*, access modes: identical.

## 6. A/UX-specific calls

Decision per call: **keep** (implemented in the personality), **map** (onto an AMIX facility), **stub** (fixed result). "Patch" = `Patch.067C`.

| # | Call | What A/UX does | Mac-side use | Decision |
|---|---|---|---|---|
| 38/50 sub 2 | sysm68k signal return | §4.4 | `_sigcode` | **keep** (signal code) |
| 38/50 sub 0x69 | CacheFlush `(addr, scope, cache, len)` | only on `cputype == 5` (68040): cache push/invalidate (scope 3 all, 2 page (len>>12 pages), 1 line *(Linux-like cacheflush args; uncertain)*); **any other CPU → EINVAL** | `__tb_coff_load`, `cBlockMove`, libmac `CacheFlush` | **keep**: 040 and **060** push+invalidate both caches (060: also clear branch cache); 020/030 → EINVAL like A/UX (Mac code then uses CACR through the privileged-instruction emulator, which must flush) |
| 38/50 sub 0x6a | tclrFileMgrFlag | rval = old system-wide `_fmgrflag`, clear it. Bits: 1 namespace change (create/link/symlink/rename/remove), 4 same by a non-Mac process, 2 attribute change (setattr, chnod), 8 same by non-Mac | Patch `vfs_trap_handler` polls it to decide whether to rescan directories | **keep**: global set from the personality's namespace/attribute calls; changes by native AMIX processes can't be seen — set bits 4/8 from hooks in `vn_create`/`vn_remove`/`vn_rename`/setattr paths if rescans matter *(decide)* |
| 38/50 sub 0x6b | get_cache_info | copy out 16 bytes `{sysinfo.lread, sysinfo.bread, 0, v_buf × v_sbufsz}` | `get_cache_info` | **map** from AMIX `sysinfo`/`v` |
| 38/50 sub 0x6c | clear_cache_info | root: zero lread/bread; else EPERM | `clear_cache_info` | **map** |
| 38/50 sub 0x6d | getBufCache | 16 bytes `{v_buf, v_sbufsz, *(sysinfo+0xb0) (availmem? uncertain), v_maxpmem << v_pageshift}` | `getBufCache` | **map** (approximate values are fine; used for cache sizing) |
| 38/50 other | 1, 3, 4, 5, 9, 0x63–0x68 | kernel-address queries, TLB flushes, set time | not used by Mac binaries | **stub** EINVAL |
| 55 | phys(n, va, size, pa) | root; map physical range at segment-aligned va | libc stub only (startmac uses the `UI_PHYS` uinter ioctl) | **stub** EINVAL; later a device-style mapping (`segdev`) if needed |
| 66 | sysslotmanager(sel, SpBlock*) | copy in 0x38-byte `SpBlock`, run ROM Slot Manager (sel ≤ 0x30) in the kernel, copy out, result in rval | Patch `cSlotManager`, `installVideoDriver`, `slot_*` | **keep**: emulate against a virtual slot table (the virtual NuBus video card); no ROM code in the AMIX kernel. Error for empty slots *(uncertain: smEmptySlot −300?)* |
| 67 | swapmmumode(mode) | non-24-bit process: rval = **1 − mode**, no change; 24-bit: swap MMU root, return previous mode (0 = 24, 1 = 32) | Patch `aSwapMMUmode` | **keep** trivially: 32-bit sessions only, return `1 − mode` |
| 127/128 | getcompat/setcompat | §4.5 | `startmac` (`set42sig`), Patch `vfs_trap_handler` (`SLINK_ACCESS`) | **keep** |
| 45 | lock | plock (root; 0/1/2/4) | libc | **map** → AMIX 45 |
| 152 | fidop(op, fidhdr*) | two kernel message queues (requests `fid_cqueue`, replies `fid_rqueue`, ≤ 20 entries each, else ENOMEM) between the Mac side and the `/etc/fidd` File-ID daemon. `fidhdr` 16 bytes {type, buf, bufsize, len}. Ops: 1 client send (flush replies, enqueue, wake server; returns msg id), 2 server receive (interruptible sleep; bufsize ≥ len else EINVAL; writes back type/len), 3 server reply, 4 client receive, 5 cancel by id; else EINVAL. `vn_rename` also posts type-1 messages `{long; old\0; new\0; cwd\0}` after every successful rename | Patch `fid_transaction` (commands 4 create, 5 resolve, 6 delete, 7 exchange, 8 rename, 10 remove; bufsize 0xc04). Without `fidd` each call retries for 120 ticks then fails with ioErr | **keep** (~150 lines, "kmsgq" shared with csop); post rename messages from the personality's rename (native renames not seen — accept); AMIX keeps no cwd string, so track a cwd path per A/UX process or send absolute paths; run the real `fidd` (A/UX binary, before the Mac environment) |
| 161 | csop(sel, buf, len) | catalog-search daemon IPC (`apple/csop.h`): 1 SEND_REQUEST (needs registered daemon, else EAGAIN), 2 SEND_REPLY, 3 RECV_REQUEST (root, blocking), 4 POLL_REQUEST (root), 5 RECV_REPLY, 6 POLL_REPLY (empty → EWOULDBLOCK 55), 7 SET_READY (root), 8 GET_READY, 9 SET_PID (root), 10 GET_PID. When ready, the kernel posts MOUNT, UNMOUNT, CREATE, REMOVE, RENAME, RFSMOD, SETXINFO notifications with root/cwd paths | Patch `csdalive` (8), `csdCatSearch` (6, 1, 5/6); `/etc/catsearchd` | phase 1 **stub**: GET_READY/GET_PID → 0, SEND_REQUEST → EAGAIN, others EINVAL, don't start catsearchd (PBCatSearch fallback *unverified*); phase 2 **keep** with kmsgq + notification hooks |
| 163/166/162 | xstat/xlstat/xfstat(path or fd, xstat*) | stat + `st_xerror` (0x3a) + `st_xinfo[9]` (0x3e, 36 bytes, `union xstat_finfo`); `st_xerror` = result of `VOP_GETXINFO`, call itself succeeds. A/UX UFS keeps xinfo in spare dinode words; **SVFS and NFS return EINVAL** | Patch `XSTAT` (`get_fs_finfo`, `get_mnt_dirIDs`) | **keep**: build the stat part (§5.1); phase 1 `st_xerror = EINVAL` (A/UX EINVAL 22) on every file system — the Mac side then behaves as on A/UX SVFS/NFS and takes Finder info from `%name` AppleDouble files |
| 165/164 | setxinfo/fsetxinfo(path or fd, xinfo*) | copy in 36 bytes, `VOP_SETXINFO` (UFS: owner or root, else EPERM), csop notification | Patch `SETXINFO` (`set_fs_finfo`, only when get didn't return −1) | phase 1 **stub** EINVAL. Phase 2 (persistent directory dirIDs/Finder info): a personality-owned store keyed by (fsid, ino), or free AMIX UFS dinode words once verified — **not** A/UX's layout (it uses time-spare words and the size high word, which SVR4 UFS probably uses) |
| 160 | chnod(path, mode, dev) | root; change an existing block/char node's type and rdev in place (no symlink follow); sets `_fmgrflag` 2 (+8) | no shipped caller | **stub** EINVAL (later: remove + create keeping owner/mode, dev translated) |
| 153/154 | asioread/asiowrite(fd, buf, n, off, asiostat*) | async pread/pwrite through kernel daemons; `asiostat` 10 bytes {int error, int count, char status, char notifysig}; ACTIVE then DONE + `notifysig` | Patch `vf_read`/`vf_write` (async PBRead/PBWrite, return "pending") | **keep**, synchronously: `VOP_RDWR` at `off`, store error/count, status = `ASIODONESYNC` (documented as legal), post `notifysig` (translated) if non-zero, return 0 |
| 155 | asiowait **(+)** | sleep until no async I/O outstanding | libc | **keep**: return 0 |
| 156/157 | sema_acq(sem, timeval*)/sema_rel(sem) | 2-byte user semaphore {lock, flags; SEM_WANTED 1}; acquire writes lock = 0x80; sleeps on the user address with timeout; zero timeout + contended → EWOULDBLOCK; ETIME, EINTR (remaining time copied out), EFAULT; release clears and wakes if WANTED | libc, libpaps | **keep** (small): `fubyte`/`subyte`, sleep channel (address space, uaddr), recheck loop |
| 158/159 | _memlock/_memunlock(addr, len, flags) | flags must be 0; permission by tunables (`memlock_access`, `memlock_group`); page-round, fault in, lock | none found (reachable through Patch's generic `aAUXSysCall`) | **map** → AMIX `as_ctl(MC_LOCK/MC_UNLOCK)` (as `memcntl`), same permission rule |
| 167 | atp_control(op, …) | ATP operations for Mac processes (ops 0–3); non-Mac → EINVAL | Patch `__at_*` | **stub** EINVAL until the AppleTalk shim exists ([aux-appletalk-interface.md](aux-appletalk-interface.md)), then **keep** |
| 139 | errsys | always EINVAL | — | **stub** |

Patch's `aAUXSysCall` (AUXDispatch) lets Mac applications issue arbitrary A/UX calls, so every A/UX `sysent` number should at least produce A/UX's own error behaviour (EINVAL, or SIGSYS for `nosys`).


## 7. Sockets (70–93), select (82)

### 7.1 How AMIX does sockets (from AMIX `libsocket`, `libc`, `libucb`, `ktli`, `/etc/netconfig`)

- No socket system calls. `libsocket` opens a TPI transport device (`/etc/netconfig`: `tcp` → `/dev/tcp`, `udp` → `/dev/udp`, `icmp` → `/dev/icmp`, `rawip` → `/dev/rawip`, AF_UNIX → `/dev/ticotsord` / `/dev/ticlts`), pushes **`sockmod`** only (never `timod`), and drives it with `I_STR` ioctls (`SI_GETUDATA` 0x4965, `SI_SHUTDOWN` 0x4966, `SI_LISTEN` 0x4967, `SI_SETMYNAME` 0x4968, `SI_SETPEERNAME` 0x4969, `TI_GETINFO` 0x548c, `TI_OPTMGMT` 0x548d, `TI_BIND` 0x548e, `TI_UNBIND` 0x548f; plain ioctls `TI_GETMYNAME` 0x5490, `TI_GETPEERNAME` 0x5491) and `putmsg`/`getmsg` with TPI primitives (`T_CONN_REQ` 0, `T_CONN_RES` 1, `T_EXDATA_REQ` 4, `T_BIND_REQ` 6, `T_UNBIND_REQ` 7, `T_UNITDATA_REQ` 8, `T_OPTMGMT_REQ` 9, `T_CONN_IND` 11, `T_CONN_CON` 12, `T_DISCON_IND` 13, `T_EXDATA_IND` 15, `T_ERROR_ACK` 18, `T_OK_ACK` 19, `T_UNITDATA_IND` 20).
- All socket state lives in `sockmod`; the library's per-fd cache is rebuilt from `SI_GETUDATA` (28 bytes: tidusize, addrsize, optsize, etsdusize, servtype, so_state, so_options). So a kernel reimplementation needs no hidden library state.
- `I_STR` replies > 0 carry a TLI error in the low byte (TSYSERR → errno in the next byte); ENXIO → EPIPE.
- Async I/O: `FIOASYNC` / `F_SETFL FASYNC` → `I_SETSIG` with `S_RDNORM|S_WRNORM` (+`S_RDBAND|S_BANDURG` when an owner is set); `SIOCSPGRP`/`F_SETOWN` accepted only for the caller's pid or pgrp. STREAMS then posts **SIGPOLL (AMIX 22 = A/UX SIGIO 31)** and SIGURG (21 → A/UX 30).
- `FIONBIO` is not handled by libsocket; the generic `ioctl` path sets `FNDELAY` only if the stream acks it *(unverified for sockmod)*.
- The kernel already has the building blocks: `ktli`'s `t_kopen` (`makespecvp` → `VOP_OPEN` → `falloc` a real fd → `strioctl(I_PUSH)` → `TI_GETINFO`), `tli_send`/`tli_recv` (`allocb`/`putnext`, `getq`/`strwaitq`); `os/exp` exports `strioctl`, `strputmsg`, `strgetmsg`, `strpoll`, `falloc`, `setf`, `getf`, `closef`.

### 7.2 Recommendation: in-kernel binding

Options: (1) upcall to a user-space helper running `libsocket`; (2) in-kernel binding: the personality opens the transport device, pushes `sockmod` and performs `libsocket`'s sequences itself; (3) hybrid.

**Recommended: (2) for AF_INET, staged as (3)** (AF_UNIX and access rights first return EPROTONOSUPPORT/EOPNOTSUPP, or go to a helper later).

Reason: the socket must be a real descriptor in the A/UX process. `Patch.067C` reads/writes it, `select`s it, dups it across `fork`, and depends on SIGIO/SIGURG registered **by the process that owns the stream**. With a helper, every socket and accepted connection has to be passed back with `I_SENDFD`/`I_RECVFD`, blocking calls (connect, accept, recv) have to be proxied, and EINTR/restart/nonblocking semantics and `I_SETSIG` ownership break across the process boundary. In the kernel, once the fd exists, read/write/close/dup/fork/poll/SIGPOLL are all native; the socket-specific part is about a dozen short sequences (≈1–1.5k lines).

The choice between (1) and (2) is open; the per-call sequences below assume (2).

### 7.3 Per-call sequences (option 2)

`KSTR(fp, cmd, buf, len)` = `I_STR` through `strioctl`/`strdoioctl` with a kernel buffer, decoding the reply as `libsocket` does. Refresh with `SI_GETUDATA` after state changes; hold no locks across sleeps. A/UX args come in a0, d1, a1, d2, a2, d3.

| A/UX | Call | Kernel sequence |
|---|---|---|
| 92 | socket(af, type, proto) | **A/UX SOCK_STREAM 1 / SOCK_DGRAM 2 are swapped on AMIX** (§5.10); pick device by (af, type): INET/STREAM → tcp, INET/DGRAM → udp, INET/RAW → rawip (icmp for `IPPROTO_ICMP`); open like `t_kopen` → new fd; `I_PUSH "sockmod"`, `I_SETCLTIME 0`, `I_SWROPT SNDZERO`; `KSTR SI_GETUDATA`; raw: `TI_OPTMGMT SO_PROTOTYPE` (0x1009) = proto. Return fd |
| 71 | bind(s, addr, len) | copyin sockaddr (A/UX has no `sa_len`, same as AMIX — see §5); `KSTR TI_BIND` with `T_BIND_REQ {ADDR_len, ADDR_off=16, CONIND=0}` + address. Already bound → EINVAL |
| 78 | listen(s, n) | CLTS → EOPNOTSUPP; unbound → bind a 2-byte wildcard (family only); `KSTR SI_LISTEN` with `T_BIND_REQ {CONIND=n}` |
| 72 | connect(s, addr, len) | AF_INET: len ≥ 16, clear `sin_zero`; bind null if unbound; `T_CONN_REQ {DEST}` down (`strputmsg` with kernel ctl, or `allocb`/`putnext`); wait for `T_OK_ACK`/`T_ERROR_ACK`; CLTS → done (sockmod records the peer); nonblocking fd → EINPROGRESS; else `strgetmsg` for `T_CONN_CON` (ok) or `T_DISCON_IND` (errno = reason; ENXIO → ECONNREFUSED) |
| 70 | accept(s, addr, lenp) | `strgetmsg` for `T_CONN_IND` (nonblocking → EWOULDBLOCK); open the same device → new fd, push sockmod, bind null; send `T_CONN_RES {QUEUE_ptr = new stream's read queue, SEQ}` on the listener (the `I_FDINSERT` equivalent); wait `T_OK_ACK`; copy out the address from the `T_CONN_IND`; inherit nonblocking/async flags and the `I_SETSIG` registration |
| 83, 85, 84 | send, sendto, sendmsg | shut down for writing → post SIGPIPE, EPIPE. Connected COTS, no flags → `VOP_WRITE` with the user uio. Address given or CLTS → `T_UNITDATA_REQ` ctl + data (≤ tidusize else EMSGSIZE) via `strputmsg`. `MSG_OOB` → `T_EXDATA_REQ`. `MSG_DONTROUTE` → temporary `SO_DONTROUTE` |
| 79, 80, 81 | recv, recvfrom, recvmsg | COTS, no flags → `VOP_READ`; otherwise `strgetmsg`, parse `T_UNITDATA_IND` (from-address), `T_EXDATA_IND` (OOB), `T_DATA_IND`, loop on MORE; `MSG_PEEK` → `I_PEEK`. `msghdr` access rights: unsupported at first |
| 75, 76 | getpeername, getsockname | `TI_GETPEERNAME`/`TI_GETMYNAME` with a kernel `netbuf` (maxlen = addrsize); ENOTCONN when no peer |
| 77, 90 | getsockopt, setsockopt | `SO_TYPE` (0x1008) answered from servtype; others `KSTR TI_OPTMGMT` with `T_OPTMGMT_REQ {T_CHECK 8 / T_NEGOTIATE 4}` + `opthdr {level, name, len}` + value. Level/option numbers per §5 |
| 91 | shutdown(s, how) | `KSTR SI_SHUTDOWN(how)` |
| 93 | socketpair | AF_UNIX only: EOPNOTSUPP at first; later `ticotsord` pair as `libsocket` does |
| 82 | select(n, r, w, e, tv) | A/UX `fd_set`s → a kernel pollfd array (read → `POLLRDNORM`, write → `POLLOUT`, except → `POLLRDBAND`); call the `poll` internals; timeout: NULL → infinite, else ms = sec×1000 + usec/1000 (EINVAL if usec ≥ 10⁶); results back into the sets (`POLLNVAL` → EBADF; `POLLHUP`/`POLLERR` → readable); return count. Restartable per §1 |

Socket ioctls (in the ioctl translator, for fds with `sockmod` on the stream): `FIONBIO` → set/clear `FNONBLOCK` in `f_flag` directly; `FIOASYNC` → `I_SETSIG S_RDNORM|S_WRNORM` (+ band bits if an owner is set) or 0; `SIOCSPGRP`/`F_SETOWN` → accept the caller (or its pgrp), `I_SETSIG` with `S_RDBAND|S_BANDURG`; `SIOCGPGRP` → caller; `FIONREAD` → `I_NREAD`; `SIOCATMARK` → `I_ATMARK` (`LASTMARK`). A/UX command values per §5.3.

Before the network module loads, A/UX returns ENETDOWN (70) from every socket call (`netdown`); the translator does the same when the transport devices are absent.

## 8. Open questions

- Socket strategy: in-kernel binding (recommended) vs a user-space helper upcall.
- A/UX image detection at exec (COFF magic 0x150 plus A/UX-specific markers) and shmat/mapping at VA 0 on AMIX.
- Exact A/UX restart set (the `$df` restart byte in `syscall0`/`syscall1` is set in the sleep path; not traced).
- AMIX `HZ` in the built kernel; AMIX `utssys` type 0 output; AMIX `statvfs` units; `FIONBIO` on sockmod streams; `strdoioctl`/`strputmsg` kernel-buffer conventions (from `ktli` callers).
- Whether PBCatSearch tolerates a stubbed csop; whether the Mac side writes Finder info into `%` headers when setxinfo fails.
- `SLINK_ACCESS` semantics; `O_GETCTTY`; `TIOCPKT` for CommandShell ptys; which A/UX-private tty ioctls the Serial driver needs.
- Where the virtual-IPL mask hook goes in AMIX (`issig`/`fsig` override vs `p_hold` manipulation).
