# XaAES in the TOS environment

XaAES (FreeMiNT's multitasking AES, `xaaes.km`, built from FreeMiNT's
pinned source) runs in the user's TOS environment process over a MiNT
layer of ours, as A/UX runs the Mac Toolbox over Unix.  No FreeMiNT
kernel: the layer is a small kernel written here, `XAMINT.PRG`, serving
the MiNT calls and XaAES's module interface (kentry).

## Shape

```
  starttos (one Unix process)
  +-- EmuTOS: BIOS/XBIOS, IKBD, VBL queue        trap #13/#14
  +-- fVDI + ashfb: VDI, vex_* mouse vectors     trap #2 $73
  +-- XAMINT.PRG (AUTO, resident; takes exec_os)
  |     processes, scheduler, timeouts, signals  trap #1 (MiNT)
  |     kentry table, km/adi loader, cnf parser  trap #2 $C8 -> XaAES
  |     files over host calls (cartridge's trap #0), drives from its table
  +-- xaaes.km + moose_w.adi; kthreads AESSYS, aesthred, XaSYS
  +-- GEM programs and accessories: MiNT processes in the same memory
```

- Built with MiNT gcc `-mshort` against FreeMiNT's headers, so `struct
  proc`, `struct file`, `kentry`, `dos_vecs` and the GEMDOS argument
  layout are FreeMiNT's; trap #1 copies the caller's raw arguments to the
  function, as FreeMiNT does.  FreeMiNT's libkern is linked in.
- Boot: EmuTOS runs AUTO; XAMINT.PRG stays resident when
  `C:\MINT\XAAES\XAAES.KM` exists and takes `exec_os`, so the shell
  process TOS starts after AUTO becomes MiNT's root (pid 0).  It hooks
  trap #1/#2, `etv_timer` and the fault vectors, adds the `MiNT` cookie
  and calls `init_km` in a loader process (pid 1) whose environment names
  `C:\MINT\XAAES.LOG` as XaAES's boot log.
- Processes: FreeMiNT `struct proc` with its own kernel stack; a switch
  saves registers and USP there.  Switches happen when a process sleeps
  (sleep, Fselect, nap, Syield, Pwaitpid), and at a return to user mode
  after the tick asked, including the return from an MFP timer C
  interrupt of user code: a program spinning without system calls is
  switched out after two 20 ms ticks.  The root is the idle loop: due timeouts run
  there (or at a return to user mode), never inside another's sleep;
  select waiters are polled; idle is `stop`.
- Programs get a 128 KB initial TPA past their BSS, as MiNT gives, and
  more with Malloc; memory is one pool, freed per process at exit.
- AES: XaAES's trap #2 handler runs in the caller's context; VDI calls
  pass on to fVDI.  Mouse: moose on fVDI's vex vectors and the VBL
  queue.  Keyboard: `u:\dev\console` over BIOS Bconin, polled.
- Files: per-process handles over host descriptors; MiNT paths map
  through the cartridge's drive table (C: the environment, U: /);
  `u:\dev\console`, `null` and `u:\pipe\*` are in memory, other
  `u:\dev` names are the host's.  Names match as given, ignoring case,
  or by their 8.3 form.
- Signals: kernel threads' handlers are called directly; a program's
  run on its own stack and return through Psigreturn, MiNT's frame.
  Talarm/Tmalarm are timeouts posting SIGALRM.
- Pvfork: the child runs in the parent's memory, the parent sleeping
  until the child's Pexec 200 or exit.  Pfork: the child gets the
  parent's blocks at the same addresses; each process's copy is
  swapped in as it is switched to.
- Fpoll rides on Fselect's polling.  Sockets (MiNTNet's calls, AF_INET)
  are the environment's host sockets (`/dev/tos` TOSIOC_SOCK), kept
  nonblocking on the host; a blocking call waits in the layer.
- Terminals: a pipe named `u:\pipe\tty*` is a pair; its creator holds
  the master, later opens get the terminal, with echo and line editing
  unless raw.  Cconrs, Cconin's echo and BIOS console calls of a user
  process go to its handles 0 and 1.
- Console windows: `CONSOLE.PRG` (run from `XAAES.CNF`) creates
  `u:\pipe\tosrun`, where XaAES writes the TOS programs it starts.
  Each gets a window with a VT52 screen on a terminal pair.
- MiNT command-line programs (`.TTP`/`.TOS` marked MiNT in the program
  header) started by Pexec run as Unix processes: starttos's spawner
  child runs `mintrun` on a host pseudo-terminal and passes the master
  back; a proxy process in the layer copies between it and the caller's
  handles, forwards signals and ends with the program's exit status.

```
  XaAES --tosrun--> CONSOLE.PRG --Pexec--> TOS program (layer process)
                        |  ttyc master/terminal  \-> proxy == pty == mintrun (Unix)
```
- Log: `C:\MINT\XAMINT.LOG`; with `C:\MINT\XAMINT.TRC` present every
  kentry and GEMDOS call is logged too.  Unserved slots log their
  number and return ENOSYS; unserved trap #1 calls go to TOS.

## Use

`starttos -e xaaes` (guest's image has it); `starttos -e NAME -b xaaes` makes
another.  TeraDesk is XaAES's shell (`XAAES.CNF`).  Plain EmuTOS
environments are unchanged.  `XAAES=deb` at build time gives XaAES's
diagnostics in the layer's log.

## Limits

- One address space, no memory protection (as in startmac).
- Pfork's copies cost a copy of the blocks per switch between the
  processes sharing them; a kernel thread touching such a block sees
  whichever copy is in place.
- Fsocketpair and AF_UNIX are not served.
