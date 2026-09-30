# Clock, restart and swap size (Quadra 800)

1. **Restart.** `init 6`, `shutdown -i6` and `uadmin 2 1` restart the machine through the ROM;
   halt halts, with its message on the screen.
2. **Clock.** The root mount takes the time from the RTC (VIA1 PB0–2); `stime` (`date`) writes
   it back. Local time and GMT are handled as A/UX does.
3. **Swap size.** The disk root's `swap` binary is patched to count 4 KB pages.

## Files

| File | Role |
|---|---|
| `rtc.c` | RTC and XPRAM access, `clkset` (boot time), `stime` wrapper, `mdboot`, `mac_reboot` |
| `build.sh` | `rtc.o` with the AMIX cross compiler (fails on warnings, commons, stray sections) |
| `verify.sh` | static checks of a built kernel, then `test/run.sh` |
| `test/rtctest.c`, `rtcsim.h`, `run.sh` | host build of `rtc.c` against a model of the RTC on port B |
| `0*.diff` | the changes to other files listed below (applied) |

## Integration

| File | Change |
|---|---|
| `mac/relink-mac.sh` | builds and links `rtc/rtc.o`; `OVR` += `clkset stime mdboot`; `ALIAS` += `stime:T` (base body as `__amix_stime`) |
| `mac/macentry.s` | `mac_restart`; it and `mac_stop` share `Lmmuoff`, which also sets the reset vectors |
| `mac/macconf.c` | `haltsys(1)` (panic, via `rtnfirm`) says "The system is halted." |
| `mac/diskroot/mkdiskroot.sh` | patches `/usr/sbin/swap` (checks the original bytes) |
| `mac/diskroot/root.manifest`, `etc/setclk` | patched `swap`; `setclk` is `exit 0` |

The build reports 51 overrides bound (48 + `clkset stime mdboot`).

## 1. Restart

