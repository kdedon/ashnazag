// Package packagebuild executes locked recipes into relocatable packages.
package packagebuild

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"path"
	"sort"
	"strings"
	"unicode/utf8"

	"amigaux.org/imagebuilder/packages"
	"amigaux.org/imagebuilder/sources"
	"amigaux.org/imagebuilder/svr4"
	"amigaux.org/imagebuilder/ufs"
)

type Input struct {
	Reader io.ReaderAt
	Size   int64
}
type Result struct {
	Package []byte
	Entries []ufs.Entry
	Receipt packages.Receipt
}

func sum(ctx context.Context, r io.ReaderAt, n int64) (string, error) {
	h := sha256.New()
	buf := make([]byte, 64<<10)
	for off := int64(0); off < n; {
		if err := ctx.Err(); err != nil {
			return "", err
		}
		size := int64(len(buf))
		if size > n-off {
			size = n - off
		}
		if r == nil {
			return "", fmt.Errorf("missing reader")
		}
		if _, err := r.ReadAt(buf[:size], off); err != nil {
			return "", err
		}
		h.Write(buf[:size])
		off += size
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
func digest(b []byte) string { v := sha256.Sum256(b); return hex.EncodeToString(v[:]) }
func Build(ctx context.Context, binding packages.Binding, inputs map[string]Input, info svr4.Info, limits sources.Limits) (Result, error) {
	var out Result
	r := binding.Recipe
	if err := packages.RequireExecutable(r); err != nil {
		return out, err
	}
	if packages.HashRecipe(r) != binding.RecipeSHA256 {
		return out, fmt.Errorf("recipe hash mismatch")
	}
	if binding.Tier != "environment" && binding.Tier != "account" && binding.Tier != "system" {
		return out, fmt.Errorf("unknown tier")
	}
	if !r.Shareable && binding.Tier != "environment" {
		return out, fmt.Errorf("private package cannot install shared")
	}
	if info.Version != r.UpstreamVersion {
		return out, fmt.Errorf("package version differs from recipe")
	}
	if limits.ExpandedBytes == 0 {
		limits.ExpandedBytes = 256 << 20
	}
	if limits.Entries == 0 {
		limits.Entries = 100000
	}
	if limits.InputBytes == 0 {
		limits.InputBytes = 256 << 20
	}
	if limits.ExpandedBytes < 0 || limits.Entries < 0 || limits.InputBytes < 0 {
		return out, fmt.Errorf("invalid limits")
	}
	for _, m := range r.Metadata.Entries {
		if m.Protection != 0 || m.Comment != "" {
			return out, fmt.Errorf("Amiga host metadata encoding is unqualified")
		}
	}
	verify := func() error {
		for _, s := range r.Sources {
			in, ok := inputs[s.SHA256]
			if !ok || in.Size != s.Size || in.Size <= 0 || in.Size > limits.InputBytes {
				return fmt.Errorf("missing or wrong-sized source %s", s.ID)
			}
			got, err := sum(ctx, in.Reader, in.Size)
			if err != nil {
				return err
			}
			if got != s.SHA256 {
				return fmt.Errorf("source checksum mismatch: %s", s.ID)
			}
		}
		return nil
	}
	if err := verify(); err != nil {
		return out, err
	}
	tree := map[string]ufs.Entry{}
	fold := map[string]string{}
	metadata := map[string]packages.MetadataEntry{}
	var total int64
	safe := func(p string) bool {
		return strings.HasPrefix(p, "/") && p != "/" && path.Clean(p) == p && !strings.ContainsAny(p, "\\\x00\r\n\t")
	}
	put := func(e ufs.Entry, replace bool) error {
		if !utf8.ValidString(e.Path) || !utf8.ValidString(e.Target) {
			return fmt.Errorf("package paths require UTF-8; source name conversion is unqualified")
		}
		if !safe(e.Path) {
			return fmt.Errorf("unsafe target path %q", e.Path)
		}
		if old, ok := tree[e.Path]; ok {
			if !replace {
				if old.Kind == 'd' && e.Kind == 'd' && old.Mode == e.Mode {
					return nil
				}
				return fmt.Errorf("conflicting write %s", e.Path)
			}
			total -= old.Size
		}
		for p := e.Path; p != "/"; p = path.Dir(p) {
			k := strings.ToLower(p)
			if old, ok := fold[k]; ok && old != p {
				return fmt.Errorf("case collision %s", p)
			}
			fold[k] = p
		}
		if len(tree) >= limits.Entries && !replace {
			return fmt.Errorf("entry limit exceeded")
		}
		if e.Size < 0 || e.Size > limits.ExpandedBytes-total {
			return fmt.Errorf("expanded size limit exceeded")
		}
		if e.Kind == 'f' {
			if e.Size > 0 && e.Data == nil {
				return fmt.Errorf("missing payload")
			}
			b := make([]byte, int(e.Size))
			if e.Size > 0 {
				if _, err := e.Data.ReadAt(b, 0); err != nil {
					return err
				}
			}
			e.Data = bytes.NewReader(b)
		}
		switch e.Kind {
		case 'f', 'd', 'h', 'l':
		default:
			return fmt.Errorf("unsupported package object %q", e.Kind)
		}
		e.UID = 0
		e.GID = 0
		tree[e.Path] = e
		total += e.Size
		return nil
	}
	for _, op := range r.Operations {
		if err := ctx.Err(); err != nil {
			return out, err
		}
		p := "/" + op.Path
		switch op.Type {
		case "extract":
			var source packages.Source
			for _, s := range r.Sources {
				if s.ID == op.Source {
					source = s
					break
				}
			}
			in := inputs[source.SHA256]
			decoded, err := sources.Read(ctx, source.Format, in.Reader, in.Size, limits)
			if err != nil {
				return out, err
			}
			for name, m := range decoded.Metadata {
				if m.Protection != 0 || m.Comment != "" {
					return out, fmt.Errorf("Amiga host metadata encoding is unqualified for %s", name)
				}
				metadata[path.Join(p, name)] = packages.MetadataEntry{Path: strings.TrimPrefix(path.Join(p, name), "/"), Protection: m.Protection, Comment: m.Comment}
			}
			if err := put(ufs.Entry{Path: p, Kind: 'd', Mode: 0755}, false); err != nil {
				return out, err
			}
			for _, entry := range decoded.Entries {
				if entry.Path == "/" {
					continue
				}
				if !safe(entry.Path) {
					return out, fmt.Errorf("unsafe source path")
				}
				entry.Path = p + entry.Path
				if entry.Kind == 'h' {
					if !safe(entry.Target) {
						return out, fmt.Errorf("unsafe hardlink target")
					}
					entry.Target = p + entry.Target
				}
				if err := put(entry, false); err != nil {
					return out, err
				}
			}
		case "file":
			if err := put(ufs.Entry{Path: p, Kind: 'f', Mode: op.Mode, Data: strings.NewReader(op.Data), Size: int64(len(op.Data))}, false); err != nil {
				return out, err
			}
		case "directory":
			if err := put(ufs.Entry{Path: p, Kind: 'd', Mode: op.Mode}, false); err != nil {
				return out, err
			}
		case "symlink", "hardlink":
			kind := byte('l')
			target := op.Target
			if op.Type == "hardlink" {
				kind = 'h'
				target = "/" + target
			}
			if err := put(ufs.Entry{Path: p, Kind: kind, Target: target, Mode: op.Mode}, false); err != nil {
				return out, err
			}
		case "patch":
			e, ok := tree[p]
			if !ok || e.Kind != 'f' {
				return out, fmt.Errorf("patch requires existing regular file %s", p)
			}
			hash, err := sum(ctx, e.Data, e.Size)
			if err != nil {
				return out, err
			}
			if hash != op.SHA256 {
				return out, fmt.Errorf("patch precondition failed %s", p)
			}
			e.Data = strings.NewReader(op.Data)
			e.Size = int64(len(op.Data))
			if op.Mode != 0 {
				e.Mode = op.Mode
			}
			if err := put(e, true); err != nil {
				return out, err
			}
		default:
			return out, fmt.Errorf("unsupported operation %s", op.Type)
		}
	}
	for _, p := range r.Verification.RequiredPaths {
		if _, ok := tree["/"+p]; !ok {
			return out, fmt.Errorf("required path missing: %s", p)
		}
	}
	for _, p := range r.State.WritablePaths {
		if binding.Tier != "environment" {
			if _, ok := tree["/"+p]; ok {
				return out, fmt.Errorf("writable state overlaps shared payload")
			}
		}
	}
	for _, m := range r.Metadata.Entries {
		if _, ok := tree["/"+m.Path]; !ok {
			return out, fmt.Errorf("metadata references missing path")
		}
		metadata["/"+m.Path] = m
	}
	for _, e := range tree {
		out.Entries = append(out.Entries, e)
	}
	sort.Slice(out.Entries, func(i, j int) bool { return out.Entries[i].Path < out.Entries[j].Path })
	check := r
	check.Operations = []packages.Operation{}
	check.Verification.RequiredPaths = []string{}
	for _, e := range out.Entries {
		op := packages.Operation{Type: "file", Path: strings.TrimPrefix(e.Path, "/")}
		if e.Kind == 'd' {
			op.Type = "directory"
		}
		if e.Kind == 'l' {
			op.Type = "symlink"
			op.Target = e.Target
		}
		if e.Kind == 'h' {
			op.Type = "hardlink"
			op.Target = strings.TrimPrefix(e.Target, "/")
		}
		check.Operations = append(check.Operations, op)
	}
	if err := packages.ValidateRecipe(check); err != nil {
		return Result{}, err
	}
	var encoded bytes.Buffer
	if err := svr4.Emit(ctx, &encoded, info, out.Entries); err != nil {
		return Result{}, err
	}
	if err := verify(); err != nil {
		return Result{}, err
	}
	receipt := packages.Receipt{FormatVersion: 1, EnvironmentID: binding.EnvironmentID, PackageID: r.ID, Version: r.UpstreamVersion, Tier: binding.Tier, RecipeSHA256: binding.RecipeSHA256, Files: []packages.InstalledFile{}, Verified: true}
	for _, e := range out.Entries {
		hash := digest(nil)
		if e.Kind == 'f' {
			var err error
			hash, err = sum(ctx, e.Data, e.Size)
			if err != nil {
				return Result{}, err
			}
		} else if e.Kind == 'l' || e.Kind == 'h' {
			hash = digest([]byte(e.Target))
		}
		md := packages.Metadata{Format: r.Metadata.Format, Entries: []packages.MetadataEntry{}}
		if m, ok := metadata[e.Path]; ok {
			md.Entries = append(md.Entries, m)
		}
		receipt.Files = append(receipt.Files, packages.InstalledFile{Path: strings.TrimPrefix(e.Path, "/"), SHA256: hash, Metadata: md})
	}
	if err := packages.ValidateReceipt(receipt); err != nil {
		return Result{}, err
	}
	out.Package = encoded.Bytes()
	out.Receipt = receipt
	return out, nil
}
