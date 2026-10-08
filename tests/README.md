# tests/ — kernel functional tests

Small AMIX programs that exercise the kernel from user space. They run on the Quadra 800 model at boot, from the RAM-disk root. Binaries, images and results are built from AMIX material and stay local.

## Run

```sh
sh tests/run-qemu.sh            # direct boot of kernel/build/unix-mac.elf, test root as -initrd
sh tests/run-qemu.sh --rom      # Q800 ROM → A/UX Startup → kernel with the test root linked in (no A/UX files: t_aux skips)
sh tests/run-qemu.sh --net      # direct boot, network root: t_net on QEMU's user network
```

`--net` needs the tape's segment-07 tools once: `sh tests/net/getnet.sh <tape-dir>`.

Direct boot also runs `display/hostio.py`: `t_display` sends it requests on SCC channel B (`/dev/term/b`, a socket on the host) and it answers with QEMU input events, screen dumps and their comparison with the expected pattern (`results/<run>/display/`). `--net` runs `t_display` before `t_net`, so the network runs with video VBL interrupts on.

`--no-build` reuses what is in `build/`. `--kernel-dir DIR` tests a built copy of `kernel/` (its ELF, RAM-disk manifest and, for `--rom`, its relink). `KERNEL`, `MEM` (default 128, ROM 32), `TMO` (default 300 s) and `ROM` override the defaults. All runs use `nice -n 19` and `timeout`; run one at a time.

Exit status: 0 all PASS, 1 any FAIL, 2 timeout, panic or no `TESTS DONE`.

## Results

`results/<time>-<mode>/`:

| File | Contents |
|---|---|
| `serial.log` | console output: kernel boot, then every result line |
| `serial.ts` | the same lines with host time, to check the guest clock (`time.wall_mark*`) |
| `summary.txt` | pass/fail/skip per area, then each FAIL and SKIP with its reason |
| `screen.png` | screen at the end |
| `monitor.txt` | CPU registers at the end |
| `build.log` | compile and image build |

Result lines:

```
PASS file.size_2049
FAIL file.append_700_reopen: content wrong after 2800 bytes (first bad offset 2048)
SKIP arith.fp_basic: killed by signal 4; kernel reports no FPU
INFO time.clk_tck: 60
```

`=== t_x` starts a program; `--- t_x: exit …, pass … fail … skip …, … ms` ends it. `FAIL t_x.run: …` means the program itself timed out, died on a signal, or exited nonzero with no FAIL line. The run ends with `TESTS DONE pass=N fail=M skip=S`; afterwards init starts the single-user shell. A `panic` on the console stops the run.

## Areas

