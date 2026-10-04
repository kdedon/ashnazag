# Guest layout and packages: decisions and work split

This answers [filesystem-package-handover.md](filesystem-package-handover.md). The decisions below were made on 4 October 2026. The target side covers the launchers, layout, locking and the on-target installer. The builder side covers the recipe schema, resolver, adapters and image generation.

## Decisions

1. **Visible folders, one per guest family.** Environments live under `~/Mac/`, `~/TOS/` and `~/Amiga/`, not in a hidden tree. Users browse and back up guest files from Unix.
2. **Three tiers, most specific wins:**
   - environment: private to one environment;
   - account: shared by one user's environments of a family;
   - system: root-installed and read-only.
3. **No project name in paths.** Use standard SVR4 places: the existing `/mac`, `/tos` and `/amiga` trees, `/etc/default`, `/var/sadm` and `/var/spool/pkg`.
4. **The builder emits SVR4 packages.** JSON recipes, locks and receipts are builder formats. The builder resolves a recipe into a relocatable SVR4 datastream package. The target installs it with `pkgadd` for the system tier, or with our `pkginst` for the account and environment tiers. No JSON parser runs on the 68k side.
5. **No lock files.** One writable session per environment is enforced by a `fcntl` lock on the environment's metadata file, held by the running launcher; the legacy Mac `~/System Folder` has no metadata file, so its existing `.stamp` is locked instead. The kernel drops the lock when the process exits, so no stale lock is left. The guest's host-directory drives hide `.env`.

## Layout

| Tier | Mac | TOS | Amiga |
|---|---|---|---|
| System templates (root, pinned, versioned) | `/mac/sys/<template>/` | `/tos/sys/<template>/` | `/amiga/sys/<template>/` |
| System apps (root, read-only) | `/mac/apps/<pkg>/` | `/tos/apps/<pkg>/` | `/amiga/apps/<pkg>/` |
| Account shared (user) | `~/Mac/Shared/` | `~/TOS/Shared/` | `~/Amiga/Shared/` |
| Environment (user, private) | `~/Mac/<env>/` | `~/TOS/<env>/` | `~/Amiga/<env>/` |
| Environment disk images | `~/Mac/<env>/disks/` | `~/TOS/<env>/disks/` | `~/Amiga/<env>/disks/` |

- `<env>` is the environment's id: a short name chosen once at creation. It's never derived from the OS version, and `Shared` is reserved. The display name and OS profile live in `<env>/.env`.
- Metadata: `<env>/.env` holds `key=value` lines: `id`, `name`, `profile` (e.g. `amiga-3.2-a4000`), `template` (template id and version), `created`.
- Package database:
  - system tier: `/var/sadm/install/contents`, the SVR4 standard;
  - account tier: `Shared/.pkg/contents`;
  - environment tier: `<env>/.pkg/contents`.

  All three use the same `contents` format, so `pkgchk`-style checks work at every tier.
- Source cache: `/var/spool/pkg` for root, the SVR4 spool. Users may keep `~/.pkgcache/sha256/<digest>`, which is discardable.
- Layout policy: `/etc/default/mac`, `/etc/default/tos` and `/etc/default/amiga` give each tier's root and the guest mapping below. The builder reads the same files from the repository, so paths aren't hard-coded in recipes.

### Legacy paths

The current single environments become the environment named `default`. Nothing moves:
- `~/System Folder` stays the default Mac environment's System Folder;
- `~/TOS` stays the default TOS C:;
- `~/Amiga` stays the default Amiga SYS:.

Each `make*` tool gains `-e ENV`; without it, behaviour is unchanged. `make* -e ENV --import` copies a legacy tree into a new environment. Nothing is ever merged silently.

## How a guest sees the tiers

SVR4 has no union mounts. Each guest merges the tiers with its own native mechanism, which acts as the opaque container that overlaps them all:

