# Native host services

This C library exposes AMIX process services for native 68k environments. Its interface describes host operations. Environment drivers must supply their own independently specified translation layer.

Compile `nativehost.c` with this directory and `kernel/mac/display` on the include path. Link with the host C library.

- Initialize each `nh_display` with `nh_display_init` before opening it. Open acquires a display session, maps its packed framebuffer, and binds nonblocking keyboard and mouse descriptors. The mapping starts at device offset zero; pixels start at `mapping + info.fi_offset`. The layout and color masks remain those reported by `fbinfo`.
- Close releases input descriptors, unmaps the framebuffer, then closes its session descriptor. A failed unmap retains the mapping and framebuffer descriptor for a later close attempt. A failed open preserves its original error; inspect the object and call close again if cleanup could not unmap it. Successful close is repeatable.
- `nh_input` returns one unchanged `inev`, zero when empty, or minus one on error. Drop events pass through for the caller to resynchronize. Input descriptors must be nonblocking. Calls poll without waiting.
- `nh_clock_read` samples host elapsed ticks and their frequency. Counter wrap follows the host's `clock_t` width. Callers choose scheduling periods and convert tick deltas.
- `nh_memory` maps private, zero-filled memory at a host-selected address. Pass one for executable memory or zero for data. Release it with `munmap` using the original size. Fixed guest address placement belongs to the environment launcher.
- `nh_file_transfer` seeks a supplied descriptor and transfers up to the requested count. It retries interrupted reads and writes, handles partial transfers, and returns bytes completed. Reading EOF returns a short count. An error after progress returns that progress; the caller must handle a short result. Use a seekable descriptor without append mode, with exclusive ownership of its file position. Open, close, naming, permissions, and metadata use native libc calls.

Functions return native `errno` values. Objects and descriptors require caller serialization. These are user-space services; invoking them from a kernel profile requires a separately designed boundary.

`GROUP=native sh tests/run-qemu.sh` runs t_nativehost (memory, files, clock, display, injected input), t_nhplat, t_nativecall and t_natenv.
