// Package provision stages system templates and packages without user homes.
package provision

import (
	"amigaux.org/imagebuilder/ufs"
	"bytes"
	"embed"
	"fmt"
	"io"
	"path"
	"sort"
	"strings"
	"unicode/utf8"
)

//go:embed policies/*
var policyFiles embed.FS

type Policy struct {
	Family string
	Values map[string]string
}
type Bundle struct {
	Family  string
	ID      string
	Entries []ufs.Entry
}

func Parse(data []byte) (Policy, error) {
	p := Policy{Values: map[string]string{}}
	if len(data) > 65536 || !utf8.Valid(data) {
		return p, fmt.Errorf("invalid policy encoding or size")
	}
	allowed := map[string]bool{}
	for _, key := range []string{"FAMILY", "SYSTEM_TEMPLATE_ROOT", "SYSTEM_APPS_ROOT", "SYSTEM_PACKAGE_DB", "SYSTEM_SOURCE_CACHE", "ACCOUNT_ROOT", "ENVIRONMENT_ROOT", "LEGACY_ROOT", "USER_SOURCE_CACHE", "ENVIRONMENT_METADATA", "ACCOUNT_PACKAGE_DB", "ENVIRONMENT_PACKAGE_DB", "METADATA", "GUEST_SYSTEM", "GUEST_APPLICATIONS", "GUEST_ENVIRONMENT_DRIVE", "GUEST_ACCOUNT_DRIVE", "GUEST_SYSTEM_DRIVE"} {
		allowed[key] = true
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok || !allowed[k] || v == "" || strings.ContainsAny(v, "\x00\r\n\t$`\\\"';&|<>(){}*?[]!") {
			return p, fmt.Errorf("invalid policy line")
		}
		if _, ok := p.Values[k]; ok {
			return p, fmt.Errorf("duplicate policy key %s", k)
		}
		for _, c := range k {
			if !(c >= 'A' && c <= 'Z' || c == '_') {
				return p, fmt.Errorf("invalid policy key")
			}
		}
		p.Values[k] = v
	}
	p.Family = p.Values["FAMILY"]
	if p.Family != "mac" && p.Family != "tos" && p.Family != "amiga" {
		return p, fmt.Errorf("invalid policy family")
	}
	for _, k := range []string{"SYSTEM_TEMPLATE_ROOT", "SYSTEM_APPS_ROOT", "SYSTEM_PACKAGE_DB", "SYSTEM_SOURCE_CACHE"} {
		v := p.Values[k]
		if !strings.HasPrefix(v, "/") || path.Clean(v) != v || v == "/" || strings.Contains(v, "..") {
			return p, fmt.Errorf("invalid %s", k)
		}
	}
	for _, k := range []string{"ACCOUNT_ROOT", "ENVIRONMENT_ROOT", "LEGACY_ROOT", "USER_SOURCE_CACHE"} {
		v := p.Values[k]
		if !strings.HasPrefix(v, "~/") || path.Clean(v) != v || strings.Contains(v, "..") {
			return p, fmt.Errorf("invalid %s", k)
		}
	}
	for _, k := range []string{"ENVIRONMENT_METADATA", "ACCOUNT_PACKAGE_DB", "ENVIRONMENT_PACKAGE_DB"} {
		v := p.Values[k]
		if v == "" || v == "." || strings.HasPrefix(v, "/") || path.Clean(v) != v || strings.Contains(v, "..") {
			return p, fmt.Errorf("invalid %s", k)
		}
	}
	if p.Values["METADATA"] != map[string]string{"mac": "appledouble", "amiga": "amiga", "tos": "dos83"}[p.Family] {
		return p, fmt.Errorf("invalid metadata policy")
	}
	for _, k := range []string{"SYSTEM_TEMPLATE_ROOT", "SYSTEM_APPS_ROOT"} {
		if !strings.HasPrefix(p.Values[k], "/"+p.Family+"/") {
			return p, fmt.Errorf("system guest roots must remain under family root")
		}
	}
	a, b := strings.ToLower(p.Values["SYSTEM_APPS_ROOT"]), strings.ToLower(p.Values["SYSTEM_TEMPLATE_ROOT"])
	if a == b || strings.HasPrefix(a, b+"/") || strings.HasPrefix(b, a+"/") {
		return p, fmt.Errorf("overlapping system roots")
	}
	if p.Values["SYSTEM_PACKAGE_DB"] != "/var/sadm/install/contents" || p.Values["SYSTEM_SOURCE_CACHE"] != "/var/spool/pkg" {
		return p, fmt.Errorf("system package paths must use standard SVR4 locations")
	}
	if p.Values["ACCOUNT_ROOT"] != p.Values["ENVIRONMENT_ROOT"]+"/Shared" {
		return p, fmt.Errorf("account root must be environment Shared directory")
	}
	if p.Family == "tos" {
		seen := map[string]bool{}
		for _, key := range []string{"GUEST_ENVIRONMENT_DRIVE", "GUEST_ACCOUNT_DRIVE", "GUEST_SYSTEM_DRIVE"} {
			v := p.Values[key]
			if len(v) != 1 || v[0] < 'A' || v[0] > 'Z' || seen[v] {
				return p, fmt.Errorf("invalid or duplicate guest drive")
			}
			seen[v] = true
		}
	} else if p.Family == "amiga" {
		if p.Values["GUEST_SYSTEM"] != "SYS:" || p.Values["GUEST_APPLICATIONS"] != "Apps:" {
			return p, fmt.Errorf("invalid Amiga guest mapping")
		}
	} else if p.Values["GUEST_APPLICATIONS"] != "Applications" {
		return p, fmt.Errorf("invalid Mac guest mapping")
	}
	return p, nil
}
func DefaultPolicies() map[string][]byte {
	out := map[string][]byte{}
	for _, f := range []string{"mac", "tos", "amiga"} {
		b, _ := policyFiles.ReadFile("policies/" + f)
		out[f] = b
	}
	return out
}
func validID(s string) bool {
	if s == "" || s == "." || s == ".." || strings.EqualFold(s, "Shared") || len(s) > 64 {
		return false
	}
	if !(s[0] >= 'a' && s[0] <= 'z' || s[0] >= 'A' && s[0] <= 'Z' || s[0] >= '0' && s[0] <= '9') {
		return false
	}
	for _, c := range s {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '-' || c == '_' || c == '.') {
			return false
		}
	}
	return true
}

