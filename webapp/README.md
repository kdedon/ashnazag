# Ash Nazag disk image forge

A static website with a Go planner compiled for both the host and WebAssembly.
Choose a machine, compatible devices, packages and guest environments for a disk
image. Media stay on the user's computer. The intended output is a bootable `.img`;
the internal JSON plan is an implementation detail.

The Quadra console recipe builds a complete `.img` in the browser from extracted
the AMIX 2.1 tape, a prebuilt Quadra kernel ELF, and an A/UX donor disk. The tape
may be given as its archive parts (`.tar.bz2`, `.tar.gz`, `.tar`, `.zip`), a SIMH
`.tap` image or the segment files; segments are found by size and SHA-256
(`amixtape/segments.json`, shared with `tools/amixtape.py`), and missing ones are named.
Go/WASM creates a new HFS boot volume, patches and builds the UFS root, and adds
swap and the Apple partition map. The donor supplies only its map and disk driver.
No native commands or uploads are involved in this browser path.

Choose the default Quadra devices without packages, guests, desktop or animation,
then use **Generate disk image** to open the media/build form. Other selections
remain planned. The supplied kernel is unchanged, including built-in drivers;
browser compilation and relinking remain future work.

Browser disk storage supports large output subject to quota; the memory fallback
limits each image to 256 MiB. The Quadra form allows a root up to 2 GiB and swap up
to 2 GiB, plus the boot volume. Temporary storage must hold both root and final disk.
The native `ashbuild` command remains available; see [native building](native-builder.md).

## Build and run

Requires Go 1.22 or later. No npm packages or external Go modules are needed.

```sh
sh webapp/build.sh
python3 -m http.server 8080 --bind 127.0.0.1 --directory webapp/build/site
```

Open `http://localhost:8080`. Serve over HTTP locally or HTTPS when hosted;
opening the HTML as a file will not load its worker and WASM reliably.
The build copies `wasm_exec.js` from the same Go installation as the compiler.
Set `GO=/path/to/go` to select an installation.

With Node.js 22 or later (`toolchain/node/bin` holds a local copy), `node webapp/test-wasm.cjs` runs the compiled WASM
and compares valid and invalid requests with the native CLI.
`node webapp/test-image-wasm.cjs` checks WASM disk assembly; an optional path to
a native Quadra image additionally checks byte-for-byte reconstruction. CI runs
the synthetic checks without installation media.
`node webapp/test-filesystem-wasm.cjs` compares native/WASM filesystem generation
and runs the independent UFS checker. `node webapp/test-storage.cjs` exercises
storage limits, partial writes, quota failures and cleanup with a mock browser API.
`node webapp/test-quadra-worker.cjs` executes the complete worker and real WASM
with synthetic media and filesystem-backed browser API mocks. It checks the disk
with the independent UFS checker and optional native boot/HFS tools.

The native planner accepts the same JSON requests as WASM:

```sh
printf '%s\n' '{"action":"catalog"}' | webapp/build/auxplan
printf '%s\n' '{"action":"plan","selection":{"machine":"q800","devices":["scsi53c96","scc"],"packages":[],"containers":[]}}' | webapp/build/auxplan
```

## Presets and recipes

A preset is a versioned starting selection for one machine: devices, disk
layout and defaults, all editable afterwards. Only the Quadra 800 preset builds
today; Falcon030, Falcon CT60/CT63, TT030 and Amiga 4000 are listed as planned.

Each build produces a recipe: format and forge versions, preset id and
revision, the changes from the preset, the resolved package lock, and each
input's role, size and SHA-256. It holds no file names, paths or media. The
browser offers it as a JSON download after a build, and the image carries it at
`/etc/forge/recipe.json`. Importing a recipe restores its selections; with the
same inputs the rebuild is byte-identical. Recipes in a malformed or newer
format are refused.

