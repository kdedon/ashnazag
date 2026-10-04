# Filesystem layout and installer package handover

For the Ash Nazag layout and installer work, 4 October 2026. This is a design proposal, not an implemented filesystem contract. It builds on the current guest templates and Go image builder. No website layout or runtime paths were changed for this handover.

Recommended rule: select applications per persistent OS environment; deduplicate their source media; share installed files only when a tested package recipe permits it. Keep writable OS configuration and application state isolated. Use one versioned declarative recipe format across native Go and browser WASM, with adapters for existing distribution formats.

## Current implementation

- The Quadra browser recipe builds HFS boot, UFS root and swap from AMIX segments 02/03/10, a supplied kernel ELF and an A/UX donor disk. It does not install guest applications or compile the kernel.
- `webapp/rootfs` imports layered cpio archives; `webapp/ufs` writes UFS; `webapp/recipes` applies the fixed Quadra recipe. These are useful execution primitives, not a generic package manager.
- `kernel/mac/diskroot/fstree.py` defines archive, removal, file, directory, link and device operations. `root.manifest` uses them to assemble an installation.
- `kernel/mac/diskroot/pkg/pkginst.py` imports a restricted SVR4 datastream: one package, class `none`, absolute paths, no installation scripts. It emits filesystem entries and package database records. Preserve this compatibility rather than replacing apkg's database with an unrelated one.
- Amiga currently uses `/amiga/sys` as the root-owned template and `~/Amiga` as the writable user installation. `makeamiga` and `startmig` implement that convention. This is not yet an installation-per-environment model. See [Amiga package instructions](../images/amigaenv/README.md) and [guest volume design](guest-container-design.md#9-real-installs).

## Identities and sharing boundaries

An **environment** is a persistent OS installation, identified by an opaque ID independent of its display name. An **OS profile** describes compatibility, such as an AmigaOS release, ROM family and CPU requirements. A **session** is one running instance of an environment. A **package release** identifies app version, build variant and recipe revision; an **artifact** identifies exact bytes by digest.

Two AmigaOS 3.2 environments remain distinct installations. Two sessions of one environment refer to the same installation; until concurrent writes are qualified, the launcher should lock it against a second writable session. A cloned environment gets a new ID and private state. Neither display names nor OS version strings should be filesystem keys.

| Data | Ownership and sharing policy |
| --- | --- |
| Original archives and media | Deduplicate by digest within the authorized owner's build/storage scope; no automatic sharing across Unix users |
| OS template | Immutable, pinned to source media and recipe; reusable by compatible environments |
| Application payload | Private copy by default; immutable shared payload only after recipe validation |
| Libraries, assigns, startup files | Installed and resolved within each environment |
| Preferences, cookies, cache, keyfiles | Private to user and environment; live-session state separate where necessary |
| Documents | Explicit shared-volume choice, with declared access mode |
| Writable disk image | One writer; never attach the same image writable to independent environments |

Sharing storage does not share a running process or its RAM. Common binaries do not make separate application memory budgets disappear. Shared payloads must remain read-only; ordinary hardlinks to mutable installed files would couple updates across environments. Do not assume copy-on-write or union mounts exist on this SVR4 target.

## Proposed host layout

These paths are placeholders for agreement with the layout owner. The ownership boundaries matter more than their spelling. Resolve paths through a layout policy rather than embedding them in every recipe.

| Logical location | Candidate host path | Policy |
| --- | --- | --- |
| Runtime launchers and guest modules | Existing `/usr/lib/<family>/` conventions | Root-owned; preserve current integration |
| Validated OS templates | `/var/lib/ashnazag/templates/<template-id>/` | Root-owned, immutable; private vendor media stays access-controlled |
| Shareable installed payloads | `/var/lib/ashnazag/packages/<release-key>/` | Immutable; only packages approved for this sharing scope |
| User environment root | `~/.ashnazag/environments/<environment-id>/root/` | User-owned guest boot tree |
| User environment disks | `~/.ashnazag/environments/<environment-id>/disks/` | Private guest filesystem images |
| Environment metadata | `~/.ashnazag/environments/<environment-id>/environment.json` | OS profile, bindings, mounts and resolved package lock |
| Application state | `~/.ashnazag/environments/<environment-id>/state/<package-id>/` | Writable private state where the app can redirect it |
| Session scratch and locks | `~/.ashnazag/run/<session-id>/` | Transient; owner-only; persistent installation lock keyed by environment ID |
| Optional source cache | `~/.ashnazag/cache/sha256/<digest>` | User-private and discardable when unused |

A recipe targets logical roots such as `host-root`, `environment-root`, `app-payload`, `app-state` and `shared-documents`. The executor maps those roots to host directories, image files or guest volumes. Never expose the package store through a writable guest mount. Guest access to host directories must retain Unix permissions and stay within the granted volume roots.

For Amiga, `SYS:` resolves to the chosen environment's root. `LIBS:`, `DEVS:`, `S:`, `ENV:` and `ENVARC:` resolve within that environment unless a reviewed recipe explicitly redirects a component. A shared `Apps:` volume is optional, not an assumption that every installer or executable supports read-only operation. Apps that write beside their executable receive a private application directory until a tested redirection exists.

Mac recipes must preserve resource forks, Finder metadata and appropriate HFS semantics; Amiga recipes must preserve protection bits, comments and assigns; TOS recipes must account for drive letters and filename restrictions. A generic Unix file-copy operation is insufficient for all three. Case-folding collisions and unsupported metadata must fail during planning, not silently lose data.

## Migration from current Amiga paths

Add environment selection to `makeamiga` and `startmig` before changing their defaults. Keep existing `~/Amiga` as the legacy default when no environment is selected. Offer an explicit import/copy into a new environment; never silently merge it with another OS installation. Version `/amiga/sys` templates so refreshes do not replace an environment's pinned base or user modifications.

Image generation needs a first-login provisioning policy because the eventual Unix user may not be known at build time. Proposed approach: stage a template and environment definition in the image, then instantiate the private root for the selected user. Do not bake the developer's home directory or UID into guest recipes.

## Common installer format

Recommend UTF-8 JSON recipes validated with **JSON Schema Draft 2020-12**, plus semantic validation in shared Go code. JSON fits the current catalog and Go/WASM bridge. JSON Schema is the structural standard; the Ash Nazag recipe vocabulary is project-specific, not an existing universal installer standard. The [official specification](https://json-schema.org/draft/2020-12) provides the schema dialect to pin.

| Candidate | Use here |
| --- | --- |
| SVR4 `pkginfo`, `prototype`, `pkgmap` and datastreams | Keep as the native Unix package adapter and database compatibility layer |
| Existing line-based filesystem manifests | Retain as import/export or debugging representation; insufficient alone for guest compatibility and state ownership |
| JSON plus JSON Schema | Common recipe, compatibility, source and installation model; recommended |
| YAML or TOML | Possible authoring frontends later; normalize to one JSON model if introduced |

SVR4 packaging already describes file objects, dependencies and optional scripts. It does not directly express our cross-guest environment bindings. Its scripts also cannot simply execute in browser WASM. Import the supported declarative subset; reject other packages or mark them as requiring a separate native/guest installer. See [Oracle's packaging model](https://docs.oracle.com/cd/E19253-01/817-0406/ch1designpkg-51728/index.html).

Separate three records:

1. **Recipe:** reviewed source rules, compatibility, dependencies, operations and state policy.
2. **Resolved lock:** exact recipe revision/hash, artifact hashes, dependency variants, environment IDs and layout-policy version. No floating `latest` references.
3. **Installation receipt:** actual installed paths, metadata and hashes, package ownership, preserved configuration and verification results.

These are internal records. The browser's user-facing output remains a disk image.

## Required recipe fields

| Field group | Required meaning |
| --- | --- |
| Identity | Format version, namespaced ID, upstream version, recipe revision, build variant |
| Compatibility | Guest family, explicit OS/environment profiles, CPU/FPU requirements and runtime capabilities |
| Sources | User-supplied or fetchable artifact, format, byte length, SHA-256, provenance and redistribution policy |
| Dependencies | Package or capability, compatible versions and installation scope; resolve to exact releases in the lock |
| Destinations | Logical volume/root and relative path; no unrestricted absolute host paths |
| Operations | Typed, deterministic operations with expected input hashes or patch preconditions |
| State | Private/shared policy, writable locations, configuration preservation and migrations |
| Resources | Installed size, temporary-space estimate and incremental running-memory estimate |
| Verification | Required files/metadata, dependency checks and native/WASM fixture tests; runtime qualification separately recorded |

Keep arbitrary shell, Amiga Installer and JavaScript hooks out of the portable recipe language. Initial operations should cover extraction, directory/file creation, metadata, links, device nodes where supported, exact patches and generated configuration. Platform adapters can add typed actions such as an Amiga assign or Mac resource update. Unknown actions fail; they are not ignored. A package requiring an unsupported vendor installer remains unavailable for automated image generation.

The payload need not be repackaged into a new archive format: a recipe can reference the original `.lha`, cpio or SVR4 package. Each format needs a validated decoder with bounded output, path checks and metadata preservation. Current cpio support does not establish browser LHA support. Source hashes identify bytes, not redistribution permission; keep user keyfiles and private media out of public recipe bundles.

Dependency conflicts are resolved per environment. Sharing the source archive never forces two environments to use the same library version. Treat legacy versions as release identifiers with explicit compatibility rules; do not presume they follow semantic versioning. Reject cycles, unresolved dependencies and conflicting writes. Unlike ordered OS archive layering, unrelated packages must not silently overwrite each other's files.

Build installations in a temporary tree, verify, then publish the new image or installation version. Updates preserve user configuration; uninstall removes only owned, unchanged package files and leaves user data. Garbage collection removes shared artifacts only after all environments and rollback references release them.

## IBrowse pilot

IBrowse is a useful first Amiga application, but no working recipe is claimed. The [official downloads](https://www.ibrowse-dev.net/download.php) distinguish OS variants and identify MUI/AmiSSL dependencies. Confirm exact requirements from the chosen release's installer and documentation; the older 2.3 installation guide is not a specification for 3.x.

Prototype contract, deliberately incomplete and non-installable:

```json
{
  "formatVersion": 1,
  "id": "amiga.ibrowse",
  "status": "planned",
  "upstreamVersion": "UNRESOLVED",
  "recipeRevision": 1,
  "target": {"family": "amiga", "environmentProfiles": []},
  "scope": "environment",
  "sharing": {"sourceArtifacts": "owner-digest-cache", "installedPayload": "private-copy"},
  "sources": [],
  "dependencies": [],
  "operations": [],
  "state": {"scope": "user-environment", "preserveOnUpgrade": true}
}
```

An empty compatibility list supports no environments. `planned` recipes must never execute. Before making this installable: pin one IBrowse variant and its media hash; inspect its installer; define the MUI/AmiSSL closure and networking capability; record real writes, assigns and state locations; supply operations and tests. Until guest networking and runtime behavior are qualified, an installed browser is not evidence that browsing works.

First use private application copies. Then test two environments with different library versions, independent preferences/cookies, concurrent launches and upgrades. Only enable a shared read-only payload after confirming the executable never needs to modify it and all writable state can be isolated. License-key placement follows the selected release's documented behavior and stays private.

## Handover work and acceptance

The layout owner should reserve stable environment IDs, define the logical roots and mount mapping, choose first-user provisioning and establish writable-environment locking. UI can place Applications under each environment and explain automatically reused media; it should not expose unsupported apps as installable.

The package implementation should add the JSON Schema and Go types, resolver/lock model, ownership database, portable operation executor and format adapters. Reuse `rootfs`, `ufs` and the fixed recipe's exact-patch checks. Keep the current Quadra recipe working while migrating it incrementally to the common model.

Acceptance cases: duplicate OS versions get distinct roots; identical sources deduplicate without sharing state; differing dependency versions coexist; package conflicts fail before image publication; modified input patches fail; malformed archives cannot escape roots; forks/protection metadata survive; uninstall preserves user data; interrupted builds publish no partial installation; native and WASM fixtures produce equivalent trees. Sharing also needs a two-session write-isolation test.

Open decisions: final path names, whether shared payloads are per-user or administrator-installed, initial Mac/TOS metadata adapters, native post-install support, and the first qualified IBrowse release. None requires redesigning the website layout in this handover.
