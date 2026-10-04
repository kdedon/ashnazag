# Quadra console root

`QuadraRoot` adapts layered AMIX core, BSD, and terminfo entries for the Quadra console kernel. Supply the three archives in order (02, 03, 10) through `rootfs.ScanLayers`, then pass its entries and a big-endian m68k executable ELF32 kernel. Keep all readers open until `ufs.Build` completes.

The recipe removes unsupported device nodes and port monitors, installs console startup files and display devices, patches shutdown synchronization and 4 KiB swap accounting, and adds the kernel namelist. Patches reject unexpected source text or opcodes. It retains surviving hardlinks when replacing their original names. Diagnostic `dstest` is omitted.

Embedded configuration contains only the repository's first-party files. A test checks these copies against their source files when the full repository is available. Installation archives and kernels remain caller-supplied.

This recipe has fixed console startup and device selection. Guest installation, desktop sessions, animations, and custom kernel/module selection require additional recipes.
