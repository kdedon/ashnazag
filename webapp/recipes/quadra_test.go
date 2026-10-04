package recipes

import (
	"bytes"
	"encoding/binary"
	"io"
	"os"
	"path/filepath"
	"testing"

	"amigaux.org/imagebuilder/ufs"
)

func fixture() ([]ufs.Entry, []byte) {
	contents := map[string]string{"/etc/profile": "TERM=amiga\n\tif sioc\n", "/usr/sbin/shutdown": "#!/sbin/sh\nif /usr/amiga/bin/sioc && test x\n", "/usr/sbin/rc0": "#!/sbin/sh\n/sbin/umountall\n", "/usr/sbin/rc6": "#!/sbin/sh\n/sbin/umountall\n", "/etc/motd": "Amiga Version 2.1\n", "/etc/screendefs": "# screens\namiga\n", "/etc/group": "root::0:\ndisplay::99:\n", "/etc/vfstab": "/dev/dsk/c0d0s1 /dev/rdsk/c0d0s1 / s5 1 yes -\n"}
	var entries []ufs.Entry
	for p, s := range contents {
		entries = append(entries, ufs.Entry{Path: p, Kind: 'f', Mode: 0644, Size: int64(len(s)), Data: bytes.NewReader([]byte(s))})
	}
	swap := make([]byte, 0x1220)
	for _, p := range [][2]int{{0x1216, 0xe581}, {0x121e, 0xe581}, {0x1034, 0x780b}, {0x1044, 0x780b}, {0x1054, 0x780b}} {
		binary.BigEndian.PutUint16(swap[p[0]:], uint16(p[1]))
	}
	entries = append(entries, ufs.Entry{Path: "/usr/sbin/swap", Kind: 'f', Size: int64(len(swap)), Data: bytes.NewReader(swap)})
	kernel := make([]byte, 52)
	copy(kernel, []byte{0x7f, 'E', 'L', 'F', 1, 2, 1})
	binary.BigEndian.PutUint16(kernel[16:], 2)
	binary.BigEndian.PutUint16(kernel[18:], 4)
	return entries, kernel
}
func entryMap(es []ufs.Entry) map[string]ufs.Entry {
	m := map[string]ufs.Entry{}
	for _, e := range es {
		m[e.Path] = e
	}
	return m
}
func content(t *testing.T, e ufs.Entry) []byte {
	t.Helper()
	b, err := io.ReadAll(io.NewSectionReader(e.Data, 0, e.Size))
	if err != nil {
		t.Fatal(err)
	}
	return b
}
func TestQuadraRoot(t *testing.T) {
	es, k := fixture()
	es = append(es, ufs.Entry{Path: "/etc/saf/screens/state", Kind: 'f'}, ufs.Entry{Path: "/dev/fb", Kind: 'h', Target: "/dev/old"}, ufs.Entry{Path: "/dev/old", Kind: 'c', Major: 7}, ufs.Entry{Path: "/etc/profile-copy", Kind: 'h', Target: "/etc/profile"})
	out, err := QuadraRoot(es, bytes.NewReader(k), int64(len(k)))
	if err != nil {
		t.Fatal(err)
	}
	m := entryMap(out)
	if _, ok := m["/etc/saf/screens/state"]; ok {
		t.Fatal("subtree survived")
	}
	if string(content(t, m["/etc/profile"])) != "TERM=vt100\n\tif false\n" {
		t.Fatal("profile")
	}
	if string(content(t, m["/etc/profile-copy"])) != "TERM=amiga\n\tif sioc\n" {
		t.Fatal("surviving link mutated")
	}
	if string(content(t, m["/etc/group"])) != "root::0:\ndisplay::25:\n" {
		t.Fatal("group")
	}
	if !bytes.Contains(content(t, m["/etc/vfstab"]), []byte("/ ufs ")) {
		t.Fatal("vfstab")
	}
	if m["/dev/fb"].Target != "/dev/fb0" || m["/dev/fb0"].Major != 51 || m["/dev/fb0"].GID != 25 {
		t.Fatal("display")
	}
	if m["/usr/sbin/rc6"].Target != "/sbin/rc6" || m["/usr/sbin/swap"].Mode != 02755 {
		t.Fatal("metadata")
	}
	if binary.BigEndian.Uint16(content(t, m["/usr/sbin/swap"])[0x1216:]) != 0xe781 {
		t.Fatal("swap")
	}
	if !bytes.Contains(content(t, m["/etc/inittab"]), []byte("co:234:respawn:/etc/getty")) {
		t.Fatal("console")
	}
}
func TestRejectChangedSources(t *testing.T) {
	for _, p := range []string{"/etc/profile", "/usr/sbin/shutdown", "/usr/sbin/rc0", "/usr/sbin/rc6", "/etc/motd", "/etc/vfstab", "/usr/sbin/swap"} {
		t.Run(p, func(t *testing.T) {
			es, k := fixture()
			for i := range es {
				if es[i].Path == p {
					es[i].Data = bytes.NewReader([]byte("changed"))
					es[i].Size = 7
				}
			}
			if _, err := QuadraRoot(es, bytes.NewReader(k), int64(len(k))); err == nil {
				t.Fatal("accepted changed source")
			}
		})
	}
}
func TestInvalidKernel(t *testing.T) {
	es, k := fixture()
	k[5] = 1
	if _, err := QuadraRoot(es, bytes.NewReader(k), int64(len(k))); err == nil {
		t.Fatal("accepted little endian")
	}
}

func TestEmbeddedConfigMatchesSource(t *testing.T) {
	root := "../../kernel/mac/diskroot/etc"
	if _, err := os.Stat(root); os.IsNotExist(err) {
		t.Skip("source tree unavailable")
	}
	files, err := config.ReadDir("etc")
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		t.Run(f.Name(), func(t *testing.T) {
			want, err := os.ReadFile(filepath.Join(root, f.Name()))
			if err != nil {
				t.Fatal(err)
			}
			got, err := config.ReadFile("etc/" + f.Name())
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(got, want) {
				t.Fatal("embedded configuration differs from source")
			}
		})
	}
}

func TestPolicyDirectoryOwnership(t *testing.T) {
	es, kernel := fixture()
	es = append(es, ufs.Entry{Path: "/etc/default", Kind: 'd', Mode: 0755, UID: 2, GID: 2})
	out, err := QuadraRoot(es, bytes.NewReader(kernel), int64(len(kernel)))
	if err != nil {
		t.Fatal(err)
	}
	m := entryMap(out)
	e := m["/etc/default"]
	if e.UID != 0 || e.GID != 0 || e.Mode != 0755 {
		t.Fatalf("policy parent %+v", e)
	}
	if m["/etc/default/mac"].Size == 0 {
		t.Fatal("missing policy")
	}
}
