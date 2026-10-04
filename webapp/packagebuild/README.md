# Locked package execution

`Build(ctx, binding, inputs, info, limits)` returns a relocatable SVR4 datastream,
a frozen file tree, and a validated receipt. Inputs are readers keyed by SHA-256;
the caller supplies package naming and BASEDIR through `svr4.Info`. Validate the
complete lock with `packages.ValidateLock` before building its bindings.

The executor verifies each source's exact length and SHA-256 before decoding and
again before publication. Extracted files are copied into bounded private
buffers. All output stays in memory until validation succeeds. Cancellation,
collisions, missing files, invalid links, failed patch preconditions, and planned
recipes return errors. Native and WASM use the same implementation.

Operations run in recipe order. Extraction creates its destination directory and
prefixes imported paths. File operations contain literal UTF-8 data. Patches
replace a regular file after matching its original digest. Hardlink targets are
package-root-relative; symlink targets are relative to the link's directory.
Metadata and source decoders must preserve the family contract. HFS imports
retain resource forks and Finder information as AppleDouble companions. HFS
raw MacRoman names that are not valid UTF-8 fail before publication because JSON
receipts cannot represent them losslessly. Amiga comments or nonzero protection
bits currently fail because their persistent host encoding has not been qualified; keeping those attributes only in a receipt would
lose them on installation. This restriction also applies to recipe metadata.

Receipts hash regular content, link targets, and empty content for directories.
`preserved` stays false for a fresh installation. On-target upgrade preservation
and the per-tier contents database belong to the target installer.
