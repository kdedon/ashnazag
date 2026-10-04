package rootfs

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"path"
	"sort"
	"strings"

	"amigaux.org/imagebuilder/media"
	"amigaux.org/imagebuilder/ufs"
)

type Options struct {
	Limits           media.Limits
	MaxMetadataBytes uint64
}

type contextReader struct {
	ctx context.Context
	r   io.Reader
}

func (r contextReader) Read(p []byte) (int, error) {
	if err := r.ctx.Err(); err != nil {
		return 0, err
	}
	return r.r.Read(p)
}

type linkKey struct{ major, minor, inode uint32 }
type linkGroup struct {
	members []int
	header  media.CPIOHeader
	data    io.ReaderAt
	size    int64
	digest  [32]byte
	target  string
}

// Scan validates an archive and retains references to its file payloads.
// The source must remain open and unchanged until filesystem generation completes.
func Scan(ctx context.Context, source io.ReaderAt, size int64, options Options) ([]ufs.Entry, error) {
	if source == nil || size < 0 {
		return nil, errors.New("invalid archive source")
	}
	if options.MaxMetadataBytes == 0 {
		options.MaxMetadataBytes = 32 << 20
	}
	if options.Limits.MaxArchiveBytes == 0 || options.Limits.MaxArchiveBytes > uint64(size) {
		options.Limits.MaxArchiveBytes = uint64(size)
	}
	if size == 0 {
		return nil, errors.New("empty archive")
	}
	r := media.NewCPIOReader(contextReader{ctx, io.NewSectionReader(source, 0, size)}, options.Limits)
	entries := []ufs.Entry{}
	paths := map[string]bool{}
	links := map[linkKey]*linkGroup{}
	var metadata uint64
	scratch := make([]byte, 64<<10)
	for {
		h, err := r.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		name := h.Name
		if name == "." {
			name = "/"
		} else {
			name = "/" + name
		}
		if paths[name] {
			return nil, fmt.Errorf("duplicate archive path %q", name)
		}
		paths[name] = true
		cost := uint64(384 + len(name))
		kind := byte(0)
		switch h.Mode & 0170000 {
		case 0040000:
			kind = 'd'
		case 0100000:
			kind = 'f'
		case 0120000:
			kind = 'l'
		case 0020000:
			kind = 'c'
		case 0060000:
			kind = 'b'
		case 0010000:
			kind = 'p'
		default:
			return nil, fmt.Errorf("unsupported file type at %q", name)
		}
		if name == "/" && kind != 'd' {
			return nil, errors.New("archive root must be a directory")
		}
		if kind != 'f' && kind != 'l' && h.Size != 0 {
			return nil, fmt.Errorf("unexpected payload at %q", name)
		}
		if kind == 'l' {
			cost += uint64(h.Size)
			if (h.Size == 0 && h.NLink <= 1) || h.Size > 4096 {
				return nil, fmt.Errorf("invalid symlink length at %q", name)
			}
		}
		if cost > options.MaxMetadataBytes-metadata {
			return nil, errors.New("archive metadata limit exceeded")
		}
		metadata += cost
		stamp := h.MTime
		e := ufs.Entry{Path: name, Kind: kind, Mode: h.Mode & 07777, UID: h.UID, GID: h.GID, Size: int64(h.Size), Major: h.RDevMajor, Minor: h.RDevMinor, ModTime: &stamp}
		offset := int64(r.Offset())
		var digest [32]byte
		if kind == 'f' {
			e.Data = io.NewSectionReader(source, offset, int64(h.Size))
			hash := sha256.New()
			if _, err := io.CopyBuffer(hash, r, scratch); err != nil {
				return nil, err
			}
			copy(digest[:], hash.Sum(nil))
		} else if kind == 'l' {
			data, err := io.ReadAll(r)
			if err != nil {
				return nil, err
			}
			e.Target = string(data)
			if strings.ContainsRune(e.Target, 0) {
				return nil, fmt.Errorf("NUL in symlink at %q", name)
			}
			digest = sha256.Sum256(data)
		}
		if kind != 'd' && h.NLink > 1 {
			key := linkKey{h.DevMajor, h.DevMinor, h.Inode}
			group := links[key]
			if group == nil {
				group = &linkGroup{header: *h}
				links[key] = group
			}
			old := group.header
			if old.Mode != h.Mode || old.UID != h.UID || old.GID != h.GID || old.MTime != h.MTime || old.NLink != h.NLink || old.RDevMajor != h.RDevMajor || old.RDevMinor != h.RDevMinor {
				return nil, fmt.Errorf("conflicting hardlink metadata at %q", name)
			}
			if uint32(len(group.members)) >= h.NLink {
				return nil, fmt.Errorf("hardlink count exceeded at %q", name)
			}
			if h.Size > 0 {
				if group.size > 0 && (group.size != int64(h.Size) || group.digest != digest) {
					return nil, fmt.Errorf("conflicting hardlink payload at %q", name)
				}
				group.data, group.size, group.digest, group.target = e.Data, e.Size, digest, e.Target
			}
			group.members = append(group.members, len(entries))
		}
		entries = append(entries, e)
	}
	for _, group := range links {
		first := group.members[0]
		if entries[first].Kind == 'l' {
			if group.target == "" {
				return nil, fmt.Errorf("invalid symlink length at %q", entries[first].Path)
			}
			entries[first].Target, entries[first].Size = group.target, group.size
		} else if group.data != nil {
			entries[first].Data, entries[first].Size = group.data, group.size
		}
		for _, index := range group.members[1:] {
			entries[index].Kind = 'h'
			entries[index].Target = entries[first].Path
			entries[index].Data = nil
			entries[index].Size = 0
		}
	}
	byPath := make(map[string]byte, len(entries))
	for _, e := range entries {
		byPath[e.Path] = e.Kind
	}
	for _, e := range entries {
		for parent := path.Dir(e.Path); parent != "." && parent != "/"; parent = path.Dir(parent) {
			if kind, ok := byPath[parent]; ok && kind != 'd' {
				return nil, fmt.Errorf("parent %q is not a directory", parent)
			}
		}
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].Path < entries[j].Path })
	return entries, nil
}
