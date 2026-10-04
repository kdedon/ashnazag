# Browser image builder: implementation research

Research date: 2026-10-03.

## Direction

Use Go for the shared build planner, manifests, validation, checksums and new image-processing code. Run it as `js/wasm` in a browser worker and as a native CLI for reproducibility. Keep HTML/CSS and a small JavaScript bridge for browser files, workers and downloads. [Go documents this browser target and requires matching `wasm_exec.js` and compiler versions](https://go.dev/wiki/WebAssembly).

A browser cross compiler is technically plausible, but unnecessary for the first builder. Follow the existing design in the project plan: CI builds redistributable m68k objects; the browser combines them with the user's media using a ported m68k linker and local image tools. User-selected modules/packages normally change the installed files and configuration, not their source code.

This is a proposed execution architecture. The website scaffold does not establish that the legacy linker, filesystem tools or complete image pipeline work in browsers.

## Existing build inventory

| Area | Current implementation | Browser approach |
|---|---|---|
| Planner and UI | Previously a design in `PLAN.md` | Go shared core; HTML/CSS/JavaScript UI |
| Kernel, drivers, guests | C and m68k assembly; `kernel/build.sh`, `kernel/mac/relink-mac.sh`, `kernel/guest/build.sh` | Compile owned source in CI; consume versioned objects |
| AMIX target toolchain | GCC 2.7.2.3, binutils 2.8.1; `toolchain/src/gcc-cross-amix/README.md` | Preserve ABI and link semantics; port only required binutils first |
| Kernel relink and patches | Shell, Python, native binutils | Explicit manifest operations; WASM linker; port patches to Go incrementally |
| ELF/COFF and module metadata | Host C tools including `coff2elf`, `elf2coff`, `mkksym`, `modfix` | Emscripten adapters or verified Go ports |
| Unix root and partitions | Python `mkufs.py`, `mks5fs.py`, `fstree.py`, `addparts.py` under `kernel/mac/diskroot` | Go ports tested against existing tools; temporary Python runtime only if justified |
| HFS and Mac metadata | hfsutils 3.2.6, C `images/hfsfork.c`, Python resource/AppleDouble tools | Port libhfs/tools to WASM; preserve forks, Finder metadata and blessing |
| X11/packages | Shell, C, Python; `x11/build.sh`, `x11/mkpkg.py`, `x11/mkimage.sh` | Prebuild eligible payloads; install selected datastreams locally |
| Mac environment | `images/macenv/mkmacimage.sh`, System-version extraction scripts | Local user media plus prebuilt owned programs |

The target compiler's README reports working C, assembler and linker, with incomplete C++. Do not promise C++ packages. Go's supported target list does not include m68k: use Go for the builder, retain C/assembly for the OS. [Go target list](https://go.dev/doc/install/source#environment).

## How the languages fit together

The browser hosts separate Go and Emscripten WASM instances. They exchange manifest records and byte buffers through JavaScript; they are not linked into one WASM executable. Start with sequential worker operations and explicit file inputs/outputs. Avoid whole-image copies between runtimes.

Emscripten supports compiling C/C++ projects through `emconfigure`/`emmake`, but the existing native executables cannot run unchanged. Configure probes, host-generated tools, filesystem calls and executable spawning need review. [Emscripten build documentation](https://emscripten.org/docs/compiling/Building-Projects.html).

For binutils, distinguish the host (WASM) from the output target (m68k AMIX). Porting `ld` does not turn the output kernel into WASM. Start with the exact legacy behavior; replacing it with modern binutils requires equivalence tests. GNU ld supports relocatable linking, which the current scripts use extensively. [GNU ld options](https://sourceware.org/binutils/docs/ld/Options.html).

A full browser compiler would additionally need the GCC frontend, assembler, headers, startup objects and libraries, plus orchestration replacing subprocesses. These include user-supplied AMIX files. Defer that substantial port until users need arbitrary source builds. Configuration tables generated as C must become prebuilt variants or validated data patches before compiler-free assembly works.

## Minimum kernel and device selection

The kernel must retain startup, CPU/MMU, interrupt/timer support, memory/process core, DLM loader/symbol resolution, and everything needed to reach the first module store. For a disk root this includes the boot storage path and root filesystem; a RAM-root design may move more drivers into early-loaded modules, after proving the handoff.

Select devices by machine capability and dependency closure. Mark entries separately as boot-required, currently static, module-ready, or planned. Enforce CPU/platform compatibility, kernel ABI identity and dependency order. Store registration records with installed modules; DLM loading alone does not configure device switch slots.

Current evidence: `kernel/mac/relink-mac.sh` links SCC, SCSI, ADB, SONIC, RTC, framebuffer/display and guest core objects into the kernel. DLM support does **not** mean these drivers are all removable today. SONIC and other optional hardware are candidates for conversion, not browser toggles that already shrink the kernel. See [DLM specification](dlm-spec.md).

Start with the Quadra 800 path. Atari has a separate relink path in `kernel/build.sh`; Amiga model selections need their own artifact and boot validation before being presented as buildable. Guest “containers” mean the repository's Mac/TOS/Amiga guest profiles, not Docker images.

## Storage, hosting and media

Keep selected media local; download only public tools and approved artifacts. Publish an allowlisted output directory, never repository build trees. Existing image documentation identifies Apple-containing test images as local-only; the toolchain README excludes AMIX headers, libraries and startup files from redistribution.

GitHub Pages serves the static app. Its published site limit is 1 GB, with a soft bandwidth limit of 100 GB/month; use small, versioned downloads and account for tool/package size before choosing artifact hosting. [Pages limits](https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits).

Use a single-threaded worker baseline. Emscripten pthreads require SharedArrayBuffer and cross-origin isolation headers; verify hosting support before adding that dependency. Its default filesystem is in memory, so a large image can exhaust memory through duplicate buffers. [Pthreads requirements](https://emscripten.org/docs/porting/pthreads.html), [filesystem model](https://emscripten.org/docs/porting/files/file_systems_overview.html).

Add OPFS scratch storage with quota checks and cleanup, then chunked export. OPFS is origin-private and quota-bound; synchronous access is available in workers. Provide a bounded Blob download fallback where streaming file export is unavailable. [Browser storage API](https://developer.mozilla.org/en-US/docs/Web/API/File_System_API/Origin_private_file_system).

## Delivery gates

1. **Scaffold:** family/model/variant catalog, dependency validation, internal JSON plan, per-guest media selection and honest stage status. Build and test native Go plus WASM; serve under a repository subpath.
2. **Native manifest runner:** replace hardcoded host paths and implicit shell state. `toolchain/src/gcc-cross-amix/build/env.sh` currently embeds an absolute home directory; `x11/mkimage.sh` constructs a scratch repository with symlinks and patches. Produce explicit operation inputs/outputs and artifact hashes.
3. **Linker proof:** build `ld`/`objcopy` for WASM; compare synthetic m68k links against native results, covering relocations, weak symbols, section addresses and metadata. Then privately compare a user-media kernel against `kernel/build.sh` checks.
4. **Filesystem proof:** port or wrap image tools; compare native/WASM partition maps, filesystem checks, metadata and hashes with deterministic fixtures. Measure peak memory on realistic disks.
5. **One complete target:** assemble Quadra 800 images from locally selected media; validate kernel symbols/relocations, partitions, HFS and Unix filesystems; boot-test in QEMU and on hardware before enabling “Build image.”
6. **Expand:** convert optional static drivers into DLMs, publish compatible package bundles, and qualify each additional machine/guest profile independently.

Each public artifact needs a content hash, source revision, target CPU/platform, kernel ABI, license/provenance and required media identifiers. CI needs controlled access to required sysroot inputs to build eligible objects; never package those inputs into the site. Source availability and redistribution eligibility must be established per artifact rather than inferred from a build succeeding.

## Work packages

| Task | Scope | Completion check |
|---|---|---|
| Build research | Existing toolchains, browser constraints, language boundaries | Source-backed inventory and delivery gates |
| Go core | Catalog, compatibility rules, CLI and WASM bridge | Native tests plus identical native/WASM responses |
| Website | Machine/device/package/guest controls, local media metadata, plan download | Working browser worker and selection flows |
| Integration | Static build, Pages workflow, public export rules, documentation | One command builds the CLI and served site |

Later packages stay narrow: the native manifest runner; a synthetic legacy-linker WASM proof;
filesystem fixtures and memory measurements. They converge at the complete Quadra image gate.
Driver modularization follows one driver at a time with boot and unload tests.
