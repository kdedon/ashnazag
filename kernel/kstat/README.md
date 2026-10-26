# kstat

System statistics, after Solaris kstat: named records read from `/dev/kstat` (character major 69). The `kstat` module loads on the first open once `modadmin -r cdev kstat 69` has registered the major (the kstat package's `S03kstat` does this at boot). One build serves the Mac, Amiga and Atari kernels; it asks the kernel for its page size and tick rate.

## Reading

Each `read(2)` returns a fresh snapshot from its start, taken with interrupts held off:

```
struct kstat_hdr { ks_magic, ks_ndata, ks_snaptime, ks_hz }
kstat_named_t    { char name[20]; unsigned long value; }  x ks_ndata
```

A short buffer gets the header and the first records. Opening for writing fails with EACCES. `kstat.h` holds the layout.

## Records

| Name | Value |
|---|---|
| `avenrun_1min`, `avenrun_5min`, `avenrun_15min` | load averages, fixed point; `fscale` is 1.00 |
| `nrun` | runnable and running processes at the last sample |
| `cpu_idle`, `cpu_user`, `cpu_kernel`, `cpu_wait` | ticks in each CPU state |
| `wait_io`, `wait_swap`, `wait_pio` | idle ticks with block, swap or raw I/O pending |
| `pagesize`, `physmem`, `freemem`, `availrmem`, `availsmem` | page size in bytes; memory in pages |
| `pgin`, `pgout`, `pgpgin`, `pgpgout`, `pswpin`, `pswpout` | page-ins and page-outs (operations, pages), swapped pages |
| `faults`, `zfod`, `pgrec`, `scan`, `dfree` | faults, zero-fill faults, reclaims, pages scanned and freed by the page daemon |
| `swtch`, `intr`, `trap`, `syscall`, `sysfork`, `sysexec` | context switches, interrupts, traps, system calls, forks, execs |
| `readch`, `writech` | bytes read and written |
| `boot_time` | seconds since 1970 at boot |

Counters run from boot and wrap. Load averages are sampled every 5 seconds; unlike the kernel's own, they count the running process. New records are added at the end; look them up by name.

## libkstat

```c
kstat_ctl_t *kc = kstat_open();          /* opens /dev/kstat, first snapshot */
kstat_read(kc);                          /* new snapshot: 0, or -1 */
kstat_named_t *k = kstat_data_lookup(kc, "freemem");
unsigned long v = kstat_value(kc, "cpu_user");  /* 0 when absent */
kstat_close(kc);
```

Link `-lkstat` (`/usr/lib/libkstat.a`, header `/usr/include/kstat.h`). Processes come from `/proc` (`PIOCPSINFO`) and swap from `swapctl(2)`; kstat holds neither.
