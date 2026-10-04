// Package sources imports bounded installation media without host extraction.
package sources

import (
	"amigaux.org/imagebuilder/media"
	"amigaux.org/imagebuilder/rootfs"
	"amigaux.org/imagebuilder/svr4"
	"amigaux.org/imagebuilder/ufs"
	"bytes"
	"context"
	"fmt"
	"io"
	"path"
	"strings"
)

type Metadata struct {
	Protection uint32 `json:"protection"`
	Comment    string `json:"comment,omitempty"`
	Encoding   string `json:"encoding,omitempty"`
}
type Result struct {
	Entries  []ufs.Entry
	Metadata map[string]Metadata
}
type Limits struct {
	InputBytes, ExpandedBytes int64
	Entries                   int
}

func defaults(l Limits) Limits {
	if l.InputBytes == 0 {
		l.InputBytes = 256 << 20
	}
	if l.ExpandedBytes == 0 {
		l.ExpandedBytes = 256 << 20
	}
	if l.Entries == 0 {
		l.Entries = 100000
	}
	return l
}
func Read(ctx context.Context, format string, r io.ReaderAt, size int64, l Limits) (out Result, err error) {
	l = defaults(l)
	defer func() {
		if e := recover(); e != nil {
			out = Result{}
			err = fmt.Errorf("malformed %s source: %v", format, e)
		}
	}()
	if size <= 0 || size > l.InputBytes || l.ExpandedBytes <= 0 || l.Entries <= 0 {
		return out, fmt.Errorf("source size or limits invalid")
	}
	archiveLimits := media.Limits{MaxArchiveBytes: uint64(l.InputBytes), MaxFileBytes: uint64(l.ExpandedBytes), MaxEntries: uint64(l.Entries) + 4}
	switch format {
	case "cpio":
		out.Entries, err = rootfs.Scan(ctx, r, size, rootfs.Options{Limits: archiveLimits})
	case "svr4":
		_, out.Entries, err = svr4.Import(ctx, r, size, svr4.Options{Limits: archiveLimits})
	case "adf":
		out, err = readADF(ctx, r, size, l)
	case "lha":
		out, err = readLHA(ctx, r, size, l)
	case "hfs":
		out, err = readHFS(ctx, r, size, l)
	default:
		err = fmt.Errorf("unsupported source format %q", format)
	}
	if err != nil {
		return Result{}, err
	}
	if len(out.Entries) > l.Entries {
		return Result{}, fmt.Errorf("entry limit exceeded")
	}
	var total int64
	seen := map[string]bool{}
	for _, e := range out.Entries {
		if err = ctx.Err(); err != nil {
			return Result{}, err
		}
		if !safe(e.Path) {
			return Result{}, fmt.Errorf("unsafe path %q", e.Path)
		}
		key := strings.ToLower(e.Path)
		if seen[key] {
			return Result{}, fmt.Errorf("case collision %q", e.Path)
		}
		seen[key] = true
		if e.Size < 0 || e.Size > l.ExpandedBytes-total {
			return Result{}, fmt.Errorf("expanded size limit exceeded")
		}
		total += e.Size
	}
	return out, nil
}
func safe(p string) bool {
	if !strings.HasPrefix(p, "/") || path.Clean(p) != p || strings.ContainsAny(p, "\x00\\:\r\n\t") {
		return false
	}
	return true
}
func relative(p string) (string, error) {
	p = strings.ReplaceAll(p, "\\", "/")
	if strings.HasPrefix(p, "/") {
		return "", fmt.Errorf("absolute archive path")
	}
	for _, s := range strings.Split(p, "/") {
		if s == ".." {
			return "", fmt.Errorf("archive traversal")
		}
	}
	p = "/" + strings.TrimSuffix(p, "/")
	if !safe(p) || p == "/" {
		return "", fmt.Errorf("unsafe archive path")
	}
	return p, nil
}
func file(p string, b []byte) ufs.Entry {
	return ufs.Entry{Path: p, Kind: 'f', Mode: 0644, Size: int64(len(b)), Data: bytes.NewReader(b)}
}
func readAt(r io.ReaderAt, off, n, size int64) ([]byte, error) {
	if off < 0 || n < 0 || off > size || n > size-off {
		return nil, io.ErrUnexpectedEOF
	}
	b := make([]byte, int(n))
	_, err := r.ReadAt(b, off)
	return b, err
}