| Guest | Mechanism | Order |
|---|---|---|
| Amiga | Multi-directory assigns made by the startup the launcher writes: `Apps:` = env Apps, `Shared:Apps`, system apps; `LIBS:`, `DEVS:`, `C:` and `FONTS:` get `ADD` entries the same way | env, account, system |
| TOS | Drive letters from the drive tables: C: env; account and system tiers on two fixed letters, chosen in `/etc/default/tos` (proposed O: account, P: system, read-only) | per drive |
| Mac | Each environment's `Applications` folder is regenerated at session start with aliases to apps in all three tiers, as the desktop database is built today. System components (extensions, control panels, fonts) stay in the environment's System Folder | env, account, system |

Rules:
- **Same package in two tiers:** the more specific tier wins, and the planner warns.
- **Writable state is always environment-local:** Preferences, `ENVARC:` and TOS config.
- **Apps that write beside their executable:** a recipe marks them `shareable: false`, and they install only at the environment tier.
- **Host permissions:** a guest reaches host directories only through these mappings, with the user's Unix permissions. The system tier is mapped read-only.
- **Metadata:**
  - Mac forks and Finder info travel as AppleDouble `%` files, as A/UX stores them on UFS.
  - Amiga protection bits and comments travel as the host-directory handler stores them.
  - TOS names must be valid 8.3 names, and case collisions fail at planning time.

## Work split

### Target side (orchestrator)

| Id | Work | Acceptance |
|---|---|---|
| L1 | `/etc/default/{mac,tos,amiga}` policy files; `.env` format | Parsed by the shell tools and by the builder fixtures |
| L2 | `makemac`, `maketos` and `makeamiga` take `-e ENV` and `--import`; legacy default unchanged | Two environments of the same OS get distinct roots |
| L3 | `startmac`, `starttos` and `startmig` take `-e ENV`; `fcntl` lock on `.env`, or on `.stamp` in the legacy `~/System Folder` | A second writable session is refused; the lock goes on exit or kill |
| L4 | Tier mapping per guest (Amiga assigns, TOS drives, Mac Applications aliases) | A guest sees apps from all three tiers, in order |
| L5 | Per-environment Mac state: PRAM and System Folder, so two Mac environments run at once | 7.1 and 8.1 run side by side with separate preferences |
| L6 | `pkginst`: relocatable packages into the account or environment tier, run as the user, with a per-tier `contents` database; root `pkgadd` for the system tier | Install, uninstall, `pkgchk`; uninstall keeps user data |
| L7 | First-login provisioning: an image stages templates and system apps, and the user's environment is made on first `start*` | No developer home directory or uid in the image |
| L8 | Suite tests for L2–L6 | Part of the QEMU suite |

### Builder side

| Id | Work | Acceptance |
|---|---|---|
| B1 | JSON Schema (Draft 2020-12) and Go types for recipe, lock and receipt, per the handover's field table, plus `shareable`, `tier` and `metadata` (AppleDouble, Amiga protection/comment, 8.3) | Schema plus semantic validation; `planned` recipes never execute |
| B2 | Resolver: per-environment dependency closure, exact versions, conflicts and cycles fail | Differing versions coexist in two environments |
| B3 | Emit relocatable SVR4 datastream packages (`pkginfo`, `pkgmap`, class `none`, no scripts, paths relative to BASEDIR) | Our `pkginst` and `pkgadd` install them; the native and WASM outputs are byte-identical |
| B4 | Source adapters: cpio (existing), SVR4, LHA, ADF, HFS; bounded and path-checked | Malformed archives can't escape the root |
| B5 | Image provisioning: stage `/{mac,tos,amiga}/sys` templates, system-tier apps and `/etc/default/*`; no user environments | Image boots; first `start*` makes the environment (L7) |
| B6 | Pilot: IBrowse, still `planned`, until Amiga networking (`bsdsocket.library`) and MUI/AmiSSL are qualified | Recipe validates and doesn't execute |

The interface between the two sides is `/etc/default/*`, `.env` and the SVR4 package. Neither side reads the other's internal records.

## Order

