# s5disk — s5 on a disk slice

`s5disk` runs in the guest at boot (inittab `sysinit`): for 1 KB blocks on `c0d0s4` and 2 KB on
`c0d0s5` it makes an s5 file system, copies `/usr/bin` in, creates, removes and renames files and
directories, unmounts, runs `fsck -n`, remounts, compares the listing and the data with
`/usr/bin`, changes the tree again and runs `fsck -n` once more. Output on the console:
`PASS s5disk.1k.fsck1` …, then `S5DISK DONE pass=N fail=M` (8 checks).

```sh
sh tests/s5disk/mkimage.sh [--kernel-dir DIR]   # -> tests/s5disk/build/s5disk.img
sh images/qemu/run-mac.sh "Quadra 800.ROM" tests/s5disk/build/s5disk.img 32 OUT wait:245 quit
grep -a 'PASS\|FAIL\|DONE' OUT/serial.log
```

`mkimage.sh` builds the disk root (`kernel/mac/diskroot`, copied into `build/`) with DIR's
`build/unix-mac.elf` and two 24 MB spare slices. It needs the tape segments in
`kernel/mac/diskroot/build/tape`. The run takes about 4 minutes; QEMU uses `-snapshot`.
