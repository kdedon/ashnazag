# Amiga package

Run `makeamiga` once to copy `/amiga/sys` into `~/Amiga`, then run
`startmig`. The launcher selects `~/Amiga`; without it, `/amiga/sys` is
used read-only, following the TOS convention. No directory argument is
required. `makeamiga -f` refreshes template files while retaining other files.

The package prepares the system directory from the supplied AmigaOS 3.2
floppies. A native extension ROM registers our directory handler before DOS
starts. Kickstart remains unchanged. The prepared startup launches the input
bridge and Workbench; the original vendor startup is preserved alongside it.

While Kickstart and the Startup-Sequence run, the screen shows "Starting
the Amiga environment" with the seconds elapsed and the file being read.
startmig logs its progress, each file opened and the handler's request
counts to `.startmig.log` in a writable SYS: (`~/Amiga`), otherwise to
`/tmp/startmig.<uid>.log`. After the hot key, the console shows startmig's
messages; errors (ROM, `/dev/amiga`, module, display) go there too.

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

Sound: the template carries `DEVS:AHI/container.audio`, its audio mode and
an AHI preferences default selecting it. AHI itself is user-supplied: an
AHI user archive (`ahi*.lha`, from Aminet or the AHI releases) listed in
`media/amiga/SHA256SUMS` adds `ahi.device`, the AHI prefs editor and
`AddAudioModes`.

With the archives in `media/amiga` (or `AMIGAAPPS`; empty disables),
MUI 3.9, AmiSSL 5.27 and the IBrowse 3.0a demo are added to the template as
their installers would install them: `SYS:MUI`, `SYS:AmiSSL`, `SYS:IBrowse`
and the installers' blocks in `S:User-Startup`. IBrowse needs a network stack
for remote pages; local files open now.

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
Fast RAM is 64 MB unless `-m MB` (0 to 128), `fastmb=MB` in an
environment's `.env` or `FAST_MB=MB` in `/etc/default/amiga` says
otherwise; startmig exits if it cannot reserve that much.
`--readonly` protects the selected tree. `--check` validates Kickstart without
opening a device; `--probe` exercises guest attachment without ROM execution.

Storage and input status describes remaining
filesystem limitations and target verification.
