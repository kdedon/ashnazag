package forge

import (
	"amigaux.org/imagebuilder/rootfs"
	"amigaux.org/imagebuilder/ufs"
	"bytes"
	"context"
	"strings"
	"testing"
)

func symbolELF(name string, typ uint16) []byte {
	b := append([]byte(nil), fixtureMedia()[3]...)
	b = append(b, make([]byte, 512)...)
	be.PutUint16(b[16:], typ)
	be.PutUint32(b[32:], 512)
	be.PutUint16(b[46:], 40)
	be.PutUint16(b[48:], 4)
	sh := func(i int, typ, off, size, link, entsize uint32) {
		x := b[512+i*40:]
		be.PutUint32(x[4:], typ)
		be.PutUint32(x[16:], off)
		be.PutUint32(x[20:], size)
		be.PutUint32(x[24:], link)
		be.PutUint32(x[36:], entsize)
	}
	sh(1, 2, 672, 32, 2, 16)
	sh(2, 3, 800, uint32(len(name)+2), 0, 0)
	sh(3, 13, 960, 4, 0, 0)
	be.PutUint32(b[512+3*40+8:], 2)
	be.PutUint32(b[688:], 1)
	b[700] = 0x11
	be.PutUint16(b[702:], 3)
	copy(b[801:], name)
	return b
}
func TestDisplayValidation(t *testing.T) {
	legacy := media(fixtureMedia())[3]
	kernel := media([][]byte{symbolELF("ds_modinit", 2)})[0]
	module := media([][]byte{symbolELF("dsuser_wrapper", 1)})[0]
	for _, tc := range []struct {
		name string
		k    Media
		m    *Media
		want string
	}{
		{"legacy", legacy, nil, ""}, {"missing", kernel, nil, "requires its matching dsuser"}, {"valid", kernel, &module, ""}, {"legacy module", legacy, &module, ""}, {"wrong module", kernel, &legacy, "relocatable"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			err := ValidateDisplay(tc.k, tc.m)
			if tc.want == "" && err != nil || tc.want != "" && (err == nil || !strings.Contains(err.Error(), tc.want)) {
				t.Fatalf("got %v, want %q", err, tc.want)
			}
		})
	}
	bad := module
	bad.Size = 100
	if ValidateDisplay(kernel, &bad) == nil {
		t.Fatal("accepted truncated module")
	}
	foreign := symbolELF("dsuser_wrapper", 1)
	be.PutUint32(foreign[512+40+20:], 48)
	be.PutUint32(foreign[512+80+20:], 38)
	be.PutUint32(foreign[704:], 16)
	foreign[716] = 0x10
	copy(foreign[816:], "missing_kernel_symbol")
	foreignMedia := media([][]byte{foreign})[0]
	if err := ValidateDisplay(kernel, &foreignMedia); err == nil || !strings.Contains(err.Error(), "missing_kernel_symbol") {
		t.Fatalf("foreign module: %v", err)
	}
	if err := ValidateDisplay(legacy, &foreignMedia); err != nil {
		t.Fatalf("stripped kernel with explicit module: %v", err)
	}
	localKernel := symbolELF("ds_modinit", 2)
	localKernel[700] = 0x01
	if err := ValidateDisplay(media([][]byte{localKernel})[0], nil); err != nil {
		t.Fatalf("local loader symbol: %v", err)
	}
	localModule := symbolELF("dsuser_wrapper", 1)
	localModule[700] = 0x01
	localMedia := media([][]byte{localModule})[0]
	if err := ValidateDisplay(kernel, &localMedia); err == nil {
		t.Fatal("accepted a local module wrapper")
	}
	oversized := module
	oversized.Size = 2<<20 + 1
	if err := ValidateDisplay(kernel, &oversized); err == nil || !strings.Contains(err.Error(), "2 MiB") {
		t.Fatalf("oversized module: %v", err)
	}
	other := media([][]byte{symbolELF("other_wrapper", 1)})[0]
	if ValidateDisplay(kernel, &other) == nil {
		t.Fatal("accepted another module")
	}
}
func TestDisplayRoot(t *testing.T) {
	for _, preset := range []string{"quadra800", "falcon030"} {
		t.Run(preset, func(t *testing.T) {
			files := fixtureMedia()
			files[3] = symbolELF("ds_modinit", 2)
			p, _ := Lookup(preset)
			s := Selection{Preset: p.ID, Machine: p.Machine, Settings: p.Settings, DSUser: true}
			if preset == "falcon030" {
				files = files[:4]
			} else {
				files = files[:5]
			}
			module := symbolELF("dsuser_wrapper", 1)
			files = append(files, module)
			if preset == "quadra800" {
				s.Provision = testSelection().Provision
				files = append(files, fixtureMedia()[5])
			}
			r, err := NewRecipe(s, digests(t, s, files))
			if err != nil {
				t.Fatal(err)
			}
			text, err := Encode(r)
			if err != nil {
				t.Fatal(err)
			}
			if decoded, err := Decode(text); err != nil || !decoded.Changes.DSUser {
				t.Fatalf("roundtrip: %v", err)
			}
			var src []rootfs.Source
			for _, m := range media(files)[:3] {
				src = append(src, rootfs.Source{Reader: m.Reader, Size: m.Size})
			}
			entries, err := rootfs.ScanLayers(context.Background(), src, rootfs.Options{})
			if err != nil {
				t.Fatal(err)
			}
			out, err := Root(context.Background(), text, entries, media(files)[3], media(files)[len(files)-1-len(s.Provision):])
			if err != nil {
				t.Fatal(err)
			}
			tampered := append([]byte(nil), module...)
			tampered[len(tampered)-1] ^= 1
			extra := media(files)[len(files)-1-len(s.Provision):]
			extra[0] = media([][]byte{tampered})[0]
			if _, err := Root(context.Background(), text, entries, media(files)[3], extra); err == nil || !strings.Contains(err.Error(), "dsuser differs") {
				t.Fatalf("module digest: %v", err)
			}
			found := false
			for _, e := range out {
				if e.Path == DisplayModulePath {
					found = true
					b := make([]byte, e.Size)
					e.Data.ReadAt(b, 0)
					if e.Kind != 'f' || e.Mode != 0644 || e.UID != 0 || e.GID != 0 || !bytes.Equal(b, module) {
						t.Fatalf("staged module %+v", e)
					}
				}
			}
			if !found {
				t.Fatal("missing staged module")
			}
			s.DSUser = false
			files = fixtureMedia()[:len(files)-1-len(s.Provision)]
			files[3] = symbolELF("ds_modinit", 2)
			s.Provision = nil
			r, err = NewRecipe(s, digests(t, s, files))
			if err != nil {
				t.Fatal(err)
			}
			text, _ = Encode(r)
			if _, err = Root(context.Background(), text, entries, media(files)[3], nil); err == nil || !strings.Contains(err.Error(), "requires its matching dsuser") {
				t.Fatalf("missing module: %v", err)
			}
		})
	}
	if _, err := stageDisplay([]ufs.Entry{{Path: "/etc/conf", Kind: 'f'}}, Media{}); err == nil {
		t.Fatal("accepted non-directory parent")
	}
}