```sh
webapp/build/ashforge presets
webapp/build/ashforge tape amix_2.1_tape_part1.tar.bz2 amix_2.1_tape_part2.tar.bz2
webapp/build/ashforge build -preset quadra800 -tape PART1 -tape PART2 -kernel unix.elf -donor aux.img -output new.img -export recipe.json
webapp/build/ashforge build -recipe recipe.json -tape PART1 -tape PART2 -kernel unix.elf -donor aux.img -output again.img
```

`ashforge tape` lists the segments found and names the missing ones. `-tape` also
takes a `.tap` image, segment files or a directory of them. The recipe's
`amix-tape` input covers the segments used; its `parts` list each given file's
size and SHA-256 and are left out of the image's copy, so any packaging of the
same tape gives the same image. Format 1 recipes, which listed segments 02, 03
and 10 separately, are converted on import. With `-recipe`, inputs that
differ from the recorded hashes are refused; the browser builds anyway and
names the differing inputs. Add `-package kind:family:id=file` per
provisioning package.

## Layout

| Path | Purpose |
| --- | --- |
| `planner/` | Embedded catalog, compatibility checks and manifest generation |
| `forge/`, `cmd/ashforge/` | Presets, recipe export/import and the complete Quadra build |
| `cmd/auxplan/` | Native JSON command |
| `cmd/wasm/` | Browser Go bridge |
| `worker.js` | WASM initialization and planner handling outside the UI thread |
| `bootimage/`, `quadra-worker.js`, `quadra.js` | HFS boot generation and complete Quadra build |
| `diskimage/`, `image-worker.js`, `assembly.js` | Go disk assembly and prepared-image browser interface |
| `media/`, `rootfs/` | Streaming SVR4 cpio import with offset-backed payloads |
| `ufs/`, `cmd/ashfs/` | Native/WASM filesystem generation and native command |
| `storage.js`, `filesystem-worker.js`, `filesystem.js` | Disk-backed browser output and archive-to-UFS interface |
| `app.js`, `index.html`, `style.css` | Static interface |
| `build/site/` | Deployable files, including the matching Go runtime |

The catalog is a curated snapshot of native recipes, not hardware certification.
Guest containers mean the project's OS personalities, not OCI/Docker containers.

## GitHub Pages

The repository workflow tests and builds the site on changes. To publish, enable
GitHub Actions as the Pages source, then run **Ash Nazag image forge** manually
with **deploy** enabled. Deployment uploads only `webapp/build/site`, which
contains the planner, prepared-image assembler and website. No local installation media enter that directory.
Relative asset URLs support a repository subpath.

The public export allowlist includes this directory and the workflow. Regenerate
and review the public export before pushing; generated build files stay excluded.
See [hosting and artifact publication](hosting.md).

## Remaining work

See [build research and task breakdown](../docs/browser-builder-research.md).
The complete Quadra console browser recipe is implemented for a supplied kernel.
Remaining work includes kernel linking/compilation, guest/package recipes,
additional machines, and physical-hardware qualification.

## Verification

Native tests and WASM tests pass. The complete worker generated a 324 MiB image
from local media; partition, UFS, HFS and boot-block checks passed. Its kernel file
matches the native ELF flattening output. Tests also cover quota exhaustion,
write failure and temporary-file cleanup. Actual browser UI/storage execution and
physical-hardware boot are not certified by the Node worker harness. GitHub Actions
deployment remains unperformed.

## Filesystem generation

The advanced filesystem panel layers extracted SVR4 cpio archives into a
4–2048 MiB UFS image. Later archives replace matching files; unsafe directory/type
conflicts fail. Hardlinks preserve their original data when a target path is replaced.
File metadata, symlinks and devices are retained; payloads stream in bounded chunks.

The Quadra console recipe takes the AMIX 2.1 tape plus a prebuilt kernel
ELF. It applies console/device configuration, UFS mounts, shutdown changes and the
swap page-size patch. It produces a root filesystem, which still needs a compatible
HFS boot partition and disk assembly. Guest/desktop selections are not applied.

The same Go engine runs natively:

