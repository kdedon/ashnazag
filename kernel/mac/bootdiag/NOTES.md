# Real-hardware fixes and boot status lines

## `fixes.diff`: HWQUIRKS items

| Change | File | Why |
|---|---|---|
| CB2 masked while SCSI is polled, restored after | `ncr96.c` | where VIA2 IFR bit 3 follows the chip's INT line (the FPGA Q800's pseudo-VIA), an interrupt taken during polling returns without reading INTR and repeats forever |
| slot line held low: SONIC/VBL served, CA1 off, port A kept in `mac_slotstuck`; the tick then serves SONIC/VBL and turns CA1 back on once the card lines are high | `macintr.s` | HWQUIRKS 5; a held line gives no new edge on a real VIA and a level storm where CA1 is a level |
| levels 3, 5, 6: counted in `mac_spurious[n]`, first one printed; panic after 65536 within one tick (`mac_storm`, cleared by the tick) | `macintr.s` | HWQUIRKS 6; Linux counts them |
| `delayus(500)` after chip reset + NOP | `ncr96.c` | HWQUIRKS 9; NetBSD `ncr53c9x.c` |
| error-path bus reset settles `ncr_settle` (2 s), not 250 ms | `ncr96.c` | HWQUIRKS 8; Linux waits 3 s |
| FIFO flush before each data-out DMA | `ncr96.c` | HWQUIRKS 13; Linux `mac_esp.c` |
| VIA2 PCR CA2 and CB2 independent falling edge (`& 0x11 \| 0x22`) | `ncr96.c` | HWQUIRKS 14; Linux sets 0x22 |
| chip clock comment: 16.5 MHz | `ncr96reg.h` | HWQUIRKS 20; values kept |

The SCSI host test (`scsi/test/ncrtest.sh`, 20 scenarios) passes with these changes.

The error-path settle runs at IPL 2, so the clock loses about 2-3 s of ticks. Error path only;
a settle that doesn't block needs a reset state in the driver.

## `bootdiag.diff`, `diag.c`, `diagvec.s`: status lines (off by default)

Applies on top of `fixes.diff`. `BOOTDIAG=1 sh kernel/build.sh` builds a kernel that keeps the
bottom six text rows for status, redrawn every 6 ticks, at each boot step, on the first idle
entries, and from the SCSI and idle paths when the clock stops:

```
DIAG MmSsEXU tick:6961 idle:12250 stopret:12249 instop:1 fpu:0 opt:01
IRQ L1:40788 L2:1176 L3:0 L4:7 L5:0 L6:0 L7:0 spur:0
VIA1 T1:6962 ADB:34555 oth:0  VIA2 SCSI:1176 CA1:0 SONIC:0 VBL:0 slot:0 oth:0
SCSI irq  cmds:884 cmd:01 stat:87 intr:20 step:0 phase:7 st:5 dma:2128384
SCSI lost:0 err:0 selto:0 nodrq:0 berr:0 chunks:8314  EXC last:25 flt:2@C101720A
EXC 2:3265 3:0 4:0 5-9:0 10:0 11:0 14:0 sys:5649 trap:0 fp:0 mmu:0 hi:0
```

- Steps: `M`/`m` root mount, `S`/`s` swap, `E`/`X` first exec, `U` first system call of
  `init`; `!`/`x` = failed.
- Exceptions are counted by pointing VBR at a table of stubs that chain to the kernel's table.
- `idle` is replaced: `instop:1` with a frozen `tick` means STOP never woke.
- Boot options: `nostop` (spin instead of STOP), `scsipoll`, `nosonic`, `novbl`, `nofpu`.
  `mkdiskimage.sh` takes them through `CMDLINE="root=c0d0s1 nostop"`.

Verified on QEMU q800 with each option set; all reached `login:`.

Note: the stock FPU probe finds an FPU when FSAVE after reset gives a null frame (real 040,
FPGA core) and none under QEMU, which writes an idle frame. So hardware runs FPU context
switching that QEMU tests never cover; `nofpu` reproduces the tested path.
