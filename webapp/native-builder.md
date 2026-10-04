# Native Quadra image builder

`ashbuild` creates an actual raw `.img` using the existing native filesystem and
boot tools. It supports a Quadra 800 console install with UFS root and swap.
Browser assembly, kernel relinking, X11, guest installs, xdm and animation remain
separate work. Unsupported selections fail instead of being silently ignored.

## Inputs

- A prebuilt Quadra kernel ELF, produced by the existing kernel build.
- User-supplied AMIX tape segments `02`, `03` and `10` in one directory.
- A user-supplied A/UX disk image containing the Apple partition map and disk driver.
- Native hfsutils (`hformat`, `hfsck`, `hmount`, `hcopy`, `humount`, `hls`).
- A compiled `mkbb` tool and `bootblk.bin` template.
- This source checkout and its Python, shell and host-C image tools.

The existing kernel build consumes the AMIX patch media; this runner consumes its
resulting ELF. It does not accept original tape archives or installer CDs yet.
AMIX media identification/extraction and browser linker ports remain prerequisites
for the final website workflow.

Build the host commands with `sh webapp/build.sh`. Build the boot helpers, if needed,
with `sh kernel/mac/bootblk/build.sh`; this needs the existing m68k binutils setup.
The native runner also checks for Python 3, a C compiler, cpio and standard Unix
tools. It is intended for a Linux build host; Windows is not a supported image-build
host. These tools are not embedded into the Go binary.

## Request

Create a JSON file using your own paths. The output directory must already exist;
the output image must not exist. Root size is MiB, at least 64 and divisible by 4.
The default root and swap sizes are each 64 MiB.

```json
{
  "selection": {
    "machine": "q800",
    "devices": ["adb", "framebuffer", "scc", "scsi53c96"]
  },
  "sourceRepo": "/path/to/ashnazag",
  "tapeDir": "/path/to/amix-tape-segments",
  "kernelELF": "/path/to/unix-mac.elf",
  "bootDonor": "/path/to/user-owned-aux-disk.img",
  "output": "/path/to/output/ash-nazag-q800.img",
  "rootMiB": 64,
  "swapMiB": 64,
  "tools": {
    "hfsutils": "/path/to/hfsutils/bin",
    "mkbb": "/path/to/mkbb",
    "bootblk": "/path/to/bootblk.bin"
  }
}
```

```sh
webapp/build/ashbuild -request build.json > build-receipt.json
```

Paths are resolved from the working directory; absolute paths are recommended.
Progress and tool logs go to stderr. The receipt on stdout records normalized
selection, input hashes, output size and output SHA-256. Optional `expectedSHA256`
maps input paths to expected hashes; a mismatch or unused path fails preflight.
Use a previous receipt's `inputs` map to pin a repeat build on the same host.
A receipt is a local audit record, not the output offered by the final website.

## Execution

The runner prepares a private temporary source directory beside the destination,
patches the AMIX root through the existing recipe, builds and checks UFS and HFS,
assembles the partition map and swap, and verifies boot blocks and root partition
contents. It rechecks source hashes before atomically publishing the image without
replacing an existing file. Temporary files are removed on success or failure;
interrupting on Unix terminates the build process group.

The console recipe omits the optional `dstest` diagnostic binary. The provided
kernel retains its built-in drivers, including SONIC even when it is not selected.
Memory declarations describe the intended hardware; the kernel discovers RAM at
boot. No prebuilt kernel, installation media or generated system image should be
copied into the website's public output directory.

Hashes cover explicit inputs and executable files, not every host runtime library.
This is a traced native recipe, not yet a hermetic toolchain. Treat source scripts
and tools as trusted build inputs. Image structure checks do not establish hardware
compatibility; boot-test each kernel/recipe combination before using the image.

## Browser work

The advanced website assembler now combines a prepared Apple boot disk and UFS
root with empty swap using Go/WASM. It preserves the supplied kernel and root;
configurator selections are not applied. Disk-backed output supports up to 8 GiB, subject to browser quota; the memory
fallback remains capped at 256 MiB. The filesystem panel and native `ashfs` command
create UFS filesystems up to 2 GiB from layered SVR4 cpio archives. The Quadra
console mode consumes segments 02/03/10 and a prebuilt kernel, applying the
console recipe in Go. The complete Quadra form also creates HFS boot media and the final disk in Go/WASM.
Kernel linking remains native; supply a prebuilt ELF.

The same high-level selection can drive the complete browser executor after media
readers, filesystem writers and linkers have WASM implementations. Go owns orchestration
and validation; host subprocess execution remains native-only. The synthetic
[linker probe](toolchain/README.md) establishes native reference behavior and can
compare a future WASM linker against it.
