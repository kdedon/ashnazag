# Native packed surfaces

Compile `nativehost_ui.c` with the nativehost and public display include
directories, then link its object into the application. It needs only libc.
`nh_surface_init` accepts a live mapping and its accessible byte count plus
`FBIOGINFO` metadata. A successful surface supports packed 1, 2, 4, 8, 16,
24, or 32-bit pixels. Keep its fields unchanged and discard it before
unmapping or changing the display mode.

`nh_surface_fill(surface, x, y, width, height, pixel)` validates the rectangle
and value before writing. It preserves neighboring pixels, row padding,
and mapping bytes outside the pixel area. Packed bits and multibyte pixels
use most-significant order. Values are native palette indices or true-color
mask values; the application manages colors through the public display API.

`nh_display_open(..., front=1)` verifies foreground ownership after acquiring
the session. It returns `EBUSY` if the requested switch did not occur. This
is an acquisition-time check: another session can become foreground later.
For capture, verify `FBIOGSTATE` at capture time and handle display notes.
A controlling console and the display service's permission policy still
apply; acquisition never grants additional rights.

t_lisa's render and publish checks exercise the packing and input paths.
