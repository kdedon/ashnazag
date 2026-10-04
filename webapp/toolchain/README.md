# Browser linker feasibility

The kernel pipeline uses **two linkers**: GNU binutils 2.8.1 merges the relocatable kernel; binutils 2.43.1 produces its final ELF. See `kernel/mac/relink-mac.sh`. Both stages, plus object conversion and weakening, need browser implementations.

## Run the native baseline

From the repository root:

```sh
python3 webapp/toolchain/probe.py
sh webapp/toolchain/check-prerequisites.sh
```

The probe assembles synthetic m68k objects, merges with the legacy linker, and final-links with the modern linker. It checks absolute and PC-relative relocations, strong-over-weak symbols, section addresses, BSS, rejection of unresolved symbols, and deterministic merge output. Inputs are generated in a temporary directory; no installation media is used.

Native tests pass on the existing repository toolchains. Emscripten is unavailable in the current environment; a WASM linker has **not** been built or validated.

## Compare a WASM candidate

Compile the legacy linker as an Emscripten CommonJS factory with `-sMODULARIZE=1 -sEXPORTED_RUNTIME_METHODS=FS,callMain -sFORCE_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=0`. Preserve its accompanying `.wasm` file. Then:

```sh
python3 webapp/toolchain/probe.py --wasm-ld /absolute/path/ld.js --node /absolute/path/node
```

The runner populates MEMFS, invokes the linker, and copies its output back. The probe requires byte-identical native/WASM relocatable objects. It fails if the module, runtime, or comparison fails. This adapter is prepared for the port; it remains untested against a real linker module.

## Required port work

1. Copy binutils sources into an isolated build directory. Preserve existing patches in `toolchain/src/gcc-cross-amix/Makefile`: `ld/configure.tgt` selects `m68kelf`, `bfd/config.bfd` selects `bfd_elf32_m68k_vec`, and GAS's `md_relax_table` declaration moves after the type definition. GAS patches matter if the assembler is included; the browser can initially consume preassembled objects.
2. Configure the **host** for Emscripten while retaining the **target** `m68k-cbm-sysv4`. Refresh `config.sub`/`config.guess`; the checked-in legacy scripts predate wasm. Build only libiberty, BFD and ld first. Avoid native configure probes being mistaken for runnable WASM programs.
3. Compile host generator programs with the native compiler. Audit old implicit function declarations, pointer/integer casts, incompatible callback signatures and the diagnostic `sbrk(0)` use in `ld/ldmain.c`. Exact source patches require compiler diagnostics and parity results; none are claimed complete here.
4. Run this probe, then extend it to archives, COMMON symbols, relocation overflow and the actual kernel linker script. Port the modern final linker and object weakening/conversion separately; test real user-media kernels privately.
5. Bundle versioned `.js`/`.wasm` assets with hashes and matching GPL source/build instructions. Run in a Web Worker with explicit scratch-space limits. A compiler is unnecessary for this fixture; browser builds consume compatible precompiled objects.

Emscripten documents [cross-build integration and native build generators](https://emscripten.org/docs/compiling/Building-Projects.html) and [module initialization](https://emscripten.org/docs/api_reference/module.html). The remaining work is a host port of these tools, not changing their m68k output target.
