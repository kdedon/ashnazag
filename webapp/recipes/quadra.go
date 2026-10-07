// Package recipes adapts installation archives for supported machines.
package recipes

import (
	"bytes"
	"embed"
	"encoding/binary"
	"fmt"
	"io"
	"path"
	"regexp"
	"sort"
	"strings"

	"amigaux.org/imagebuilder/provision"
	"amigaux.org/imagebuilder/ufs"
)

//go:embed etc/*
var config embed.FS

// QuadraRoot configures a console root from the core, BSD and terminfo layers.
// Readers must remain available until the filesystem has been written.
func QuadraRoot(entries []ufs.Entry, kernel io.ReaderAt, kernelSize int64) ([]ufs.Entry, error) {
	header := make([]byte, 52)
	if kernel == nil || kernelSize < 52 || kernelSize > 2147483647 {
		return nil, fmt.Errorf("kernel size is invalid")
	}
	n, err := kernel.ReadAt(header, 0)
	if n != len(header) || (err != nil && err != io.EOF) || !bytes.Equal(header[:7], []byte{0x7f, 'E', 'L', 'F', 1, 2, 1}) || binary.BigEndian.Uint16(header[16:]) != 2 || binary.BigEndian.Uint16(header[18:]) != 4 {
		return nil, fmt.Errorf("kernel must be an executable big-endian m68k ELF32")
	}
	tree := map[string]ufs.Entry{}
	for _, e := range entries {
		if e.Path == "" || !strings.HasPrefix(e.Path, "/") || path.Clean(e.Path) != e.Path || strings.ContainsRune(e.Path, 0) {
			return nil, fmt.Errorf("invalid path %q", e.Path)
		}
		if _, ok := tree[e.Path]; ok {
			return nil, fmt.Errorf("duplicate path %s", e.Path)
		}
		tree[e.Path] = e
	}
	original := map[string]ufs.Entry{}
	groups := map[string][]string{}
	for p, e := range tree {
		seen := map[string]bool{p: true}
		target := p
		for e.Kind == 'h' {
			target = e.Target
			if seen[target] {
				return nil, fmt.Errorf("cyclic hard link %s", p)
			}
			seen[target] = true
			var ok bool
			e, ok = tree[target]
			if !ok {
				return nil, fmt.Errorf("missing hard link target %s", target)
			}
		}
		if e.Kind == 'd' && target != p {
			return nil, fmt.Errorf("directory hard link %s", p)
		}
		original[p] = e
		groups[target] = append(groups[target], p)
	}
	read := func(p string) ([]byte, error) {
		e, ok := original[p]
		if !ok || e.Kind != 'f' || e.Size < 0 || e.Size > 8<<20 || (e.Size > 0 && e.Data == nil) {
			return nil, fmt.Errorf("missing or invalid recipe source %s", p)
		}
		b := make([]byte, e.Size)
		if len(b) == 0 {
			return b, nil
		}
		n, err := e.Data.ReadAt(b, 0)
		if n != len(b) {
			return nil, fmt.Errorf("read %s: %w", p, io.ErrUnexpectedEOF)
		}
		if err != nil && err != io.EOF {
			return nil, err
		}
		return b, nil
	}
	touched := map[string]bool{}
	put := func(e ufs.Entry) { tree[e.Path] = e; touched[e.Path] = true }
	file := func(p string, mode, uid, gid uint32, b []byte) {
		put(ufs.Entry{Path: p, Kind: 'f', Mode: mode, UID: uid, GID: gid, Size: int64(len(b)), Data: bytes.NewReader(b)})
	}
	patch := func(src, dst string, mode, uid, gid uint32, fn func([]byte) ([]byte, error)) error {
		b, e := read(src)
		if e != nil {
			return e
		}
		b, e = fn(b)
		if e != nil {
			return fmt.Errorf("%s: %w", src, e)
		}
		file(dst, mode, uid, gid, b)
		return nil
	}
	replace := func(pairs ...string) func([]byte) ([]byte, error) {
		return func(b []byte) ([]byte, error) {
			s := string(b)
			for j := 0; j < len(pairs); j += 2 {
				if strings.Count(s, pairs[j]) != 1 {
					return nil, fmt.Errorf("expected one patch site %q", pairs[j])
				}
				s = strings.Replace(s, pairs[j], pairs[j+1], 1)
			}
			return []byte(s), nil
		}
	}
	jobs := []struct {
		src, dst       string
		mode, uid, gid uint32
		fn             func([]byte) ([]byte, error)
	}{
		{"/etc/profile", "/etc/profile", 0644, 0, 3, replace("TERM=amiga", "TERM=vt100", "\tif sioc\n", "\tif false\n")},
		{"/usr/sbin/shutdown", "/sbin/shutdown", 0755, 0, 3, replace("\nif /usr/amiga/bin/sioc &&", "\nif false &&")},
		{"/usr/sbin/rc0", "/sbin/rc0", 0744, 0, 3, replace("\n/sbin/umountall\n", "\n/sbin/umountall\n/sbin/sync\n/usr/bin/sleep 5\n")},
		{"/usr/sbin/rc6", "/sbin/rc6", 0744, 0, 3, replace("\n/sbin/umountall\n", "\n/sbin/umountall\n/sbin/sync\n/usr/bin/sleep 5\n")},
		{"/etc/motd", "/etc/motd", 0644, 2, 2, replace("Amiga Version 2.1", "Ash Nazag")},
		{"/etc/screendefs", "/etc/screendefs", 0644, 2, 2, func(b []byte) ([]byte, error) {
			var out []byte
			for _, line := range bytes.SplitAfter(b, []byte("\n")) {
				if bytes.HasPrefix(line, []byte("#")) {
					out = append(out, line...)
				}
			}
			if len(out) == 0 {
				return nil, fmt.Errorf("missing screen comments")
			}
			return out, nil
		}},
		{"/etc/group", "/etc/group", 0444, 0, 3, func(b []byte) ([]byte, error) {
			if len(b) == 0 || b[len(b)-1] != '\n' {
				return nil, fmt.Errorf("invalid group file")
			}
			var out []byte
			for _, line := range bytes.SplitAfter(b, []byte("\n")) {
				if !bytes.HasPrefix(line, []byte("display:")) {
					out = append(out, line...)
				}
			}
			return append(out, []byte("display::25:\n")...), nil
		}},
		{"/etc/vfstab", "/etc/vfstab", 0744, 0, 3, func(b []byte) ([]byte, error) {
			r := regexp.MustCompile(`(?m)^(/dev/dsk/c0d0s1[\t ].*[\t ]/[\t ]*)s5([\t ]|$)`)
			if len(r.FindAllIndex(b, -1)) != 1 {
				return nil, fmt.Errorf("expected one s5 root mount")
			}
			return r.ReplaceAll(b, []byte("${1}ufs${2}")), nil
		}},
		{"/usr/sbin/swap", "/usr/sbin/swap", 02755, 2, 3, func(b []byte) ([]byte, error) {
			for _, p := range [][3]int{{0x1216, 0xe581, 0xe781}, {0x121e, 0xe581, 0xe781}, {0x1034, 0x780b, 0x780c}, {0x1044, 0x780b, 0x780c}, {0x1054, 0x780b, 0x780c}} {
				if len(b) < p[0]+2 || int(binary.BigEndian.Uint16(b[p[0]:])) != p[1] {
					return nil, fmt.Errorf("unexpected swap instruction at %#x", p[0])
				}
				binary.BigEndian.PutUint16(b[p[0]:], uint16(p[2]))
			}
			return b, nil
		}},
	}
	for _, j := range jobs {
		if err := patch(j.src, j.dst, j.mode, j.uid, j.gid, j.fn); err != nil {
			return nil, err
		}
	}
	for _, pattern := range []string{"/stand/*", "/dev/scr", "/dev/screen", "/dev/term/*", "/dev/dsk/fd*", "/dev/rdsk/fd*", "/dev/cage", "/dev/clock", "/dev/aen*", "/dev/amiga", "/dev/machid", "/dev/noise", "/dev/par", "/dev/tiga*", "/dev/res*", "/etc/saf/screens", "/etc/saf/serial", "/etc/saf/xdm", "/etc/saf/tcp", "/etc/saf/inetd"} {
		for p := range tree {
			for candidate := p; candidate != "/"; candidate = path.Dir(candidate) {
				match, _ := path.Match(pattern, candidate)
				if match {
					delete(tree, p)
					touched[p] = true
					break
				}
			}
		}
	}
	if old, ok := tree["/etc/default"]; ok && old.Kind != 'd' {
		return nil, fmt.Errorf("layout policy parent must be a directory")
	}
	put(ufs.Entry{Path: "/etc/default", Kind: 'd', Mode: 0755})
	put(ufs.Entry{Path: "/stand/unix", Kind: 'f', Mode: 0644, UID: 0, GID: 3, Size: kernelSize, Data: kernel})
	put(ufs.Entry{Path: "/etc/ap", Kind: 'd', Mode: 0755, UID: 0, GID: 3})
	for _, d := range []struct {
		p                       string
		mode, gid, major, minor uint32
	}{{"/dev/term/b", 0620, 7, 0, 1}, {"/dev/fb0", 0660, 25, 51, 0}, {"/dev/kbd", 0660, 25, 52, 0}, {"/dev/mouse", 0660, 25, 53, 0}} {
		put(ufs.Entry{Path: d.p, Kind: 'c', Mode: d.mode, GID: d.gid, Major: d.major, Minor: d.minor})
	}
	for _, f := range []struct {
		p, name string
		mode    uint32
	}{{"/etc/ap/chan.ap", "chan.ap", 0644}, {"/etc/ioctl.syscon", "ioctl.syscon", 0644}, {"/etc/inittab", "inittab", 0644}, {"/etc/sysinit", "sysinit", 0744}, {"/etc/TIMEZONE", "TIMEZONE", 0444}, {"/etc/nodename", "nodename", 0644}, {"/usr/amiga/bin/setclk", "setclk", 0755}, {"/etc/saf/_sactab", "_sactab", 0644}, {"/etc/inet/network-config", "network-config", 0444}} {
		b, err := config.ReadFile("etc/" + f.name)
		if err != nil {
			return nil, err
		}
		if f.name == "sysinit" {
			b = withoutLines(b, "modadmin -r") // no loadable drivers in this root
		}
		if f.name == "inittab" {
			b = withoutLines(b, "/usr/lib/sndd", "/usr/lib/sndaux", "/usr/lib/pingd") // not in this root
		}
		file(f.p, f.mode, 0, 3, b)
	}
	for _, p := range []string{"/var/adm/utmp", "/var/adm/utmpx"} {
		file(p, 0644, 0, 2, nil)
	}
	for _, h := range [][2]string{{"/dev/fb", "/dev/fb0"}, {"/usr/sbin/shutdown", "/sbin/shutdown"}, {"/usr/sbin/rc0", "/sbin/rc0"}, {"/usr/sbin/rc6", "/sbin/rc6"}} {
		put(ufs.Entry{Path: h[0], Kind: 'h', Target: h[1]})
	}
	// Preserve surviving links when their original name was replaced or removed.
	for target, names := range groups {
		sort.Strings(names)
		var survivors []string
		for _, p := range names {
			if !touched[p] {
				survivors = append(survivors, p)
			}
		}
		if len(survivors) == 0 {
			continue
		}
		anchor := survivors[0]
		if !touched[target] {
			anchor = target
		}
		e := original[anchor]
		e.Path = anchor
		tree[anchor] = e
		for _, p := range survivors {
			if p != anchor {
				tree[p] = ufs.Entry{Path: p, Kind: 'h', Target: anchor}
			}
		}
	}
	out := make([]ufs.Entry, 0, len(tree))
	for _, e := range tree {
		out = append(out, e)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return provision.Apply(out, provision.DefaultPolicies(), nil, nil)
}

// withoutLines drops the lines that contain any of subs.
func withoutLines(b []byte, subs ...string) []byte {
	var out []byte
	for _, l := range bytes.SplitAfter(b, []byte("\n")) {
		keep := true
		for _, sub := range subs {
			if bytes.Contains(l, []byte(sub)) {
				keep = false
			}
		}
		if keep {
			out = append(out, l...)
		}
	}
	return out
}
