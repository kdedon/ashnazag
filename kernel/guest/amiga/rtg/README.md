# container.card

An AmigaOS P96 board driver for the `startmig` shared framebuffer. It builds
as an Amiga HUNK library with `FindCard` and `InitCard` at -30 and -36.

```sh
kernel/guest/amiga/rtg/build.sh images/work/amiga-rtg
```

The build uses the repository's m68k ELF assembler and a checked ELF-to-HUNK
converter. No Amiga compiler or P96 SDK installation is required.

## Install inside AmigaOS

The baseline is the supplied `Picasso96.lha`: Picasso96 2.0 with
`rtg.library 40.3945 (16.11.99)`. Install its runtime using the included
installer and documentation. Copy `container.card` to
`LIBS:Picasso96/container.card`. Copy the archive's
`Devs/Monitors/Picasso96` executable and `Picasso96.info` to
`DEVS:Monitors/Container` and `DEVS:Monitors/Container.info`. Set these
monitor icon tooltypes:

```text
BOARDTYPE=container
SOFTSPRITE=No
NOBLITTER=Yes
DISPLAYCHAIN=Yes
```

Start only the Container monitor for this board. Launch `Picasso96Mode`
from the supplied archive to create and attach its screen modes. The
archive's existing hardware drivers are unnecessary for this virtual board.

Set P96's `DisableAmigaBlitter` environment variable to `Yes` in both the
current and saved environment, then restart AmigaOS. With P96's environment
directories present, the Amiga Shell commands are:

```text
Echo "Yes" >ENV:Picasso96/DisableAmigaBlitter
Echo "Yes" >ENVARC:Picasso96/DisableAmigaBlitter
```

The card requests CPU drawing itself; this setting also routes native planar
operations through P96's CPU implementations.

Choose an 8-bit mode no larger than the host session's display. The driver
advertises that size to P96, supports 25.175/40/65/108 MHz nominal clocks,
and leaves mode creation to P96 and its preferences. Select the resulting
mode in AmigaOS ScreenMode preferences. For example, 640x480 at 8 bits uses
25.175 MHz where the host display is at least 640x480.

This library accesses the container's fixed shared-memory address and must
only be loaded inside `startmig`. It requires the RTG mapping supplied by
the matching launcher. P96 and AmigaOS are user-supplied dependencies.

## Behavior

- Four MiB of linear, indexed 8-bit video RAM; rows align to four bytes.
- Virtual bitmap rows up to 4096 pixels, subject to video RAM capacity.
- Palette, viewport panning, screen switching, and display blanking.
- P96's CPU drawing; direct bitmap writes are visible to the host display
  refresh.
- A hardware sprite when the host offers one: the card publishes the
  pointer's image, colours and position and the host draws it, so pointer
  moves change no screen memory. Otherwise P96's software pointer.
- A bounded delay implements the documented fallback for absent retrace
  hardware. Refresh is unsynchronized and may tear.

The driver claims one board per guest and stays resident after that claim.
BoardType remains the experimental `BT_NoBoard` value; an assigned P96 board
identifier is required before distributing it as a registered board type.

## Validation

The build checker validates HUNK records, relocations at two load addresses,
the resident record, six library vectors, and `CardBase.Name` initialization.
The driver uses the 1624-byte Picasso96 2.0 BoardInfo and ignores callback
arguments introduced after 2.0.

These checks inspect files without executing the library. Loading Picasso96,
selecting a screen, drawing, and switching screens still require guest
verification. The card does not supply an AmigaOS boot device or filesystem.

Interface source: [P96 Driver Development](https://wiki.icomp.de/wiki/P96_Driver_Development).
SDK attribution and license: [NOTICE](NOTICE).
