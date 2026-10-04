# System image provisioning

Canonical policies live in `etc/default/{mac,tos,amiga}` at the repository root.
The build copies them into this package for embedding; a test detects drift.
Quadra console images include all three policies automatically.

Policies use literal `KEY=value` lines, with blank lines and full-line comments
allowed. Values may contain spaces. Parse them as data: shell tools can use
`while IFS='=' read -r key value`; do not source or evaluate them. A leading `~/`
is expanded against the account's home by the target launcher. `.env` uses the
same literal convention with `id`, `name`, `profile`, `template`, and `created`.
The ID stays fixed after creation; `Shared` is reserved. Target launchers still
need to adopt this contract. TOS O/P drive mappings are provisional defaults.

`Apply` stages templates and apps under the selected family's system roots.
Ownership becomes root:root; set-id and group/other write permissions are removed.
Escaping links, host devices, conflicting payloads, and case collisions fail.
Existing identical policy files are accepted. No account environment is created.

`Stage` accepts digest-pinned relocatable SVR4 packages and freezes their contents
before staging. `kind` is `template` or `app`; `family` is `mac`, `tos`, or `amiga`;
`id` identifies the versioned destination. `sha256` and `size` identify the input.
The package BASEDIR is overridden by the policy destination. This prepares a
system image tree; it does not register an installed package in the target
`contents` database. Target installation/registration remains part of L6/L7.

Native example:

```sh
webapp/build/ashfs -archive base.cpio -provision provision.json -output root.img -size 512
```

`provision.json` contains an array of objects with those fields plus `file`,
resolved relative to the JSON file. The WASM `auxRootFilesystem` request accepts
that array as `provision`, omitting `file`; append package read callbacks after
archive callbacks and the optional kernel callback. Both paths write actual UFS
images. `test-provision-wasm.cjs` compares those images and independently extracts
the staged files and policies.

Staging currently buffers up to 256 MiB of package datastreams in aggregate.
This is a package working-memory budget, separate from the 2 GiB UFS and browser
8 GiB disk limits. Large guest disks should remain streamed disk artifacts.
Target boot, first-start environment creation and package registration need the
launcher/installer integration and emulator acceptance tests.

The `filesystem` and `quadra` worker messages also accept optional `provision`
items `{kind,family,id,sha256,file}`, where `file` is a local Blob. Workers derive
sizes and pass readers to WASM. The Quadra recipe makes the AMIX `/etc/default`
directory root-owned before installing policies. The layout UI can connect its
package selections to this interface without changing the image writer.
