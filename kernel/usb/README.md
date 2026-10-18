# kernel/usb — the USB stack, as a loadable module

U-Boot's USB core and hub code as FreeMiNT carries them (GPL-2, see
`LICENSE`), with a bulk-only mass storage class driver, under the AMIX
kernel's module loader. The same files are atari-sysv-sp1's `driver-ub`,
which runs them on Atari System V; `ub.h` (the `ub_`, "USB bus", prefix) is
the AMIX side of that: printing, delays, byte order.

GPL code is never linked into a kernel image here: `usb` and the host
controller drivers that register with it are separately loaded modules
(`docs/modules.md`).

    ub.h              what the stack needs from the kernel
    usb.c hub.c       the core: enumeration, control/bulk/interrupt messages, hubs
    ucd.c udd.c       controller and class driver registration (usb_api.h)
    usb.h hub.h usb_defs.h usb_api.h
    ubstor.[ch]       mass storage, bulk-only: probe, CBW/CSW transport,
                      TEST UNIT READY, INQUIRY, READ CAPACITY, READ/WRITE 10
    usbmod.c          the module: /dev/usb (major 64), its ioctls (ubioc.h)
    usbtool.c         the user side: poll, devs, stor, read, dd, daemon

## How it runs

Everything is in process context, there is no interrupt. `usb` loads on the
first open of `/dev/usb`, or as the dependency of a host controller module;
its load initialises the stack and the storage class driver. A controller
module (`rapidroad`, `kernel/amiga/rapidroad`) registers its root hub when
it is loaded, which enumerates what is plugged in at that moment. Later
plugging and unplugging is seen when the hubs are polled: `usbtool poll`,
or `usbtool daemon` every half second from an rc script.

    modadmin -l rapidroad       # usb loads first, then the controller
    usbtool devs                # addresses, ids, strings
    usbtool stor                # the mass storage units
    usbtool read 0 0 1          # block 0 of unit 0, hex
    usbtool dd 0 2048 128 > s.bin   # 64 KB raw

The ioctls (`ubioc.h`): `UBIOC_POLL` (hub events), `UBIOC_DEVS` and
`UBIOC_STOR` (lists), `UBIOC_RD`/`UBIOC_WR` (blocks of a storage unit, up to
64 KB; writes need root), `UBIOC_STATUS`. A block device over the storage
units is the next step; so are the HID class (keyboard, mouse) and a
controller interrupt.

## Building

`kernel/mods.sh TARGET=amiga OUT drivers` builds `usb` and `rapidroad` with
the other Amiga modules (ANSI C, so without `-traditional`); the only kernel
symbols they use are already in `kernel/include/kabi.amiga`.
`kernel/amiga/test/mkhdf.sh` installs them, `usbtool` and `/dev/usb` on the
test disk. `usbtool` is a plain user program: `m68k-cbm-sysv4-gcc -O
-I kernel/usb -o usbtool kernel/usb/usbtool.c`.

## Status

The stack and the storage driver run on a real Atari TT (an ISP1160 on the
Lightning VME: a SanDisk Cruzer Fit enumerates, its blocks read through the
ioctls, 64 KB in about a second). On AMIX they are compile-tested only: no
emulator models a RapidRoad, and the first run needs a real X-Surf-100 with
the module on it. See `kernel/amiga/rapidroad/README.md`.
