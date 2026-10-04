# Portable package records

`Recipe`, `Lock`, and `Receipt` use Draft 2020-12 JSON schemas. `ValidateJSON`
checks schema structure using the embedded schemas; the typed validators add
path, compatibility, dependency, sharing, and hash checks. Unknown fields and
operations fail. All required arrays use `[]`, including empty arrays.

`Resolve` accepts a recipe catalog and requests for distinct environments within
one Unix account. Versions and variants are exact identifiers. Dependencies
precede dependents in the lock. Source artifacts deduplicate by digest;
installation bindings remain separate. `ValidateLock` checks the embedded
recipe hashes before execution. Locks pin layout policy version 1.

A recipe's `tier` is its default. Shareable recipes can be selected at any tier;
non-shareable recipes require `environment`. Different versions can coexist in
private environments. A shared tier has one version of each package per family.
The resolver warns when a more specific tier shadows another.

Operations target relative package paths. `extract` names a source ID; `file`
carries literal UTF-8 data; `directory` carries its mode; `symlink` and
`hardlink` carry safe relative targets. `patch` replaces an entire file only
when its current SHA-256 matches. Executor support is checked separately from
recipe validity. Recipes contain no executable hooks.

Metadata declarations require AppleDouble preservation for Mac payloads,
Amiga protection bits/comments, or TOS 8.3 paths. TOS path components and
case collisions are validated before execution. Source adapters and executors
must preserve declared metadata or reject the operation. Writable state is
always environment-local, even for shared applications.

The embedded IBrowse fixture is deliberately `planned`: no release, source hash,
MUI/AmiSSL versions, or installation operations are guessed. Its networking
capability is `bsdsocket.library`. Qualification requires a tested release and
dependency closure before changing its status. `RequireExecutable` and the
resolver refuse every planned recipe.
