#!/bin/sh
# trial-link.sh -- link the SCSI objects into the Mac kernel in a scratch tree.
#
#   sh kernel/mac/scsi/trial-link.sh WORKDIR
#
# Copies kernel/mac to WORKDIR/mac, applies the INTEGRATE.md changes to the
# copies (VIA2 level-2 dispatch in macintr.s, the SCSI objects and the
# seven sd* overrides in relink-mac.sh), relinks build/unix-040 into
# WORKDIR/unix-mac(.elf), then runs the build.sh image checks.  Writes only
# WORKDIR.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$HERE/../.." && pwd)
AUX=$(cd "$K/.." && pwd)
PORT="$K/amix-040-060-port"
T="${1:?usage: trial-link.sh WORKDIR}"
mkdir -p "$T"
T=$(cd "$T" && pwd)
rm -rf "$T/mac" "$T/w"
mkdir -p "$T/mac" "$T/w"
cp "$K"/mac/*.s "$K"/mac/*.c "$K"/mac/mac.ld "$K"/mac/relink-mac.sh "$T/mac/"
PATH="$AUX/toolchain/linux/bin:$AUX/toolchain/bin:$PATH"
export PATH

python3 - "$T/mac" "$K" "$HERE" "$T/w" <<'EOF'
import sys
mac, K, scsi, W = sys.argv[1:]

def sub(path, old, new):
    s = open(path).read()
    if old not in s:
        sys.exit('trial-link: patch anchor not found in %s:\n%s' % (path, old))
    open(path, 'w').write(s.replace(old, new, 1))

# macintr.s: VIA2 CB2 (SCSI) -> ncr96intr
sub(mac + '/macintr.s',
"""	moveb	%a0@(0x1a00),%d0
	andb	%a0@(0x1c00),%d0
	andib	&0x7f,%d0
	moveb	%d0,%a0@(0x1c00)
	moveb	%d0,%a0@(0x1a00)
	addql	&1,mac_spurious+8
	jmp	intret
""",
"""	moveb	%a0@(0x1a00),%d0
	andb	%a0@(0x1c00),%d0
	btst	&3,%d0			| CB2: 53C96 SCSI
	beq.w	Lp2other
	moveb	&0x08,%a0@(0x1a00)	| ack the edge, then service the chip
	jsr	ncr96intr
	jmp	intret
Lp2other:
	andib	&0x7f,%d0
	moveb	%d0,%a0@(0x1c00)
	moveb	%d0,%a0@(0x1a00)
	addql	&1,mac_spurious+8
	jmp	intret
""")

# macconf.c: root=cNd0sM on the boot command line
sub(mac + '/macconf.c',
"""static void
bi_parse()
""",
"""/*
 * root=cNd0sM: root on the SCSI disk at target N, slice M (block major 18);
 * swap moves to slice 2 of the same disk.
 */
#define DD_BMAJ		18
#define SWAP_NAME	16		/* swapfile.bo_name */
#define SWAP_ID		(SWAP_NAME + 10)	/* the N of "/dev/dsk/cNd0s2" */

extern unsigned long rootdev;
extern char swapfile[];

static void
mac_rootarg(s)
char *s;
{
	register char *p;

	for (p = s; *p; p++)
		if (p[0] == 'r' && p[1] == 'o' && p[2] == 'o' && p[3] == 't'
		&& p[4] == '=' && p[5] == 'c' && p[6] >= '0' && p[6] <= '6'
		&& p[7] == 'd' && p[8] == '0' && p[9] == 's'
		&& p[10] >= '0' && p[10] <= '7') {
			rootdev = DD_BMAJ << 18 | (p[10] - '0') << 4 | (p[6] - '0');
			if (swapfile[SWAP_NAME + 9] == 'c')
				swapfile[SWAP_ID] = p[6];
			mac_puts("  root: ");
			mac_puts(p + 5);
			mac_puts("\\n");
			return;
		}
}

static void
bi_parse()
""")
sub(mac + '/macconf.c',
"""			mac_puts((char *)p);
			mac_puts("\\"\\n");
			break;
""",
"""			mac_puts((char *)p);
			mac_puts("\\"\\n");
			mac_rootarg((char *)p);
			break;
""")

# relink-mac.sh: fixed kernel tree, scratch work dir, SCSI objects, overrides
r = mac + '/relink-mac.sh'
sub(r, 'K=$(cd "$MAC/.." && pwd)', 'K="%s"' % K)
sub(r, 'W="$K/build/mac"', 'W="%s"' % W)
sub(r, 'OBJS="$W/macentry.o $W/macconf.o $W/macintr.o $W/pstartmac.o $W/$ADAPT.o"',
       'sh "%s/build.sh" "$W"\n'
       'OBJS="$W/macentry.o $W/macconf.o $W/macintr.o $W/pstartmac.o $W/$ADAPT.o $W/macscsi.o"'
       % scsi)
sub(r, 'p1int p2int p3int p4int p5int p6int parinit qlintr slpoll autocon delayus"',
       'p1int p2int p3int p4int p5int p6int parinit qlintr slpoll autocon delayus\n'
       'sdopen sdqueue sdhardwarename sdpartition sdvalid sdblkno sddevsize"')
EOF

echo "[*] relink (copy of relink-mac.sh with INTEGRATE.md applied)"
sh "$T/mac/relink-mac.sh" "$PORT/build/unix-040" "$T/unix-mac" | tee "$T/relink.log"

echo "[*] build.sh stage-4 checks on the trial image"
ELF="$T/unix-mac.elf"
u=$(m68k-elf-nm -u "$ELF" | wc -l)
echo "unresolved (final ELF): $u"
[ "$u" -eq 0 ]
python3 "$PORT/src/check_relink_relocs.py" "$T/unix-mac" > "$T/validator.log" || true
tail -1 "$T/validator.log"
grep -q 'TOTAL complaints: 0 ' "$T/validator.log"
m68k-elf-readelf -h "$ELF" | grep -q 'Type: *EXEC'
[ "$(m68k-elf-readelf -l "$ELF" | grep -c '^ *LOAD')" -eq 2 ]

echo "[*] SCSI bindings in the trial image"
for s in sdopen sdqueue sdhardwarename sdpartition sdvalid sdblkno sddevsize \
	ncr96intr ncr96queue ncr96init apm_scan ncr_blind; do
	m68k-linux-gnu-nm "$T/unix-mac" | awk -v s="$s" '$3==s{printf "  %-16s %s %s\n", s, $2, $1}'
done
# relocations that reach the overrides must resolve into our objects
m68k-linux-gnu-readelf -r "$T/unix-mac" | awk '$5 ~ /^(sdopen|sdqueue|sdpartition|sdvalid|sdblkno|sddevsize|sdhardwarename)$/' \
	| wc -l | sed 's/^/  relocations to the sd* overrides: /'
echo "[OK] trial link $T/unix-mac.elf"