// Apply adds root-owned system content. Existing conflicting paths fail.
func Apply(base []ufs.Entry, policies map[string][]byte, templates, apps []Bundle) ([]ufs.Entry, error) {
	out := append([]ufs.Entry(nil), base...)
	seen := map[string]ufs.Entry{}
	for _, e := range base {
		if !absolutePath(e.Path) {
			return nil, fmt.Errorf("unsafe base path")
		}
		if _, ok := seen[e.Path]; ok {
			return nil, fmt.Errorf("duplicate base path %s", e.Path)
		}
		seen[e.Path] = e
	}
	add := func(e ufs.Entry) error {
		if old, ok := seen[e.Path]; ok {
			if old.Kind == 'd' && e.Kind == 'd' {
				if old.UID != 0 || old.Mode&022 != 0 {
					return fmt.Errorf("unsafe system directory ownership or mode: %s", e.Path)
				}
				return nil
			}
			return fmt.Errorf("provision path conflict %s", e.Path)
		}
		out = append(out, e)
		seen[e.Path] = e
		return nil
	}
	ensureParents := func(target string) error {
		dirs := []string{}
		for p := path.Dir(target); ; p = path.Dir(p) {
			dirs = append(dirs, p)
			if p == "/" {
				break
			}
		}
		for i := len(dirs) - 1; i >= 0; i-- {
			if err := add(ufs.Entry{Path: dirs[i], Kind: 'd', Mode: 0755}); err != nil {
				return err
			}
		}
		return nil
	}
	parsed := map[string]Policy{}
	families := []string{}
	for f := range policies {
		families = append(families, f)
	}
	sort.Strings(families)
	for _, f := range families {
		p, err := Parse(policies[f])
		if err != nil {
			return nil, err
		}
		if p.Family != f {
			return nil, fmt.Errorf("policy family mismatch")
		}
		parsed[f] = p
		b := append([]byte(nil), policies[f]...)
		if err := ensureParents("/etc/default/" + f); err != nil {
			return nil, err
		}
		if old, ok := seen["/etc/default/"+f]; ok && old.Kind == 'f' && old.UID == 0 && old.GID == 0 && old.Mode == 0644 && old.Size == int64(len(b)) && old.Data != nil {
			data := make([]byte, len(b))
			n, err := old.Data.ReadAt(data, 0)
			if n == len(b) && (err == nil || err == io.EOF) && bytes.Equal(data, b) {
				continue
			}
		}
		if err := add(ufs.Entry{Path: "/etc/default/" + f, Kind: 'f', Mode: 0644, Size: int64(len(b)), Data: bytes.NewReader(b)}); err != nil {
			return nil, err
		}
	}
	bundleRoots := map[string]bool{}
	for _, group := range []struct {
		items []Bundle
		key   string
	}{{templates, "SYSTEM_TEMPLATE_ROOT"}, {apps, "SYSTEM_APPS_ROOT"}} {
		for _, bundle := range group.items {
			p, ok := parsed[bundle.Family]
			if !ok || !validID(bundle.ID) {
				return nil, fmt.Errorf("invalid provision bundle")
			}
			entries := append([]ufs.Entry(nil), bundle.Entries...)
			sort.Slice(entries, func(i, j int) bool { return entries[i].Path < entries[j].Path })
			root := p.Values[group.key] + "/" + bundle.ID
			key := strings.ToLower(root)
			if bundleRoots[key] {
				return nil, fmt.Errorf("duplicate bundle root")
			}
			bundleRoots[key] = true
			if err := ensureParents(root); err != nil {
				return nil, err
			}
			objects := map[string]ufs.Entry{}
			for _, e := range entries {
				if _, ok := objects[e.Path]; ok {
					return nil, fmt.Errorf("duplicate bundle entry")
				}
				objects[e.Path] = e
			}
			for _, e := range entries {
				if e.Kind == 'h' {
					target, ok := objects[e.Target]
					if !ok || target.Kind != 'f' {
						return nil, fmt.Errorf("hardlink requires same-bundle regular target")
					}
				}
			}
			if err := add(ufs.Entry{Path: root, Kind: 'd', Mode: 0755}); err != nil {
				return nil, err
			}
			local := map[string]string{}
			for _, e := range entries {
				if !absolutePath(e.Path) || !utf8.ValidString(e.Target) {
					return nil, fmt.Errorf("unsafe bundle path")
				}
				for prefix := e.Path; prefix != "/"; prefix = path.Dir(prefix) {
					key := strings.ToLower(prefix)
					if prior, ok := local[key]; ok && prior != prefix {
						return nil, fmt.Errorf("bundle case collision")
					}
					local[key] = prefix
				}
				if e.Path == "/" {
					if e.Kind != 'd' {
						return nil, fmt.Errorf("invalid bundle root")
					}
					continue
				}
				if e.Kind != 'f' && e.Kind != 'd' && e.Kind != 'h' && e.Kind != 'l' {
					return nil, fmt.Errorf("guest bundles cannot create host devices")
				}
				if e.Size < 0 || e.Kind == 'f' && e.Size > 0 && e.Data == nil {
					return nil, fmt.Errorf("invalid bundle file data")
				}
				e.Path = root + e.Path
				e.UID = 0
				e.GID = 0
				e.Mode &= 0755
				if e.Kind == 'h' {
					if !strings.HasPrefix(e.Target, "/") || path.Clean(e.Target) != e.Target {
						return nil, fmt.Errorf("unsafe hardlink")
					}
					e.Target = root + e.Target
				}
				if e.Kind == 'l' {
					target := path.Clean(path.Join(path.Dir(e.Path), e.Target))
					if e.Target == "" || strings.ContainsAny(e.Target, "\x00\\\r\n") || path.IsAbs(e.Target) || strings.Contains("/"+e.Target+"/", "/../") || (target != root && !strings.HasPrefix(target, root+"/")) {
						return nil, fmt.Errorf("bundle symlink escapes")
					}
				}
				if err := ensureParents(e.Path); err != nil {
					return nil, err
				}
				if err := add(e); err != nil {
					return nil, err
				}
			}
		}
	}
	for _, e := range out {
		for p := path.Dir(e.Path); p != "/" && p != "."; p = path.Dir(p) {
			if ancestor, ok := seen[p]; ok && ancestor.Kind != 'd' {
				return nil, fmt.Errorf("provision parent is not a directory")
			}
		}
	}
	return out, nil
}

func absolutePath(p string) bool {
	return utf8.ValidString(p) && strings.HasPrefix(p, "/") && path.Clean(p) == p && !strings.ContainsAny(p, "\x00\\\r\n\t")
}