**How the machine restarts.** A/UX's `doboot` (0x55c8e) sets IPL 7 and, on machine IDs ≥ 9,
jumps to ROM + $0A (0x4080000A) with the MMU still on. In the Quadra 800 ROM header, $0A, $0E
and the reset entry ($2A, the target of the header's reset PC) all branch to $8C, the reset
code: IPL 7, caches and TC off, DTT0/DTT1 set, `reset`, VBR into the ROM, then the full start-up
(memory, video, SCSI boot). ITT0/ITT1 are not touched there, so they must be clear already.

**Ours.** `uadmin(A_SHUTDOWN or A_REBOOT, fcn)` calls `mdboot(fcn)` in `rtc.c`:

```
mdboot(fcn): dhalt()
  AD_HALT (0)           -> haltsys(0): "The system is halted; you may turn off power."
  AD_BOOT, AD_IBOOT, .. -> mac_reboot(): "Restarting the system."
                            IPL 7, VIA1/VIA2 IER = 0x7F, SONIC IMR = 0 and CR = RST,
                            53C96 chip reset, SCSI bus reset
                            mac_restart: physical stack, Lmmuoff, jmp through (4)
Lmmuoff: cpusha, TC/ITT0/ITT1/DTT0/DTT1 = 0, pflusha, CACR = 0, cinva,
         (0) = ROM long 0, (4) = ROM + $0A
```

- The SONIC masters the bus, so it is reset before the ROM reuses RAM, without relying on the
  ROM's `reset` instruction. The ROM re-initialises the VIAs, SCC, SCSI and video.
- The SCSI bus is reset because the driver grants disconnect: a target disconnected mid-command
  (`uadmin` without a sync) would otherwise reselect into the ROM's SCSI code.
- `cinva` after the caches are off: no line fetched since `cpusha` survives into the ROM.
- The vectors at 0/4 serve an external reset that loads SP/PC from RAM 0/4, as QEMU's q800
  does. On the machine the reset overlays the ROM at 0, so the write is harmless.
- `rtnfirm` (panic) halts, so a panic message stays on the screen.
- In a direct `-kernel` boot under QEMU, the stand-in ROM at 0x40800000 has a power-off routine
  at $0A, so a restart ends QEMU.

## 2. Clock

**Hardware** (VIA1 port B; Guide to the Macintosh Family Hardware, RTC chapter): PB0 data, PB1
clock, PB2 enable (low = selected). Bytes go MSB first; the RTC takes a bit on the rising clock
edge and drives the next one after the falling edge.

| Command | Bytes |
|---|---|
| read seconds byte *n* (0 = LSB) | `0x81 \| n<<2`, then read |
| write seconds byte *n* | `0x01 \| n<<2`, data |
| write-protect | `0x35`, `0x55` off / `0xD5` on |
| XPRAM byte *a* | `0xB8 \| a>>5` (read) or `0x38 \| a>>5` (write), `(a & 0x1F) << 2`, then data |

Each transaction runs at IPL 7 (the ADB driver rewrites port B from its IPL 4 interrupt) and
changes only PB0–2 and their directions: 17 to 28 VIA accesses per byte, about 100 µs per
transaction.

**Time zone, as A/UX.** A/UX's `init_time`/`Mac2UnixTime` read XPRAM 0x0C; if it holds 'NuMc',
XPRAM 0xEC–0xEF is the Map control panel's setting: low 24 bits the signed offset east of GMT in
seconds (daylight saving included), bit 31 the DST flag. Unix time = RTC − 0x7C25B080 − offset,
so the RTC runs on local time. `SetMacClock` (from `stime` via `setthetime`) writes the RTC back
the same way. The kernel does not read `/etc/TIMEZONE`; `TZ` there only sets how programs print
the time. Keep the two consistent: the image ships `TZ=GMT0`, matching a Map offset of 0 (the
ROM's default PRAM, and PRAM without 'NuMc').

**Boot.** AMIX sets the time in `clkset(fs_time)`, called by `ufs_mountroot` and `s5mountroot`
with the root's last write time. The Amiga kernel has no clock read of its own (`setclk` reads
`/dev/clock`). Our `clkset` reads the RTC (two equal readings in a row) and uses it unless it is
earlier than the root's time (a dead battery reads 1904 or 1956) or never reads the same twice,
and prints one line:

```
mac: time YYYY-MM-DD hh:mm:ss GMT from the RTC (0 s from GMT)
```

**Setting.** `stime` (the only setter in SVR4.0; `adjtime` only slews) is weakened and wrapped:
`__amix_stime` (the base body: `suser`, `hrestime`, `time`, `rf_stime`), then the RTC is written
(write-protect off, four bytes, write-protect on), read back and retried once; if it still
differs, "mac: the RTC did not take the new time". The disk root's `/usr/amiga/bin/setclk` is
`exit 0`, since `date` runs `setclk -s`, which fails without `/dev/clock`.

## 3. Swap size

The kernel uses the whole slice. For the 65,536-sector `c0d0s2`:

- `mac_diskprobe` sets `swapfile.bo_size` = 65,536 (`sddevsize` = the slice length in 512-byte
  blocks); `swapconf` calls `swapadd(vp, 0, 65536, name)`.
- `swapadd` limits the end to `va_size` of the device (`commonvp`: `d_size` << 9 = 32 MB) and
  makes (end − start + 4095) >> 12 = 8,192 pages of 4 KB (`si_npgs`).
- `swapctl(SC_LIST)` returns `ste_pages` = 8,192. AMIX's `/usr/sbin/swap -l` prints pages << 2
  (2 KB pages built in); `swap -s` does pages << 11 >> 10 << 1, also half. `swap` does not ask
  `sysconf(_SC_PAGESIZE)` (4,096 here).

`mkdiskroot.sh` patches the tape's `swap` at build time: the two `asl.l #2,%d1` at 0x1216 and
0x121e become `asl.l #3`, the three `moveq #11,%d4` at 0x1034/0x1044/0x1054 become `moveq #12`,
after checking the original bytes. `-a`/`-d` take 512-byte blocks and are unchanged. The
RAM-disk root keeps the unpatched binary.

## Testing

- `sh kernel/mac/rtc/verify.sh`: static checks of the built kernel (overrides, alias), then the
  host model (`test/run.sh`), which covers the RTC bit protocol, reads, writes with
  write-protect, XPRAM and the Map offset.
- Under QEMU q800 with the Quadra 800 ROM and the disk from `mkdiskimage.sh`: the boot line
  above, `date` against the host's UTC, `swap -l`/`swap -s` totals of 65,536 blocks, `init 6`
  back to `login:`, and `shutdown -i0` with the halt message on screen and serial.

QEMU's VIA1 RTC ignores seconds writes and restarts from the host clock each run, so there a
`date` set prints "the RTC did not take the new time" and reverts at the next boot; the write
path is covered by the host model. QEMU keeps no PRAM between runs unless given a PRAM drive,
so the Map offset there is 0, and its `reset` instruction does nothing, so the kernel quiets
the VIAs and the SONIC itself.

## Open

- Check on a real Quadra 800: RTC write, restart, and the XPRAM Map offset path.
- `adjtime` does not reach the RTC (only `stime` does).
- `xpram_read`/`xpram_write` are exported for a later `/dev/nvram`; nothing uses them yet.
- Soft power-off (VIA2 PB2) is not wired to any `uadmin` function.

## Sources

The shipped A/UX 3.1 kernel (`doboot`, `init_time`, `Mac2UnixTime`, `Unix2MacTime`,
`SetMacClock`, `stime`), the Quadra 800 ROM (header, reset path), our kernel (`uadmin`,
`mdboot`, `stime`, `clkset` and callers, `swapadd`, `swapctl`, `spec_getattr`, `commonvp`),
AMIX's `swap`, `date` and `setclk`; the AMIX link kit's `amiga/driver/ben.c`; Apple's Guide to
the Macintosh Family Hardware for the RTC commands.
