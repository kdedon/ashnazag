# QEMU q800 test harness

QEMU 8.2.2 (`Debian 1:8.2.2+ds-0ubuntu1.18`), unpacked from the Ubuntu 24.04 `qemu-system-misc`, `qemu-system-data` and `qemu-system-common` packages (`apt-get download`, `dpkg-deb -x`) into `toolchain/qemu/`. Libraries missing on the host (libslirp, liburing, libaio, libpmem, librdmacm and their dependencies) are unpacked into `toolchain/qemu/lib` and found through `LD_LIBRARY_PATH`; the packages are kept in `toolchain/qemu/debs/`.

All runs are headless under `nice -n 19` and `timeout`, drive QEMU over QMP for screen dumps and keys, and open disks with `-snapshot`, so images are never modified.

## Files

| File | Purpose |
|---|---|
| `run-direct.sh [OUT] [STEP...]` | direct ELF boot (`-kernel`, no ROM); `KERNEL=` selects the ELF (default `kernel/build/unix-mac.elf`) |
| `run-mac.sh [--net] [--sock] [--tmo S] ROM DISK MB OUT [STEP...]` | ROM boot with the disk at SCSI ID 0; `--net` user-mode network, `--sock` serial A on `OUT/serial.sock` for `serial:` steps, `--tmo` timeout; `GDB=1` adds a gdbstub socket `OUT/gdb.sock`, `QEXTRA=` adds QEMU options, `TMO=` sets the timeout (260 s) |
| `run-rom.sh ROM [DISK] [OUT]` | minimal ROM boot, optional disk |
| `qmp.py` | QMP steps: `wait:N`, `shot:NAME`, `type:TEXT` (ADB keys), `serial:TEXT`, `hmp:CMD`, `quit` |
| `s5write-model.py` | host model of the s5 write path on 4 KB pages; checks the byte patches of `kernel/mac/patch_s5pages.py` (`-x` drops one patch group at a time) |
| `debug/` | gdbstub client (`rsp.py`, `calltrace.py`, `auxentry.py`), QMP park-and-dump (`park.py`), image patcher (`patchimg.py`), `run-gdb.sh` |
| `results/` | logs and screen dumps (local only) |

## Usage

```sh
images/qemu/run-direct.sh                                             # direct boot
images/qemu/run-mac.sh "Quadra 800.ROM" images/q800-test-small.img 8 OUT   # ROM → A/UX Startup → kernel
```

A ROM boot of `q800-test-small.img` reaches the single-user shell on the built-in screen with ADB keyboard input, at 8 and 32 MB.

## QEMU q800 limits

- With more than 32 MB (e.g. 36 MB) the ROM does not start (white 800×600 screen, stock images too).
- The `420DBFF3` (Quadra 700/900) ROM stops in early ROM code.
- The kernel prints `no fpu detected`: `chk_fpu` expects a null frame from `fsave` after restoring one, and QEMU's 68040 `fsave` always writes an idle frame (0x41000000). A real 68040 writes a null frame.
- The display sense code is 7, so the ROM describes a 1152-wide screen; the console takes its size from `CrsrPin` (0,0,480,640) when valid.
