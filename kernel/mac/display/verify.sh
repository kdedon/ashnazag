#!/bin/sh
# verify.sh -- static checks of the display service in the linked kernel.
#
#   sh kernel/mac/display/verify.sh [kernel-dir]
#
# Run after build.sh.  Reads build/unix-mac.elf; one OK/FAIL line per
# check, exit 1 on any FAIL.
set -e

K=$(cd "${1:-$(dirname "$0")/../..}" && pwd)
AUX=$(cd "$K/.." && pwd)
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH
ELF="$K/build/unix-mac.elf"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

m68k-elf-nm "$ELF" > "$T/nm"
m68k-elf-objdump -d "$ELF" > "$T/dis"

python3 - "$T" <<'EOF'
import re, sys
T = sys.argv[1]
sym, typ = {}, {}
for l in open(T + '/nm'):
    p = l.split()
    if len(p) == 3:
        sym[p[2]] = int(p[0], 16)
        typ[p[2]] = p[1]
body, cur = {}, None
for l in open(T + '/dis'):
    m = re.match(r'[0-9a-f]+ <([^>]+)>:', l)
    if m:
        cur = m.group(1)
        body.setdefault(cur, [])
    elif cur:
        body[cur].append(l.rstrip('\n'))
def calls(f):
    return re.findall(r'(?:jsr|bsr[a-z.]*|jmp) [^<]*<([A-Za-z_0-9]+)>', '\n'.join(body.get(f, [])))
def reach(roots, prefix):
    seen, todo = set(), list(roots)
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        todo += [c for c in calls(f) if c.startswith(prefix)]
    return seen
bad = 0
def check(ok, msg):
    global bad
    print(('OK   ' if ok else 'FAIL ') + msg)
    if not ok:
        bad = 1

# the VBL shares VIA2 CA1 with the SONIC: every low line served, bounded
p2 = '\n'.join(sum((body.get(x, []) for x in ('Lp2slot', 'Lp2serve', 'Lp2loop', 'Lp2vbl', 'Lp2again')), []))
check('<snintr>' in p2 and '<ds_vblintr>' in p2 and 'btst #6,%d3' in p2 and
      re.search(r'dbeq %d2,[0-9a-f]+ <Lp2loop>', p2) is not None,
      'p2int: CA1 serves port A bit 0 (snintr) and bit 6 (ds_vblintr) until both are high')

# user translations are unloaded only where no process is inside the HAT:
# the service's own functions reached from its interrupt-time entries
irq = reach(['ds_key', 'ds_mouse', 'ds_vblintr', 'ds_tick', 'ds_panic'], 'ds_')
out = set(c for f in irq for c in calls(f))
for f in ('ds_switch', 'ds_unloadsess', 'hat_unload', 'kmem_alloc', 'kmem_zalloc', 'kmem_free', 'sleep'):
    check(f not in irq | out, '%s not reached from the ADB, VBL, tick or panic paths' % f)
check('qenable' in calls('ds_key') and 'ds_switch' in calls('ds_srv'),
      'hotkeys: ds_key -> qenable, ds_srv (queuerun) -> ds_switch')
check(set(c for c in sym if 'ds_unloadsess' in calls(c)) <= {'ds_switch', 'ds_endsess'},
      'ds_unloadsess called only by ds_switch and ds_endsess')

# the console path and panic hook
fo = '\n'.join(body.get('fb_own', []))
check(re.search(r'movel #%d,[0-9a-f]+ <fbcons_panicfn>' % sym.get('ds_panic', -1),
                '\n'.join(body.get('ds_init', []))) is not None and
      re.search(r'<fbcons_panicfn>,%a0\n.*\n.*\n.*<fbcons_panicfn>\n.*jsr %a0@', fo) is not None,
      'fb_own calls fbcons_panicfn once, which ds_init sets to ds_panic')
t = '\n'.join(body.get('adbkbd_tick', []))
check('adb_keyfn' not in t, 'adbkbd_tick repeats for the console whatever the key consumer')
check('conskey' in calls('adbkbd_cons'), 'adbkbd_cons is the console key path')

# no stray writes to hat internals: the WT change touches leaf entries only
check('flushmmu' in calls('ds_setwt'), 'ds_setwt flushes the ATC after changing entries')
for s in ('ds_sess', 'ds_fbh', 'ds_evh', 'ds_disp'):
    check(typ.get(s) in ('B', 'b', 'D', 'd'), '%s is data (%s)' % (s, typ.get(s)))
sys.exit(bad)
EOF
