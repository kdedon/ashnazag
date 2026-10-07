package recipes

import (
	"bytes"
	"embed"
	"fmt"
	"sort"
	"strings"

	"amigaux.org/imagebuilder/ufs"
)

//go:embed atarietc/*
var atariConfig embed.FS

// AtariRoot turns the console root into the Falcon root: its node name, clock
// setup and mounts, with /home on its own slice. zone is /etc/TIMEZONE's TZ.
func AtariRoot(entries []ufs.Entry, zone string) ([]ufs.Entry, error) {
	if zone == "" || strings.ContainsAny(zone, " \t\r\n'\"\\$`;&|<>") {
		return nil, fmt.Errorf("invalid time zone %q", zone)
	}
	tree := map[string]ufs.Entry{}
	for _, e := range entries {
		tree[e.Path] = e
	}
	file := func(p string, mode, uid, gid uint32, b []byte) {
		tree[p] = ufs.Entry{Path: p, Kind: 'f', Mode: mode, UID: uid, GID: gid, Size: int64(len(b)), Data: bytes.NewReader(b)}
	}
	for _, f := range []struct {
		p, name string
		mode    uint32
	}{{"/etc/sysinit", "sysinit", 0744}, {"/etc/nodename", "nodename", 0644}, {"/usr/amiga/bin/setclk", "setclk", 0755}} {
		b, err := atariConfig.ReadFile("atarietc/" + f.name)
		if err != nil {
			return nil, err
		}
		if f.name == "sysinit" {
			b = withoutLines(b, "modadmin -r") // no loadable drivers in this root
		}
		file(f.p, f.mode, 0, 3, b)
	}
	file("/etc/TIMEZONE", 0444, 0, 3, []byte("TZ="+zone+"\nexport TZ\n"))
	file("/etc/vfstab", 0744, 0, 3, []byte("/dev/dsk/c0d0s1\t/dev/rdsk/c0d0s1\t/\tufs\t1\tno\t-\n"+
		"proc\t-\t/proc\tproc\t0\tno\t-\nfd\t-\t/dev/fd\tfd\t0\tno\t-\n"+
		"/dev/dsk/c0d0s3\t/dev/rdsk/c0d0s3\t/home\tufs\t2\tyes\t-\n"))
	tree["/home"] = ufs.Entry{Path: "/home", Kind: 'd', Mode: 0755, UID: 0, GID: 3}
	out := make([]ufs.Entry, 0, len(tree))
	for _, e := range tree {
		out = append(out, e)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out, nil
}

// AtariHome lists /home: lost+found and the guest account's directory.
func AtariHome() []ufs.Entry {
	return []ufs.Entry{{Path: "/lost+found", Kind: 'd', Mode: 0755}, {Path: "/guest", Kind: 'd', Mode: 0755, UID: 100, GID: 1}}
}