L1 and B1 first, since they define the shared files. Then L2, L3 and B2/B3 in parallel, then L6 against B3's packages, then L4, L5 and L7 with B5. The Amiga runtime work (insert-disk screen, host-directory boot) has to land before L4 for the Amiga can be tested.

## Still open

- TOS drive letters for the account and system tiers.
- Whether the Mac's account tier also appears as a mounted volume, in addition to the aliases.
- How `.env` records the size of disk images per environment.
- The first qualified IBrowse release.

## Builder implementation status

The image builder also owns the canonical L1 policy files under
`etc/default/`. They use literal `KEY=value` data, including spaces; consumers
must parse rather than source them. The detailed contract and integration calls
are in [provisioning](../webapp/provision/README.md).

- B1/B2: [schemas and resolver](../webapp/packages/README.md) validate recipes,
  locks and receipts, exact dependencies, per-environment versions and sharing.
- B3: [SVR4 writer/importer](../webapp/svr4/README.md) and
  [package executor](../webapp/packagebuild/README.md) emit actual datastreams.
  [ashpkg and the WASM API](../webapp/cmd/ashpkg/README.md) expose resolution and
  execution. Target `pkgadd`/tier-aware `pkginst` acceptance remains open.
- B4: bounded cpio, SVR4, LHA, ADF and classic HFS readers are implemented.
  HFS preserves resource forks/Finder data as AppleDouble. Unsupported Amiga
  comments/protection persistence fails explicitly during package execution.
- B5: native/WASM UFS generation stages policies, system templates and apps,
  with root ownership and no user environments. Boot/first-start acceptance
  depends on L7. Staging does not yet populate the target package database.
- B6: IBrowse remains a validating `planned` recipe; execution is refused until
  the release and dependency/runtime requirements are qualified.

The builder tests include native/WASM package and image byte parity, independent
UFS extraction, malformed archives, source hashes, unsafe paths and planned
recipe refusal. They do not substitute for the pending target acceptance tests.

Verified on 4 October 2026: native tests, Go/WASM package/import/provisioning tests,
all Node bridge/storage tests, and the complete Quadra worker with synthetic and
local real media. The worker builds an approximately 324 MiB image; independent UFS, HFS and
boot-block checks pass. Its policies are included automatically. The
output is generated media, excluded from publication. Browser UI execution and
target boot remain unverified.

## Machine presets and exportable build recipes (2026-10-04)

The forge offers one preset per supported computer, so a first image takes one choice plus the user's media. A preset is a versioned build recipe: machine, devices, disk layout, kernel options, guest environments and packages, each with defaults the user can change.

- Presets: base Quadra 800, base Falcon030 (no FPU, IDE, 512 MB raw), Falcon with CT60/CT63 (060), base TT030, base Amiga 4000 (040). A preset is offered only once its machine boots from a forge-built image; the others appear as planned and can't run, like `planned` packages.
- Export: the exact recipe used, with format version, forge version, preset id and revision, every user change, the resolved package lock, and each input's role, size and SHA-256. File names, paths and media contents are not included, and nothing is uploaded. One click downloads it as JSON.
- Import: loading an exported recipe restores the same selections. With the same inputs it rebuilds a byte-identical image, so a bug report needs only the recipe plus the hashes to reproduce or compare.
- The finished image carries its recipe at `/etc/forge/recipe.json`, so a recipe can be recovered from a disk.

| Id | Work | Acceptance |
|---|---|---|
| B7 | Preset format and the Quadra preset (from today's fixed recipe); planned entries for Falcon, Falcon CT60, TT030, A4000 | The Quadra preset builds the same image as today's recipe |
| B8 | Recipe export and import in UI and CLI; recipe embedded in the image | Export → import → rebuild is byte-identical; malformed or newer-format recipes are refused with a clear message |
| B9 | Falcon preset, once the forge can write the Atari disk layout (AHDI, root-sector boot, AXB loader) as `kernel/atari/mkdisk.sh` does | Forge Falcon image boots in Hatari |
