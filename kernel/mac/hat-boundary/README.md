# HAT table-skip boundary

`hat_chgprot` skips an absent A table by moving to the next 32 MB boundary,
and an absent B table by moving to the next 256 KB boundary. In the top band
that addition wraps to 0, and the unsigned loop test then walks addresses
outside the request. `hat-chgprot040.diff` ends the walk through the existing
cache-flush tail when the addition carries.

`tools/setup.sh` applies it to the 040 port, after `kernel/port-local.diff`
and `kernel/port-segvn.diff` (segvn checks the access against the segment's
protection when there are no per-page protections). `kernel/build.sh` then
relinks the base and the Mac kernel.

`t_mem`'s `mprotect_top_band` checks map a shared page in the top 32 MB band,
and, after faulting an alias there, in the top 256 KB band, then change its
protection. `high-shared.c` is the same check as a stand-alone program
(`./high-shared a`, `./high-shared b`); an unpatched kernel can hang in the walk.
