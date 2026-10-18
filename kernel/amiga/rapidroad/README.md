# rapidroad — the RapidRoad's ISP1763 for the usb module

Individual Computers' RapidRoad carries an ST-Ericsson/NXP ISP1763A, a
Hi-Speed USB host controller with its own transaction translator (full and
low speed devices work on its port), on the X-Surf-100's local expansion
port. This module drives it for the `usb` module (`kernel/usb`): a host
controller driver with the root hub, registered when the module loads.

From U-Boot's isp1760 driver (`drivers/usb/isp1760`, `usb_urb.c`), itself
Linux's; GPL-2, notices in the files. Loadable only, like everything GPL
here.

    isp1763.h    registers and fields, the chip memory layout, the access macros
    isp1763.c    the driver: PTDs, chip memory, done maps, root hub, polled submit
    rrlist.h     the list the queue heads and descriptors hang on
    rrmod.c      the module: autoconfig, the register window, ucd_register

## The card's side

The X-Surf-100 (autoconfig 4626/100) maps the chip's 16-bit registers with a
four byte spacing at two offsets in its window: 0x2800 with the bytes of
each word swapped, 0x3800 as they are. The probe reads the chip ID at
register 0x72 from both and keeps the window that returns 0x0017. Chip
memory goes through the HC_MEMORY/HC_DATA port a word at a time, the bytes
put in USB order in software; the card's longword data windows
(0xa000-0xbffc, two bus cycles per access) are not used yet, nor its
interrupt (register 0x4800, level 6): a transfer is submitted and the done
maps are polled from the caller's context, busily for the first two
milliseconds, then a tick at a time, as U-Boot does. The clockport flavour
of the RapidRoad (banked registers behind HighFive contexts) is not handled.

## Untested

Nothing emulates the RapidRoad, so this has only been compiled. What the
first run on an A4000 with the card should show, and what to send back:

1. `modadmin -l rapidroad` prints `rapidroad: ISP1763 rev NN on the
   X-Surf-100 at 0x..., registers at +0x3800` (or `+0x2800`), then the
   stack's `USB ISP 1763 HW rev. N started` and the root hub's enumeration
   (one port, high speed). "no ISP1763 (ID xxxx at +0x3800, xxxx at
   +0x2800)" means the chip did not answer: the two IDs tell whether the
   window or the chip is the problem.
2. With a stick in: `usbtool devs` lists it, `usbtool stor` its size,
   `usbtool read 0 0 1` its first block (a partition table ending 55 aa).
   "scratch test failed" or "the host controller would not start" point at
   the register access; a device that enumerates but whose bulk transfers
   time out at the data port.
3. The console output of the load and of a failing `usbtool` command is
   what to report.

Next: the longword data windows for throughput, the interrupt, a block
device over the storage units, HID.
