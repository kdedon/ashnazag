package rootfs

import (
	"context"
	"errors"
	"fmt"
	"io"
	"path"
	"sort"

	"amigaux.org/imagebuilder/ufs"
)

type Source struct {
	Reader io.ReaderAt
	Size   int64
}

type layeredEntry struct {
	entry ufs.Entry
	file  *ufs.Entry
}

// ScanLayers overlays archives in order, retaining references to their payloads.
// Sources must remain open and unchanged until filesystem generation completes.
// Directories merge; replacing a directory with another file type is rejected.
// Limits bound the cumulative archive input, entries, and metadata.
func ScanLayers(ctx context.Context, sources []Source, options Options) ([]ufs.Entry, error) {
	if len(sources) == 0 {
		return nil, errors.New("no archive sources")
	}
	if options.MaxMetadataBytes == 0 {
		options.MaxMetadataBytes = 32 << 20
	}
	if options.Limits.MaxArchiveBytes == 0 {
		options.Limits.MaxArchiveBytes = 8 << 30
	}
	if options.Limits.MaxEntries == 0 {
		options.Limits.MaxEntries = 1_000_000
	}
	var bytes, count, metadata uint64
	tree := map[string]layeredEntry{}
	for layer, source := range sources {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		if source.Reader == nil || source.Size <= 0 {
			return nil, fmt.Errorf("layer %d: invalid archive source", layer+1)
		}
		if uint64(source.Size) > options.Limits.MaxArchiveBytes-bytes {
			return nil, errors.New("layered archive size limit exceeded")
		}
		bytes += uint64(source.Size)
		if count == options.Limits.MaxEntries || metadata == options.MaxMetadataBytes {
			return nil, errors.New("layered archive metadata or entry limit exceeded")
		}
		remaining := options
		remaining.Limits.MaxEntries -= count
		remaining.MaxMetadataBytes -= metadata
		entries, err := Scan(ctx, source.Reader, source.Size, remaining)
		if err != nil {
			return nil, fmt.Errorf("layer %d: %w", layer+1, err)
		}
		files := map[string]*ufs.Entry{}
		for i := range entries {
			e := &entries[i]
			metadata += uint64(384 + len(e.Path))
			if e.Kind == 'l' {
				metadata += uint64(len(e.Target))
			}
			if e.Kind != 'd' && e.Kind != 'h' {
				files[e.Path] = e
			}
		}
		count += uint64(len(entries))
		for _, e := range entries {
			if old, ok := tree[e.Path]; ok && (old.entry.Kind == 'd') != (e.Kind == 'd') {
				return nil, fmt.Errorf("layer %d: directory type conflict at %q", layer+1, e.Path)
			}
			file := files[e.Path]
			if e.Kind == 'h' {
				file = files[e.Target]
				if file == nil {
					return nil, fmt.Errorf("layer %d: missing hardlink target %q", layer+1, e.Target)
				}
			}
			tree[e.Path] = layeredEntry{e, file}
		}
		for _, item := range tree {
			for parent := path.Dir(item.entry.Path); parent != "." && parent != "/"; parent = path.Dir(parent) {
				if ancestor, ok := tree[parent]; ok && ancestor.entry.Kind != 'd' {
					return nil, fmt.Errorf("layer %d: parent %q is not a directory", layer+1, parent)
				}
			}
		}
	}
	names := make([]string, 0, len(tree))
	for name := range tree {
		names = append(names, name)
	}
	sort.Strings(names)
	result := make([]ufs.Entry, 0, len(tree))
	targets := map[*ufs.Entry]string{}
	for _, name := range names {
		item := tree[name]
		e := item.entry
		if item.file != nil {
			// Bind surviving names to the original inode even if its first name was replaced.
			e = *item.file
			e.Path = name
			if target, ok := targets[item.file]; ok {
				e.Kind, e.Target, e.Data, e.Size = 'h', target, nil, 0
			} else {
				targets[item.file] = name
			}
		}
		result = append(result, e)
	}
	return result, nil
}