| Program | Covers |
|---|---|
| `t_proc` | fork/wait/exit status, 300 sequential and 25 concurrent forks, orphan reparenting, vfork, exec of `xh_*` (file ends at 16…4096 bytes into its last page, 70 KB data), exec of every root tool, argv/envp, ENOENT/EACCES/ENOEXEC, `#!`, exec of a freshly written copy |
| `t_mem` | sbrk grow/shrink/regrow, 8 MB malloc, copy-on-write of heap/bss/data/stack, file mmap shared/private/offset and EOF zero fill, `/dev/zero` private and shared, mprotect, munmap, 400 KB stack |
| `t_file` | sizes around 512/1 K/2 K/4 K/10 K (direct-block limit), in-place overwrite, 700-byte appends, 600 KB file (double indirect), seeks, F_FREESP/ftruncate, sparse files, stat fields, chmod/utime, directories past one block, rename/link/symlink/unlink, free-count restore; writeback checked through the raw device and by rereading after `msync(MS_INVALIDATE)` drops the pages |
| `t_pipe` | pipe (duplex, I_NREAD, capacity, blocking writer, EAGAIN/O_NDELAY, EOF, SIGPIPE/EPIPE), 1 MB transfers, FIFOs, dup/dup2/F_DUPFD, close-on-exec, record locks, poll/select |
| `t_sig` | kill/signal/sigaction, SA_RESETHAND, masks and pending, alarm, SIGCHLD and SIG_IGN reaping, EINTR vs SA_RESTART, stop/continue, process groups, SIGFPE/SIGILL, siglongjmp from handlers and faults |
| `t_time` | time, gettimeofday and times() against sleep(2) and a 5 s poll (tick rate), monotonic time, CPU time |
| `t_tty` | console isatty, TCGETA/TCSETA and TCGETS/tcsetattr round trips (same values written back), ldterm on the stream |
| `t_arith` | 32/64-bit multiply, divide, shifts; FP and libm in a child; FP state across context switches |
| `t_sys` | uname, sysinfo, getppid, limits, EMFILE, environment through exec, `/proc` readdir/PIOCPSINFO/PIOCSTATUS/memory read, kernel counters via `/dev/kmem` |
| `t_streams` | `/dev/null`, `/dev/zero`, sad autopush query and module validation, clone open, I_PUSH/I_LOOK/I_POP, pseudo-terminal pair |
| `t_net` (`--net` only) | DLPI on `/dev/aen0` (info, address, SNAP bind, multicast limit, bad offset), slink/ifconfig/route, ping 10.0.2.2, 64 KB TCP echo via guestfwd, refused connect, ifconfig down/up, CA1 interrupt count |
| `t_stress` | 20 s of fork + pipe + file I/O + exec + malloc; checks freemem, availsmem and filesystem counts before and after |
| `t_ufs` | a ufs volume (SCSI disk 2, from `mkufs.py`) mounted, used (read, write, chown, shared mmap, directories) and unmounted 10 times, then remounted read-only; after each unmount, s5 I/O and exec, and writes at the end of fresh file-window slots must not fault on the next slot |
| `t_page` | 4 processes, 2 generations, touch twice free memory (within what swap can reserve) and verify every page forwards, backwards and after fork (copy on write); `swapctl` free space drops while held and recovers; a holder stuck 300 s fails with its wait channel. Pages out only when swap exceeds free memory: on the RAM-disk root boot with `rdswap=MB`, e.g. `MEM=48 CMDLINE=rdswap=28` |
| `t_moddemo` | a write to a shared file mapping right after a read of the same page reaches the file after `msync`: the MMU must set the page's modified bit on that first write |
| `t_dlm` | loadable modules from `/tests/mod.d` (`dlm/`): load, dependent load calling into its dependency, `modstat`, `getksym` both ways, unload order, `moduload(0)`, the loader's errnos, `EPERM` for non-root, `modadmin`; skipped on a kernel without module support |
| `t_aux` | A/UX COFF programs as guest processes (modules built by `aux/build.sh`, programs and files from a local A/UX root `AUXROOT`, never committed): `sh` scripts and signals, `ls -l`, file tools; with shared-library support, `abi` (BSD signals, itimers, select, wait3/waitpid, locks, statfs, truncate, utimes, shm, TIOCPKT), `.lib` failure paths, `sleep` (libc1_s, SVR3 frame), `more` and `vi` on a pty; each part skips on a kernel without it |
| `t_mac` | the Mac environment (modules and files as for `t_aux`, plus `startmac`, `libmac1_s`, `Patch.067C`, the System file and a Mac ROM image `AUXROM`, all local only): `/dev/uinter0` from a native process (auto-load, version, refusal); `macabi`, an A/UX program that makes itself a Mac task (layer ioctls, low memory at 0, ui page, ROM mapping and ProductInfo, PRAM, A-line frames, privileged instructions on the virtual SR, signals held by the virtual IPL, the SIGIOT tick); `startmac` with `TBVERBOSE`/`TBWARN`: output relayed as `mac|` lines, the kernel trace of its opens and uinter commands as `ktrace|` lines, checks that it passed `doDispatch` and opened the System file after `UI_SET`; each part skips without what it needs |
| `t_mac76` | `t_mac`'s `startmac` run (`t_mac.c` built with `SYS76`) on Mac OS 7.6.1: the System Folder `/mac/sys/S761` made from the CD image (`CD761`, local only) by `images/macenv/mksys76.sh`, the model set to a Quadra 800 (`uinter_boxflag` 29); checks `'boot'` 3's `_AUXDispatch(36, 0)` (it makes `/tmp/.mac/unix`), the Finder desktop, `SysVersion` `$0761`, the model, About This Computer, SimpleText (linked into `/Desktop Folder`) opened by keyboard and quit, the disk window and the Special menu; skips without the CD |
| `t_vtop` | raw `rd0` read/write and `/proc` read/write with user buffers below 1 GB, each at the address equal to the physical address of the RAM-disk block it moves, so a kernel that takes it for a physical one only copies that block onto itself; raw SCSI read when a disk is present |
| `t_display` | `/dev/fb0` ioctls (info, modes, bounds, `EFAULT`), VBL wait, sessions, `mmap` limits, CLUT round trip, pattern and CLUT rotation checked in screen dumps, blank, keys and mouse through QEMU with rising timestamps, background session gets no input, hotkey switch with contents and key releases, a forked mapping following a switch, partial `munmap` refused, close with a mapping keeping the session, emergency key, node modes and group `display` permissions, `kill -9` of `dstest` in front returns the console screen unchanged. Host parts need `hostio.py` (direct boot); elsewhere they are skipped |
| `t_env` | guest environments: record locks on s5 and ufs released by `SIGKILL`; `maketos -e` makes distinct roots, refuses reserved and bad names and existing ones, `--import` copies `~/TOS` without its environments and leaves it unchanged; `starttos -e`, `startmig -e` (on the ufs volume) hold the lock on `.env`, and `envlock` on the legacy System Folder's `.stamp`, while running, a second session is refused, other environments and the legacy tree lock separately, and the lock goes on `SIGKILL` and on exit |

