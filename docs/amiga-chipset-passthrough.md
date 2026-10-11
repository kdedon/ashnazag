# Amiga chipset passthrough on an AMIX host (design, for review)

The Amiga environment in front owns the chipset; it gives it back on each switch. Refines guest-container-design.md §8 ("shared" grade): save and restore the custom registers and the chip RAM the guest uses at each switch, as Falcon `-P` does with the Videl and ST-RAM.

## What the guest gets, what stays AMIX's

| Part | In front | Hidden |
|---|---|---|
| Display (bitplanes, copper, sprites, colours, display window) | real, written through | registers kept in a shadow, display DMA off |
| Blitter | real, written through | idle (waited for at the switch) |
| Paula audio | real | DMA stopped, registers replayed on return |
| Chip RAM the guest uses | identity mapped (DMA uses guest addresses) | saved to fast RAM, AMIX's content back |
| INTENA, INTREQ, DMACON, ADKCON | merged: guest bits pass, AMIX's kept | AMIX's only |
| CIA-A/B, keyboard, serial, floppy, SDMAC (A3000 SCSI), Zorro interrupts | AMIX's; the replacement layer as today | same |

AMIX keeps level 2 (PORTS: SDMAC, Zorro, CIA-A) and level 6 (EXTER: CIA-B) and the serial bits, so its disks, clock and console keep running while the guest is in front. The guest keeps its replaced trackdisk, keyboard, timer and serial devices.

## Custom registers: trapped write-through

The custom page stays unmapped (one 4 KB page holds every register, so it cannot be split). The fault handler that emulates it today gets a third mode besides emulate and absorb: **forward**. Per register:

- *pass*: written to the chip and the shadow (bitplane/sprite/copper pointers, BPLCON*, DIW/DDF, colours, blitter, audio).
- *merge*: INTENA, INTREQ, DMACON, ADKCON: the guest's bits go to the chip, AMIX's bits are never cleared or set by the guest; reads return the guest's view.
- *read through*: VPOSR/VHPOSR, DMACONR, BLTDDAT, POTxDAT, JOYxDAT.
- *emulate*: SERDAT/SERPER/SERDATR (AMIX's serial), DSK* (floppy stays replaced), anything AMIX owns.

Interrupts: the host's level 3/4/5 handlers see INTREQ bits the guest enabled (VERTB, COPER, BLIT, AUDx, DSKBLK only if passed) and post them to the guest the way the replacement layer posts its own; AMIX acknowledges nothing it does not own.

Cost: one fault per write. Blitter-heavy native code is slow (as §8 says for the shared grade); copper-driven display costs nothing, the copper runs on the chip. Take-over (page mapped, guest owns INTENA) stays a separate, explicit choice.

## Chip RAM

The chipset's DMA uses guest addresses, so guest chip RAM is physical chip RAM, 1:1. AMIX keeps live blocks in chip RAM (its console screens, copper lists, the display service's planes, X on the chipset) and draws into them while the guest is in front, so the guest must never own those pages:

1. At start the environment module allocates the guest's chip RAM from AMIX's chip allocator, as much as configured; AMIX's own live blocks become holes the guest's exec sees as allocated (its chip MemHeader is built with them taken).
2. At switch-out: wait for the blitter, stop the guest's DMA, save the shadow, copy the chip pages the guest has allocated (its MemHeader's taken ranges, holes excluded) to fast RAM, free them to AMIX's allocator for the display service and other sessions.
3. At switch-in: take the same physical pages back (they must be free; else the switch waits or fails with a message), copy the guest's content in, load the shadow, restart the copper (COPJMP1), enable the guest's DMA bits.

## Display service (done)

- A session acquired with `FBA_VIDEL` on an Amiga host owns the chipset while in front (`s_chip`); one at a time, only with a provider attached.
- The provider registers with `am_passattach(fn)`; `ds_switch` calls `fn(from, to)` when either side owns the chipset, in the safe context the Videl swap uses. `am_passdetach` refuses while such a session lives.
- In front, the service does not touch the screen; with an RTG card attached it switches the card to show the chipset (the ZZ9000 captures it).

## Safe when

- The host machine matches the ROM (A3000 Kickstart on an A3000, etc.), root granted it, no other guest holds the chipset.
- The guest's chip RAM fits the configured amount; the A3000 3.2 ROM in the environment boots (it currently stops at cia.resource, see the Amiga host notes).

## Open questions for review

1. Chip RAM for the guest: a fixed size (e.g. 1 MB) or all free chip RAM at start?
2. Floppy and Paula audio: pass them too (guest's own trackdisk.device, raw MFM) or keep floppy replaced as now?
3. Hotkey back while the guest has all interrupts off: the keyboard is AMIX's CIA-A (level 2), so it always works; confirm that is enough.