```sh
webapp/build/ashfs -archive /path/to/base.cpio -archive /path/to/overlay.cpio \
  -output /path/to/root.img -size 512

webapp/build/ashfs -recipe quadra-console \
  -archive /path/to/02 -archive /path/to/03 -archive /path/to/10 \
  -kernel /path/to/unix-mac.elf -output /path/to/quadra-root.img -size 512
```

Output must be a new file. Native generation uses sparse writes; browser generation
uses origin-private disk storage when available. Downloads remain available until
inputs change or the page closes. Temporary files are removed on reset/cancel;
abandoned files are cleaned on later visits when Web Locks are available.
Filesystem size is separate from total partitioned disk size. Larger images have
not been qualified on physical machines.

## Hardware and guest configuration

Choose a family (Macintosh, Amiga, Atari), then a model and CPU/accelerator
variant. Configure memory banks and drivers in that panel. A target ID identifies
one concrete recipe; family names only organize the interface. Falcon has separate
stock 68030 and CT60/CT63 68060 variants. Unqualified models stay planned; Mega STE
is a feasibility candidate. Numeric RAM entries record intent, not validated board
limits or an applied kernel configuration.

Guest instances have stable IDs, environment IDs and separate local media bindings.
Add the same environment repeatedly to create separate installs. Module dependencies
are deduplicated; instances and their media remain distinct. Files and drafts survive
hardware switching within the page session, but reloading clears them.

Mac environment choices follow existing recipes: System 7.0.1 from shared A/UX,
Mac OS 7.6.1 and Mac OS 8.1. EmuTOS 1.4 is the current TOS recipe. Adding another
version requires a recipe and compatibility validation before exposing it here.
A/UX media is selected once globally. A Macintosh guest on a Macintosh host defaults
to its host ROM; an explicit file override carries a possible performance penalty.
Other hosts require an appropriate ROM file when Mac guest support becomes available.
Changing an environment clears its OS media binding; choosing the host ROM clears
an override binding. Media identification and ROM compatibility checks remain pending.

Schema 2 adds `containerInstances`, `baseMedia` and `memoryMiB`. File references
are session-local opaque IDs, not content hashes. The legacy `containers` field
continues to represent shared profile dependencies.

## Startup and memory intent

Startup configuration follows the “Boot to X” and “Quiet boot” designs.
Choose X11 plus twm/OpenLook, console or xdm login, and an installed desktop or
specific guest instance as the authenticated default session. Removing a default
guest or X11 resets dependent choices. Splash/animation records the quiet-boot
preference; image integration must retain verbose override and visible fatal/fsck
prompts. xdm, xsm, desktop bundles and init/session wiring remain pending.

Per-instance RAM budgets follow `docs/guest-container-design.md` §10.2. Native
`startmac` exposes TBMEMORY; `starttos -m` sets 1–14 MiB ST-RAM. Builder budgets
are intent, not an enforced partition. Concurrent Mac instances still need uinter,
ROM and PRAM isolation. The all-running estimate uses a provisional 16 MiB host
allowance, 16 MiB for X, each guest budget, and 2 MiB for splash buffers. It warns
when RAM is exceeded or less than 8 MiB remains. These are planning assumptions,
not measurements or guarantees; DMA pools, applications, caches and swap need
runtime qualification. Do not use the estimate to enable image generation.

System 6.0.x is selectable as planned support, pending the A/UX 2.x extraction,
24-bit libmac/uinter and ROM qualification described in the project plan. Shared A/UX 2.x
or 3.x support inputs appear once beneath the guests that need them. AMIX media
remains separate. Environment-specific installation media stays with each guest.

## Guest packages and system provisioning

[ashpkg](cmd/ashpkg/README.md) and `auxPackages` resolve exact per-environment
recipes and generate relocatable SVR4 packages locally. The
[package model](packages/README.md) defines the shared installer format;
[provisioning](provision/README.md) stages templates, system apps and canonical
layout policies in actual UFS images through `ashfs` or `auxRootFilesystem`.
IBrowse remains planned until its Amiga runtime dependencies are qualified.
