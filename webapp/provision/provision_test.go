package provision

import (
	"bytes"
	"context"
	"crypto/sha256"
	"fmt"
	"io"
	"os"
	"strings"
	"testing"

	"amigaux.org/imagebuilder/svr4"
	"amigaux.org/imagebuilder/ufs"
)

func TestCanonicalPolicies(t *testing.T) {
	for family, b := range DefaultPolicies() {
		actual, err := os.ReadFile("../../etc/default/" + family)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(b, actual) {
			t.Fatalf("stale embedded %s policy", family)
		}
		if _, err = Parse(b); err != nil {
			t.Fatal(err)
		}
	}
}
func TestStageSystemPayload(t *testing.T) {
	var archive bytes.Buffer
	entries := []ufs.Entry{{Path: "/", Kind: 'd', Mode: 0777}, {Path: "/App", Kind: 'f', Mode: 06777, UID: 1000, GID: 1000, Size: 5, Data: strings.NewReader("hello")}, {Path: "/Alias", Kind: 'h', Target: "/App"}}
	if err := svr4.Emit(context.Background(), &archive, svr4.Info{Package: "ASHtest", Name: "Test", Version: "1", Architecture: "m68k", BaseDir: "/"}, entries[1:]); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(archive.Bytes())
	a := Artifact{Kind: "app", Family: "mac", ID: "test-1", SHA256: fmt.Sprintf("%x", sum), Size: int64(archive.Len())}
	out, err := Stage(context.Background(), nil, []Artifact{a}, []io.ReaderAt{bytes.NewReader(archive.Bytes())})
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, e := range out {
		if strings.HasPrefix(e.Path, "/home/") {
			t.Fatal("created home")
		}
		if e.Path == "/mac/apps/test-1/App" {
			found = true
			if e.UID != 0 || e.GID != 0 || e.Mode != 0755 {
				t.Fatalf("unsafe ownership/mode: %+v", e)
			}
		}
		if e.Kind == 'h' && e.Target != "/mac/apps/test-1/App" {
			t.Fatal(e.Target)
		}
	}
	if !found {
		t.Fatal("missing app")
	}
	if _, err = Apply(out, DefaultPolicies(), nil, nil); err != nil {
		t.Fatal("policies not idempotent", err)
	}
	a.SHA256 = strings.Repeat("0", 64)
	if _, err = Stage(context.Background(), nil, []Artifact{a}, []io.ReaderAt{bytes.NewReader(archive.Bytes())}); err == nil {
		t.Fatal("accepted corrupted package")
	}
}
func TestBundleSafety(t *testing.T) {
	tests := [][]ufs.Entry{
		{{Path: "/../escape", Kind: 'f'}}, {{Path: "/link", Kind: 'l', Target: "../../etc/passwd"}}, {{Path: "/device", Kind: 'c'}},
		{{Path: "/App/a", Kind: 'f'}, {Path: "/app/b", Kind: 'f'}}, {{Path: "/a", Kind: 'l', Target: "."}, {Path: "/a/b", Kind: 'f'}},
		{{Path: "/a/b/up", Kind: 'l', Target: "../.."}, {Path: "/a/b/esc", Kind: 'l', Target: "up/../../../etc"}},
	}
	for _, entries := range tests {
		if _, err := Apply(nil, DefaultPolicies(), []Bundle{{Family: "mac", ID: "system-7", Entries: entries}}, nil); err == nil {
			t.Fatalf("accepted unsafe entries %+v", entries)
		}
	}
	out, err := Apply(nil, DefaultPolicies(), []Bundle{{Family: "mac", ID: "system-7", Entries: []ufs.Entry{{Path: "/", Kind: 'd'}, {Path: "/System Folder", Kind: 'd'}}}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, e := range out {
		if e.Path == "/mac/sys/system-7/System Folder" {
			found = true
		}
	}
	if !found {
		t.Fatal("missing staged template directory")
	}
}
func TestInvalidPolicies(t *testing.T) {
	b := DefaultPolicies()["mac"]
	for _, bad := range [][]byte{append(append([]byte{}, b...), []byte("FAMILY=mac\n")...), bytes.ReplaceAll(b, []byte("/mac/sys"), []byte("/home/user")), bytes.ReplaceAll(b, []byte("appledouble"), []byte("none"))} {
		if _, err := Parse(bad); err == nil {
			t.Fatal("accepted invalid policy")
		}
	}
}

func TestProtectedAncestors(t *testing.T) {
	for _, base := range [][]ufs.Entry{
		{{Path: "/mac", Kind: 'd', UID: 1000, Mode: 0755}},
		{{Path: "/mac", Kind: 'd', Mode: 0777}},
		{{Path: "/etc", Kind: 'l', Target: "tmp"}},
		{{Path: "/", Kind: 'd', Mode: 0777}},
		{{Path: "/../escape", Kind: 'd', Mode: 0755}},
	} {
		if _, err := Apply(base, DefaultPolicies(), nil, []Bundle{{Family: "mac", ID: "test", Entries: []ufs.Entry{{Path: "/file", Kind: 'f'}}}}); err == nil {
			t.Fatal("unsafe base accepted", base)
		}
	}
	out, err := Apply(nil, DefaultPolicies(), nil, []Bundle{{Family: "mac", ID: "test", Entries: []ufs.Entry{{Path: "/private/file", Kind: 'f'}, {Path: "/private", Kind: 'd', Mode: 0700}}}})
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range out {
		if e.Path == "/mac/apps/test/private" && e.Mode != 0700 {
			t.Fatal("private mode lost")
		}
	}
}
func TestDuplicateAndExternalLinkBundles(t *testing.T) {
	a := Bundle{Family: "mac", ID: "test", Entries: []ufs.Entry{{Path: "/a", Kind: 'd', Mode: 0755}}}
	b := Bundle{Family: "mac", ID: "test", Entries: []ufs.Entry{{Path: "/b", Kind: 'd', Mode: 0755}}}
	if _, err := Apply(nil, DefaultPolicies(), nil, []Bundle{a, b}); err == nil {
		t.Fatal("duplicate bundles merged")
	}
	a.Entries = []ufs.Entry{{Path: "/link", Kind: 'h', Target: "/absent"}}
	if _, err := Apply(nil, DefaultPolicies(), nil, []Bundle{a}); err == nil {
		t.Fatal("external hardlink accepted")
	}
	a.Entries = []ufs.Entry{{Path: "/dir", Kind: 'd'}, {Path: "/link", Kind: 'h', Target: "/dir"}}
	if _, err := Apply(nil, DefaultPolicies(), nil, []Bundle{a}); err == nil {
		t.Fatal("directory hardlink accepted")
	}
	a.Entries = []ufs.Entry{{Path: "/caf\x8e", Kind: 'f'}}
	if _, err := Apply(nil, DefaultPolicies(), nil, []Bundle{a}); err == nil {
		t.Fatal("lossy filename accepted")
	}
}
func TestPolicyMappings(t *testing.T) {
	mac, tos := DefaultPolicies()["mac"], DefaultPolicies()["tos"]
	for _, b := range [][]byte{
		append(append([]byte{}, mac...), []byte("EVIL=value\n")...),
		bytes.ReplaceAll(mac, []byte("/mac/apps"), []byte("/mac/sys")),
		bytes.ReplaceAll(mac, []byte("/var/sadm/install/contents"), []byte("/home/developer/contents")),
		bytes.ReplaceAll(mac, []byte("~/Mac/Shared"), []byte("~/Other")),
		bytes.ReplaceAll(mac, []byte("Applications"), []byte("$(bad)")),
		bytes.ReplaceAll(tos, []byte("GUEST_ACCOUNT_DRIVE=O"), []byte("GUEST_ACCOUNT_DRIVE=C")),
	} {
		if _, err := Parse(b); err == nil {
			t.Fatal("unsafe mapping accepted")
		}
	}
}

type cancellingReader struct {
	cancel context.CancelFunc
	calls  int
}

func (r *cancellingReader) ReadAt(p []byte, off int64) (int, error) {
	r.calls++
	r.cancel()
	return len(p), nil
}
func TestStageCancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	r := &cancellingReader{cancel: cancel}
	a := Artifact{Kind: "app", Family: "mac", ID: "test", Size: 128 << 10}
	if _, err := Stage(ctx, nil, []Artifact{a}, []io.ReaderAt{r}); err != context.Canceled {
		t.Fatal(err)
	}
	if r.calls != 1 {
		t.Fatal("read after cancellation")
	}
	if _, err := Stage(context.Background(), nil, []Artifact{a}, []io.ReaderAt{nil}); err == nil {
		t.Fatal("nil reader accepted")
	}
}
