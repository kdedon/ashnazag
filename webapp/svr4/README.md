# Relocatable packages

`Emit` writes one single-part SVR4 datastream. `Import` reads the same bounded,
script-free subset. Both use logical absolute `ufs.Entry` paths; package paths
are relative to `BASEDIR`. File data remains backed by the supplied `ReaderAt`,
which must remain open and unchanged until emission or filesystem generation
finishes.

Supported entries: regular files, directories, symlinks with safe relative
names, and hardlinks to regular files. Names with spaces use single-quoted pathname components, including each side
of a link. Tabs, newlines, traversal, `=`, quotes, backslashes, `$`, and backticks
fail. Device nodes, scripts, editable-file actions, unknown
classes, multipart streams, and absolute package paths fail. Import verifies
file sizes and SVR4 additive checksums. Numeric ownership and common SVR4 owner
names are accepted. Emission uses numeric ownership.

Emission sorts entries, fixes metadata timestamps from the supplied timestamp
or entry timestamps, and uses uncompressed newc archives padded to 512 bytes.
The stream contains metadata `PKG/pkginfo` and `PKG/pkgmap`, followed by a payload
archive containing `pkginfo` and `reloc/` files. Directory and link installation
is described by `pkgmap`.

`testdata/ASHtest.pkg` is a redistributable fixture containing a directory, a
six-byte text file, a hardlink, and a symlink. Native and Node-hosted Go/WASM tests
compare emitted bytes to that fixture. GNU cpio independently unpacks both
archives. Malformed input, path collisions, cancellation, and checksums are
tested.

Actual installation with target `pkgadd` and the new tier-aware `pkginst` is
pending target integration. The existing Python installer accepts absolute
packages only; it cannot validate this relocatable format. To test on target,
install the fixture with `pkgadd -d ASHtest.pkg ASHtest`, then verify the contents
under `/amiga/apps/test`, including hardlink inode equality, symlink target,
ownership, modes, and package database records. Remove the test package after
validation. Use the target tier installer against an empty account/environment
root for its corresponding test.

Path quoting follows the [SVR4.2 pkgmap reference](https://www.bitsavers.org/pdf/att/unix/System_V_Release_4.2/0130176826_System_Files_and_Devices_Reference_1992.pdf).
System Folder paths and linked paths round-trip in native and WASM tests.
Quoted-path installation still requires target acceptance testing.
