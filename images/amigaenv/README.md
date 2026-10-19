# Amiga package

Run `startamiga`. Its first start copies `/amiga/sys` into `~/Amiga`; without
a writable home, `/amiga/sys` is used read-only.

The package prepares the system directory from the supplied AmigaOS 3.2
floppies. A native extension ROM registers our directory handler before DOS
starts. Kickstart remains unchanged. The prepared startup launches the input
bridge and Workbench; the original vendor startup is preserved alongside it.

While Kickstart and the Startup-Sequence run, the screen shows "Starting
the Amiga environment" with the seconds elapsed and the file being read.
startamiga logs its progress, each file opened and the handler's request
counts to `.startamiga.log` in a writable SYS: (`~/Amiga`), otherwise to
`/tmp/startamiga.<uid>.log`. After the hot key, the console shows startamiga's
messages; errors (ROM, `/dev/amigaguest`, module, display) go there too.

`startamiga`, `amigareg` and the drivers are the `amigaenv`
package (`images/envpkg/mkenvpkgs.sh`). Kickstart, the floppies and the
`SYS:` template come from your AmigaOS 3.2 CD, in a new private staging
directory:

```sh
sh images/amigaenv/mkamiga.sh images/work/amiga-stage AmigaOS3.2CD.iso
```

The optional second argument selects the CD. An optional third argument
selects a local Picasso96 archive or an unpacked one (the directory holding
`Picasso96Install`), otherwise `Picasso96.lha` at the repository root is used
if present. An archive and its checksum are retained under `root/amiga/rtg/`.

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

The image builders take Kickstart, the floppies and `SYS:` from the staging
directory (`AMIGASTAGE`). On a running system with `amigaenv` installed, as
root, from a copy of the staging directory:

```sh
cd amiga-stage/root && find etc/amiga/kicka*.rom amiga/media amiga/sys | cpio -pdm /
```

As the intended display-group user:

```sh
startamiga
```

The current execution path requires a 68040 or 68060 and a packed 8-bit display.
Kickstart: the A3000 or A4000 3.2 (47.96) ROM. On an Amiga with one of
them the machine's own ROM is used, otherwise `/etc/amiga/kicka4000.rom`
(or `kicka3000.rom`), or `-r FILE`.
Fast RAM is 64 MB unless `-m MB` (0 to 128), `fastmb=MB` in an
environment's `.env` or `FAST_MB=MB` in `/etc/default/amiga` says
otherwise; startamiga exits if it cannot reserve that much.
`--readonly` protects the selected tree. `--check` validates Kickstart without
opening a device; `--probe` exercises guest attachment without ROM execution.

Storage and input status describes remaining
filesystem limitations and target verification.