Floating point: when the kernel reports no FPU (`fpu_present` 0), FP failures are SKIPs, not FAILs.

## Add a test

1. Write `src/t_<area>.c`. Start with `t_init("<area>", watchdog_secs)`, report with `t_check(name, ok, fmt, ...)`, `t_pass`, `t_fail`, `t_skip`, `t_info`, and `return t_done();`.
2. Keep it to seconds. Run anything that may crash or hang in a child and reap it with `t_waitchild(pid, &st, secs)`. Children report through exit status, not output.
3. Add it to `tests[]` in `src/runall.c` with a timeout above its watchdog.
4. `sh tests/build.sh` compiles every `src/t_*.c`; `ONLY=src/t_x.c NOXH=1` builds one.

Tests see `/tests/ksyms` (kernel symbol addresses, for `t_kmem("freemem")` or `t_kmem("anoninfo+8")`) and scratch space in `/tmp` on the RAM root. Core dumps are off.

## Build pieces

| File | Role |
|---|---|
| `build.sh` | AMIX gcc; links `libc.so.1`, `libm.a`, libc's nonshared members; builds `xh_*` sized exec targets |
| `mktestroot.sh` | `build/testroot.img`: the normal root manifest + `/tests`, pty and clone nodes, `etc/inittab` (runs `/tests/runall` as a sysinit entry) |
| `mkromimage.sh` | for `--rom`: relinks a copy of `kernel/` with a 2 MB test root linked in, puts it as `unix.coff` on a copy of `images/q800-test-small.img` |
| `display/hostio.py` | `t_display`'s host side: QMP input and screen dumps, pattern references (the pattern of `kernel/mac/display/dspat.h`) |
| `summarize.py` | builds `summary.txt` and the exit status from `serial.log` |
| `aux/build.sh` | `t_aux`'s modules, A/UX programs, `/shlib/libc1_s`, vt100 terminfo and termcap from `AUXROOT`, and `abi` (`abi.c`, `abi0.s`, `abi.ld`, made an A/UX COFF file by `elf2aux.py`); `macabi` (`macabi.c`, `macabi0.s`) and, with the `uinter` module, the Mac environment files for `t_mac` |
| `dlm/build.sh` | `t_dlm`'s modules (`dlm/mod/`), built with the kernel tree's `dlm/tools/mkmod` against the kernel under test |

## The Windows 3.x environment

`tests/win16/` runs on the build host, not in QEMU: `sh tests/win16/run.sh` builds `startwin` with its in-memory screen, checks `startwin -install` on synthetic compressed disks and runs the Win16 test programs in `tests/win16/src` (built with Open Watcom 2, `WATCOM`) with their input scripts against `tests/win16/expect`; `sh tests/win16/x86suite.sh` runs the 80386 instruction suite. Details in `docs/win16-design.md`.
