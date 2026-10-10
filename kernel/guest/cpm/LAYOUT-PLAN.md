# CP/M drive layout (decided 2026-10-10, implemented)

## Drives

| Drive | Unix | Access |
|---|---|---|
| A: | `~/CPM` (with `-e env`: `~/CPM/env`) | read-write, starts empty, user areas |
| H: | `$HOME` | Unix permissions |
| M: | `~/.cpm/tmp` (with `-e env`: `~/.cpm/tmp/env`) | private (0700), emptied at each start |
| O: | `/` | Unix permissions |
| P: | `/cpm/dist`: DRI's programs from all nine disks | read-only, shared |

The plan said S:, T: and U:. DRI's CCP and PIP accept only A: to P: ("S:?", "INVALID FORMAT"), so the letters moved: temp is M: (Concurrent CP/M's scratch drive letter), root is O:, programs are P:. Other letters are unassigned. Users add drives with:
- `~/CPM/DRIVES`: one `X=path` per line; `#` starts a comment; `~` and relative paths start at home; `X=` removes a drive.
- `startcpm -d X=path`, after DRIVES.
- `MOUNT X: path` on P:, inside a session; `MOUNT` alone lists the drives. The CCP upper-cases the path, so each missing name is matched ignoring case.

## Command search
The CCP looks a command up with a search across all user areas of the current drive, then opens it there. The BDOS adds P:'s `.68K` and `.SUB` files to that search. It also retries a failed open of such a file on P: and points the FCB at P:. A file on the user's own drive wins. Nothing is copied into A:; P: holds real files (`X.68K` is a hard link to `X.REL`), not symlinks.

## Unix names as 8.3
- Fits 8.3 (case ignored) and not taken: upper case, as before. Leading dot: SYS.
- Otherwise mangled: the first six valid characters, `~`, one base-36 digit from a hash of the full name, then the first three of the last suffix: `Makefile.linux` -> `MAKEFI~7.LIN`. A taken digit moves to the next free one, in directory order.
- Names that differ only in case: the later one is mangled.
- Directories and devices are hidden. New files from CP/M get lower-case host names.

## User areas
Only A: has user areas (subdirectory `N` for user N > 0). Other drives have user 0 only: USER 5 there shows nothing and cannot create files.

## New users and the image
A new user's A: is created empty on first start. The image no longer ships a prefilled `~/CPM/A`; the old `~/CPM/A` ... `P` layout is gone, with no migration.

## Concurrent CP/M-68K later
The drive table, name rule and P: search are in the shared host file layer, ready for a second BDOS. Concurrent CP/M adds per-console default drive and user, record locks (Unix `fcntl`) and shared P:/O:. `DRIVES` is per user, read by every console.

## Slow `dir`
Search first stats every matching file before DIR prints a line. In QEMU (RAM disk), DIR of 200 files takes 84 ms, the same as TYPE of the same output, so the CPU path is fine. On real disks the old A: (156 symlinks into `/cpm/dist`) cost about three cold reads per entry, plus an `access()` that walked each link again. Both are gone: `access()` is skipped when only the owner may write, and A: no longer holds links. A lazy stat in search next would make output start sooner; not done.

## Open
The letters M:, O:, P: replace the plan's T:, U:, S:; the user should confirm them.
