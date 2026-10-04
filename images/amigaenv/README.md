# Amiga package

Run `makeamiga` once to copy `/amiga/sys` into `~/Amiga`, then run
`startmig`. The launcher selects `~/Amiga`; without it, `/amiga/sys` is
used read-only, following the TOS convention. No directory argument is
required. `makeamiga -f` refreshes template files while retaining other files.

The package prepares the system directory from the supplied AmigaOS 3.2
floppies. A native extension ROM registers our directory handler before DOS
starts. Kickstart remains unchanged. The prepared startup launches the input
bridge and Workbench; the original vendor startup is preserved alongside it.
**Boot and Workbench execution remain unverified.** No emulator has run.

Build a new private staging directory:

```sh
sh kernel/guest/mod/amigaguest/build.sh kernel/build/unix-mac.elf images/work/amiga-modules
sh images/amigaenv/mkamiga.sh images/work/amiga-stage AmigaOS3.2CD.iso images/work/amiga-modules/mod.d
```

The output directory must be new. The optional second argument selects the
CD; the third selects a directory containing `guestcore` and `amigaguest`.
An optional fourth argument selects a local Picasso96 archive, otherwise
`Picasso96.lha` at the repository root is used if present. Its original archive
and checksum are retained under `root/amiga/rtg/`.

`root/` holds the target installation layout. Media hashes are in
`media/manifest.json`; `/amiga/sys/.container-template.json` records each
prepared file's source. Proprietary media and the prepared OS stay local.
`kernel.sha256` identifies the kernel used to link supplied modules.

The template follows the installer disk's file selection for an English
A4000 setup. It omits physical-device mounts and CPU/MMU/ROM patching in the
active startup. This is a prepared container profile, not a completed run of
the vendor installer. The custom P96 card is copied into `Libs/Picasso96`;
Picasso96 2.0 runtime installation and screen configuration remain separate.
See [driver instructions](../../kernel/guest/amiga/rtg/README.md).

Lightweight host checks:

```sh
sh kernel/guest-amiga/test/check.sh images/work/amiga-stage/media/ROM/kicka4000.rom
```

Transfer the staging directory to the target Unix system. As root there:

```sh
sh /path/to/amiga-stage/installmig /path/to/amiga-stage
```

The installer copies the utilities, template, ROMs and modules, creates
`/dev/amiga`, and grants device/media access to the display group. It also
updates the shared `guestcore` in the existing Mac module directory when
present. Reboot after updating modules, then register as root:

```sh
/usr/sbin/amigareg /usr/lib/amiga/mod.d
```

As the intended display-group user:

```sh
makeamiga
startmig
```

The current execution path requires a 68040 and a packed 8-bit display.
`--readonly` protects the selected tree. `--check` validates Kickstart without
opening a device; `--probe` exercises guest attachment without ROM execution.
`--rom-only` omits the boot extension and directory broker for diagnostics.
`--experimental` remains an alias for the normal, still-unverified boot path.

Storage and input status describes remaining
filesystem limitations and target verification.
