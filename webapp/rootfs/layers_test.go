package rootfs

import (
	"bytes"
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"amigaux.org/imagebuilder/media"
	"amigaux.org/imagebuilder/ufs"
)

func layer(files ...fixture) Source {
	data := cpio(files...)
	return Source{bytes.NewReader(data), int64(len(data))}
}

func TestLayerReplacementPreservesHardlinks(t *testing.T) {
	entries, err := ScanLayers(context.Background(), []Source{
		layer(fixture{"dir", "", 0040700, 9, 1}, fixture{"a", "old", 0104755, 7, 3}, fixture{"b", "", 0104755, 7, 3}, fixture{"c", "", 0104755, 7, 3}),
		layer(fixture{"dir", "", 0040755, 9, 1}, fixture{"a", "new", 0100600, 7, 2}, fixture{"d", "", 0100600, 7, 2}),
	}, Options{})
	if err != nil {
		t.Fatal(err)
	}
	byPath := map[string]ufs.Entry{}
	for _, entry := range entries {
		byPath[entry.Path] = entry
	}
	for _, pair := range []struct {
		path, body string
		mode       uint32
	}{{"/a", "new", 0600}, {"/b", "old", 04755}} {
		entry := byPath[pair.path]
		if entry.Kind != 'f' || entry.Mode != pair.mode || entry.UID != 42 || entry.GID != 43 || *entry.ModTime != 12345 {
			t.Fatalf("metadata: %+v", entry)
		}
		body, err := io.ReadAll(io.NewSectionReader(entry.Data, 0, entry.Size))
		if err != nil || string(body) != pair.body {
			t.Fatalf("%s: %q %v", pair.path, body, err)
		}
	}
	if byPath["/c"].Target != "/b" || byPath["/d"].Target != "/a" || byPath["/dir"].Mode != 0755 {
		t.Fatalf("entries: %+v", entries)
	}
}

func TestLayerConflicts(t *testing.T) {
	for _, sources := range [][]Source{
		{layer(fixture{"a", "", 0040755, 1, 1}), layer(fixture{"a", "x", 0100644, 1, 1})},
		{layer(fixture{"a", "x", 0100644, 1, 1}), layer(fixture{"a", "", 0040755, 1, 1})},
		{layer(fixture{"a/b", "x", 0100644, 1, 1}), layer(fixture{"a", "target", 0120777, 1, 1})},
		{layer(fixture{"a", "target", 0120777, 1, 1}), layer(fixture{"a/b", "x", 0100644, 1, 1})},
	} {
		if _, err := ScanLayers(context.Background(), sources, Options{}); err == nil {
			t.Fatal("accepted conflicting tree")
		}
	}
}

func TestLayerBoundsAndCancellation(t *testing.T) {
	sources := []Source{layer(fixture{"a", "old", 0100644, 1, 1}), layer(fixture{"a", "new", 0100644, 1, 1})}
	for _, options := range []Options{
		{MaxMetadataBytes: 500},
		{Limits: media.Limits{MaxEntries: 1}},
		{Limits: media.Limits{MaxArchiveBytes: uint64(sources[0].Size)}},
	} {
		if _, err := ScanLayers(context.Background(), sources, options); err == nil || !strings.Contains(err.Error(), "limit") {
			t.Fatal(err)
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := ScanLayers(ctx, sources, Options{}); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	if _, err := ScanLayers(context.Background(), nil, Options{}); err == nil {
		t.Fatal("accepted missing layers")
	}
}

func TestLocalTapeLayers(t *testing.T) {
	var sources []Source
	for _, name := range []string{"02", "03", "10"} {
		f, err := os.Open(filepath.Join("..", "..", "kernel", "mac", "diskroot", "build", "tape", name))
		if os.IsNotExist(err) {
			t.Skip("local tape archives unavailable")
		}
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { f.Close() })
		info, err := f.Stat()
		if err != nil {
			t.Fatal(err)
		}
		sources = append(sources, Source{f, info.Size()})
	}
	entries, err := ScanLayers(context.Background(), sources, Options{})
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) < 500 {
		t.Fatalf("unexpected tape entry count: %d", len(entries))
	}
	byPath := map[string]ufs.Entry{}
	for _, entry := range entries {
		byPath[entry.Path] = entry
	}
	for _, entry := range entries {
		if entry.Kind == 'h' && (byPath[entry.Target].Kind == 0 || byPath[entry.Target].Kind == 'h' || byPath[entry.Target].Kind == 'd') {
			t.Fatalf("unresolved hardlink: %+v", entry)
		}
	}
	anchor := func(name string) string {
		if entry := byPath[name]; entry.Kind == 'h' {
			return entry.Target
		}
		return name
	}
	console := anchor("/dev/console")
	if byPath[console].Kind != 'c' || anchor("/dev/syscon") != console || anchor("/dev/systty") != console {
		t.Fatal("console device aliases lost their shared inode")
	}
	t.Logf("merged %d tape entries", len(entries))
}

func TestNonregularLayerHardlinks(t *testing.T) {
	for _, item := range []struct {
		kind byte
		mode uint32
		body string
	}{
		{'c', 0020620, ""}, {'b', 0060600, ""}, {'p', 0010644, ""}, {'l', 0120777, "../target"},
	} {
		t.Run(string(item.kind), func(t *testing.T) {
			entries, err := ScanLayers(context.Background(), []Source{
				layer(fixture{"a", "", item.mode, 7, 3}, fixture{"b", item.body, item.mode, 7, 3}, fixture{"c", "", item.mode, 7, 3}),
				layer(fixture{"a", "replacement", 0100600, 7, 1}),
			}, Options{})
			if err != nil {
				t.Fatal(err)
			}
			b, c := entries[1], entries[2]
			if b.Path != "/b" || b.Kind != item.kind || b.Mode != item.mode&07777 || b.UID != 42 || b.GID != 43 || *b.ModTime != 12345 || c.Kind != 'h' || c.Target != "/b" {
				t.Fatalf("entries: %+v", entries)
			}
			if (item.kind == 'c' || item.kind == 'b') && (b.Major != 3 || b.Minor != 4) {
				t.Fatalf("device: %+v", b)
			}
			if item.kind == 'l' && b.Target != item.body {
				t.Fatalf("symlink: %+v", b)
			}
		})
	}
}

func TestNonregularHardlinkConflicts(t *testing.T) {
	data := cpio(fixture{"a", "", 0020600, 7, 2}, fixture{"b", "", 0020600, 7, 2})
	// Change the second member's device minor while retaining its inode identity.
	second := bytes.Index(data[6:], []byte("070702")) + 6
	copy(data[second+6+10*8:second+6+11*8], []byte("00000005"))
	if _, err := scan(data, Options{}); err == nil || !strings.Contains(err.Error(), "hardlink metadata") {
		t.Fatal(err)
	}
	for _, files := range [][]fixture{
		{{"a", "one", 0120777, 7, 2}, {"b", "two", 0120777, 7, 2}},
		{{"a", "", 0120777, 7, 2}, {"b", "", 0120777, 7, 2}},
	} {
		if _, err := scan(cpio(files...), Options{}); err == nil {
			t.Fatal("accepted invalid symlink group")
		}
	}
}
