# SCC tty driver

## Files

| File | Contents |
|---|---|
| `scc.h` | Z85C30 register bits, minor layout, `struct scc` |
| `scc.c` | STREAMS driver `sccinfo` (qinits `scc_rinit`/`scc_winit`), handler `sccintr` |
| `scccons.c` | `coinfo` = the SCC qinits (console selection) |
| `build.sh` | compiles both with the AMIX cross gcc, kernel flags of `relink-mac.sh` plus `-Wall`; fails on warnings, commons, extra sections |

## Design

```
 level 4 ──> p4int ──> sccintr: loop on RR3 (channel A)
                         rx:  RR1+data ──> rx ring (char | PE/FE/OE/BRK)  ──qenable(rq)
                         ext: RR0 delta: break start ──> BRK entry; break end ──> drop next NUL
                              DCD (modem minors) ──> CARR_ON / wakeup / SCF_HUP
                              CTS (hwflow minors) ──> restart output
                         tx:  tx ring ──> data port; empty ──> reset TxIP, wakeup closer,
                              qenable(wq) at low water or when a drain is waited for

 sccrsrv: ring ──> M_DATA (IGNPAR/PARMRK/INPCK, CSIZE mask), M_BREAK, M_HANGUP; canput/bufcall
 sccwput: M_DATA, M_DELAY, M_BREAK, ordered ioctls ──> putq; flush/stop/start/stopi/starti,
          TCGET*, TCXONC, TCFLSH, TIOCM*, M_IOCDATA, MC_CANONQUERY ──> immediately
 sccwsrv: M_DATA ──> tx ring; TCSET*W/F, TCSBRK, M_DELAY, M_BREAK wait for RR1 all-sent
```

- The handler only touches rings, the chip, `qenable` and `wakeup`. All message work runs in service procedures. AMIX runs queues on return to user mode and in the `swtch` idle loop (`runqueues`), so a qenable from the handler is serviced even when every process sleeps.
- All driver register access is at IPL 4 (= AMIX `spltty`/`splstr`, which also masks the SCC), so WR0 pointer sequences never interleave. Private `scc_spl`/`scc_splx` add a `"memory"` clobber, which the AMIX `inline.h` versions lack.
- Coexistence with polled `putchar`: it reads only RR0 and writes the data port. The `macconf.c` patch keeps its ready-test/write pair at IPL ≥ 4 so the driver cannot fill the transmitter in between. A printf byte that goes out while the driver is idle raises a Tx interrupt, which the handler either uses or clears with "reset TxIP".
- Line discipline stays in ldterm: the driver answers `MC_CANONQUERY` with `MC_DO_CANON`, sends `M_BREAK` upstream (AMIX `ldtermrput` handles IGNBRK/BRKINT/PARMRK for it; checked in the link-kit object), does IXOFF by sending VSTART/VSTOP on `M_STARTI`/`M_STOPI` and on ring high/low water, and honours `M_STOP`/`M_START`.
- Rates: BRG from RTxC 3.6864 MHz, x16, TC = 115200/baud − 2, B50–B38400 (SVR4.0 `CBAUD` has no higher codes). B0 drops DTR and keeps the rate.
- Channel parameters are reloaded in the full WR4…WR14 order only when rate or format change; otherwise WR3/WR5/WR15 only. No channel reset, so the console keeps working across the switch from polled to interrupt mode.
- Default termios per open: `B9600|CS8|CREAD` plus `CLOCAL` (local minors) or `HUPCL` (modem minors); iflag/oflag/lflag/c_cc as the AMIX console driver sets them, used only if no ldterm answers first.
- Transparent `TCSET*`, `TCGET*`, `TIOCM*` are handled with M_COPYIN/M_COPYOUT, so the driver also works without ldterm.

## Sources

AMIX 2.1 link kit and headers (`usr/sys/amiga/console/c0.c`, `amiga/driver/sl.c`, `amiga/ml/ttrap.s`, `master.d/kernel.c`, `usr/include/sys/*`); the relinked AMIX kernel (`oncons`, `swtch`, `ldtermrput`, `ureturn`); `docs/mac-q800-hardware.md`, `docs/amix-platform-interface.md`; NetBSD 10.1 `dev/ic/z8530reg.h`, `dev/ic/z8530sc.c`, `arch/mac68k/dev/zs.c` (register bits, RR3 dispatch, Mac clocking, CTS inversion, "never set HFC"); Zilog Z85C30 register semantics.

## Open questions

1. **CTS polarity.** `CTS_ON()` treats a clear RR0 CTS bit as asserted, following NetBSD mac68k ("CTS is wired backwards"). Confirm on hardware; affects only `+0x40` minors and TIOCM_CTS.
2. **Access recovery.** `SCC_DELAY` adds one VIA1 read per SCC access. NetBSD says Mac glue inserts the delay itself; build with `-DSCC_NODELAY` once confirmed on the Q800.
3. **Printer-port DCD** can be a clock input on Macs; don't use `+0x80` on channel B with a clocked peripheral.
4. **Break end.** The NUL that the Z85C30 leaves after a break is dropped only if it is the next character; confirm with a real break.
5. **Parity marking.** The driver does IGNPAR/PARMRK/INPCK because ldterm only sees bytes. It does not double a received `\377` under PARMRK; check whether AMIX ldterm expects the driver to.
6. **Other Amiga majors.** `cdevsw` still routes majors 4, 5, 6, 10, 21, 22, 46 … to Amiga drivers that touch Amiga chip addresses, and `oncons()` still treats the `scropen` major (10) as a console. Needs a Mac `master.d/kernel.c` or more overrides; outside this driver.
7. **Throughput.** At IPL 4 with a 3-byte receive FIFO, long `splhi` sections can overrun above 19200 (`sc_noe` counts it). Console default is 9600.
8. **Two opens waiting for carrier**: if one is interrupted by a signal before the line is open, it shuts the channel down under the other.
