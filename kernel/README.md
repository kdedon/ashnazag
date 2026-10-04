# kernel/ — Macintosh kernel (AMIX 2.1c + 68040 port + Mac layer)

Local work area. AMIX binaries, relinked kernels and ROMs stay here and are never published.

## Build

```sh
sh kernel/build.sh
```

Stages, one `PASS`/`FAIL` line each, logs in `build/logs/`:

1. **kit-2.1c** — `tools/mk-kit21c.sh` builds the 2.1p2a link kit in `kit-2.1c/` and
   `tools/relink-kit.sh` relinks it to `amix-2.1p2a/stand/unix`; sha256 must be
   `b37cb0ed…1af0`.
2. **port** — the AMIX 040/060 port's `relink-040.sh` → `amix-040-060-port/build/unix-040`;
   all byte-patch assertions pass and the relocation validator reports `TOTAL complaints: 0`.
3. **root** — with `AMIX_TAPE` set, `mac/ramdisk/mkroot.sh` rebuilds the s5 root image
   `mac/ramdisk/build/root.img`; an existing image must pass `s5check.py`. No image is
   allowed (root then comes from `BI_RAMDISK` or `root=`).
4. **mac** — `mac/relink-mac.sh` builds the SCC, SCSI and RAM-disk drivers, overlays the
   Mac platform layer on `unix-040` → `build/unix-mac` (`ld -r`) and `build/unix-mac.elf`
   (fully linked at 0x10000); the root image is linked in as `rd_image`.
5. **checks** — no unresolved symbols, validator clean, ELF is EXEC with two PT_LOAD and
   entry `mac_entry`, and `stext` no longer contains the 68030 `pflusha` (`f000 2400`).
6. **coff** — `mac/boot/mkcoff.sh` wraps the ELF as `build/unix-mac.coff` for A/UX
   Startup; `elf2coff -c` must report it ACCEPTED.

Inputs, all set up by `tools/setup.sh` (see `BUILDING.md`): the toolchains under
`toolchain/` (AMIX cross `amix/` with `src/gcc-cross-amix/build/env.sh`, `m68k-linux-gnu`
binutils and gcc in `linux/`, `m68k-elf` binutils in `bin/`), the tape link kit in
`toolchain/amix-root/usr/sys`, the patch-disk sources in `patch/payload/`, NetBSD 10.1
`syssrc.tgz` in `toolchain/dl/`, and the port clone with its `config.sh`.

## Notes

- The 2.1p2a patch disk ships kernel sources only (`c0.c`, `aen.c`, `master.d/kernel.c`),
  built with the native AMIX compiler. `mk-kit21c.sh` reproduces the resulting object sizes
  from the shipped objects (c0 and aen keep 2.1 behaviour), giving the layout the port
  patches; the port's `tools/verify-stock.sh` accepts its hash as a known base.
- Mac layer (`mac/`): entry shim taking a Linux/m68k boot record or an A/UX Startup
  hand-off (`boot/auxentry.s`), `config`, polled SCC console for printf, VIA1 60 Hz tick,
  Quadra autovector handlers (level 2 VIA2 → `ncr96intr`, level 4 → `sccintr`), and
  `pstartmac` (supervisor-only ITT0/DTT0, Mac I/O mapped VA = PA, ROM alias at 0x52800000).
- Drivers: `scc/` (`/dev/console`, major 0), `scsi/` (53C96 under AMIX's `dd`, majors
  18/40), `ramdisk/` (`bdevsw[20]`, raw 42).
- Clock and restart (`rtc/`): the root mount takes the time from the VIA RTC, `stime` sets it;
  `uadmin` restart re-enters the ROM, halt stays halted.
- Root and swap, first match wins: `root=cNd0sM` on the boot command line → SCSI target N
  slice M, swap and dump on slice 2; else a RAM-disk image → rd0 (s5), swap rd1; else the
  base defaults (`c6d0s1`, swap `c6d0s2`). `config` prints the choice.
