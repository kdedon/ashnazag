# Directory passthrough handler

`container-handler` translates AmigaDOS packets into the host directory broker's requests. It uses opaque host handles and a shared 4 KiB transfer buffer. Reads and writes split larger requests. Each request writes a doorbell in the boot ROM's space, which wakes the host broker; `timer.device` waits cover a broker without one.

Build without running guest code:

```
kernel/guest/amiga/dos/build.sh images/work/amiga-dos
```

The build checks the 32-bit ABI, both HUNK files and the extension ROM, including handler relocations. It uses the existing Linux m68k compiler, without linking Linux libraries.

Implemented packets: startup, locate/free/copy/parent lock, open/close, read/write/seek, examine/next, create directory, rename/delete, disk information, current volume, flush and filesystem query. Unsupported packets return `ERROR_ACTION_NOT_KNOWN`. FileInfoBlock names use the packet-level BSTR convention; `dos.library` converts them for callers.

The mailbox rejects a second handler process. Startup publishes a DOS volume and retains a root lock. Host volume zero is the configured root. Metadata setters, concurrent directory enumerations still need work. Guest execution and Workbench compatibility remain unverified.

## Mount after DOS starts

Copy `container-handler` to an already accessible `L:` and `CONTAINER` to `DEVS:DOSDrivers`. Run `Mount CONTAINER:` with the host broker active. The resulting `CONTAINER:` accesses the configured host directory. This requires an existing DOS boot path; it does not bootstrap itself from that directory. Mount only one instance.

## Cold boot

`container-boot.rom` is a 512 KiB extension ROM mapped read-only at `0x00f00000`. A4000 Kickstart 47.96 already scans that range for resident tags. The original Kickstart remains unchanged.

The extension's cold-start resident runs before ROM boot selection. It copies the embedded handler into allocated RAM, applies its relocations, clears the instruction cache, and registers a `MIG0` boot node with priority 127. The node has a handler segment list and a virtual ConfigDev with a diagnostic boot entry. Its startup field is zero, so the ROM selects the diagnostic path without reading disk sectors. That entry initializes `dos.library`; DOS selects the node at the head of the expansion mount list and starts our handler against the launcher-selected host directory. The handler publishes the volume `Amiga`; DOS owns the `SYS:` assignment.

The handler needs an installed AmigaOS directory containing the usual `C`, `L`, `Libs`, `Devs`, and `S` files. An ISO or ADF file is not an installed directory.

`container-boot-hook` remains available as a relocatable callable module, **not a shell command**. Its entry uses the m68k C ABI:

```
LONG register_boot(struct ConfigDev *board, BPTR handler_segments);
```

The extension invokes this hook with an allocated ConfigDev and persistent handler segments. It creates a memory-only DeviceNode and calls `AddBootNode`. Successful registration retains the node and handler segments for the guest lifetime.

## Verification

Small builds and static checks validate resident discovery, priorities, handler relocation and the diagnostic route. No emulator has run; booting Workbench is not yet runtime-verified.

`kernel/guest-amiga/test/check.sh` given the ROM path also checks the supported ROM revision.

References: [DOS packets](https://wiki.amigaos.net/wiki/AmigaDOS_Packets), [packet filesystem conventions](https://developer.amigaos3.net/sites/default/files/downloads/2024-10/Amiga_ROM_Kernel_Reference_Manual_DOS.pdf), [AddBootNode](https://developer.amigaos3.net/autodocs/expansion.library/AddBootNode.html).
