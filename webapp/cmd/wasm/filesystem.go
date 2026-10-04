//go:build js && wasm

package main

import (
	"amigaux.org/imagebuilder/provision"
	"amigaux.org/imagebuilder/recipes"
	"amigaux.org/imagebuilder/rootfs"
	"amigaux.org/imagebuilder/ufs"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"strings"
	"syscall/js"
)

type jsWriterAt struct{ write js.Value }

func (w jsWriterAt) WriteAt(p []byte, off int64) (int, error) {
	data := js.Global().Get("Uint8Array").New(len(p))
	js.CopyBytesToJS(data, p)
	w.write.Invoke(float64(off), data)
	return len(p), nil
}
func filesystemJS(_ js.Value, args []js.Value) (result any) {
	fail := func(err any) string {
		b, _ := json.Marshal(map[string]any{"ok": false, "error": fmt.Sprint(err)})
		return string(b)
	}
	defer func() {
		if err := recover(); err != nil {
			result = fail(err)
		}
	}()
	if len(args) != 4 {
		return fail("expected archive size, filesystem MiB and read/write callbacks")
	}
	for i := 0; i < 2; i++ {
		if args[i].Type() != js.TypeNumber {
			return fail("invalid size")
		}
	}
	archiveSize, sizeMiB := args[0].Float(), args[1].Float()
	if archiveSize <= 0 || archiveSize > float64(assemblyLimit) || archiveSize != float64(int64(archiveSize)) {
		return fail("archive exceeds 8 GiB or size is invalid")
	}
	if sizeMiB < 4 || sizeMiB > 2048 || sizeMiB != float64(int(sizeMiB)) || int(sizeMiB)%4 != 0 {
		return fail("filesystem size must be 4–2048 MiB in multiples of 4")
	}
	if args[2].Type() != js.TypeFunction || args[3].Type() != js.TypeFunction {
		return fail("expected read/write callbacks")
	}
	ctx := context.Background()
	entries, err := rootfs.Scan(ctx, jsReader{args[2], int64(archiveSize)}, int64(archiveSize), rootfs.Options{})
	if err != nil {
		return fail(err)
	}
	report, err := ufs.Build(ctx, jsWriterAt{args[3]}, ufs.Options{SizeMiB: int(sizeMiB)}, entries)
	if err != nil {
		return fail(err)
	}
	b, _ := json.Marshal(map[string]any{"ok": true, "report": report})
	return string(b)
}

func rootFilesystemJS(_ js.Value, args []js.Value) (result any) {
	fail := func(err any) string {
		b, _ := json.Marshal(map[string]any{"ok": false, "error": fmt.Sprint(err)})
		return string(b)
	}
	defer func() {
		if err := recover(); err != nil {
			result = fail(err)
		}
	}()
	if len(args) != 3 || args[0].Type() != js.TypeString || !js.Global().Get("Array").Call("isArray", args[1]).Bool() || args[2].Type() != js.TypeFunction {
		return fail("expected metadata JSON, read callbacks array and write callback")
	}
	if len(args[0].String()) > 1024*1024 {
		return fail("metadata exceeds 1 MiB")
	}
	var request struct {
		Mode    string `json:"mode"`
		SizeMiB int    `json:"sizeMiB"`
		Sources []struct {
			Name string `json:"name"`
			Size int64  `json:"size"`
		} `json:"sources"`
		KernelSize int64                `json:"kernelSize"`
		Provision  []provision.Artifact `json:"provision,omitempty"`
	}
	decoder := json.NewDecoder(strings.NewReader(args[0].String()))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&request); err != nil {
		return fail(err)
	}
	if decoder.Decode(new(any)) != io.EOF {
		return fail("expected one JSON request")
	}
	if request.Mode != "layers" && request.Mode != "quadra-console" {
		return fail("unsupported filesystem recipe")
	}
	if request.SizeMiB < 4 || request.SizeMiB > 2048 || request.SizeMiB%4 != 0 {
		return fail("filesystem size must be 4–2048 MiB in multiples of 4")
	}
	if len(request.Sources) == 0 || len(request.Sources) > 64 {
		return fail("choose 1–64 archives")
	}
	expectedReaders := len(request.Sources)
	if request.Mode == "quadra-console" {
		if len(request.Sources) != 3 || request.Sources[0].Name != "02" || request.Sources[1].Name != "03" || request.Sources[2].Name != "10" {
			return fail("Quadra console requires tape segments 02, 03 and 10 in order")
		}
		if request.KernelSize < 52 || request.KernelSize > assemblyLimit {
			return fail("invalid kernel size")
		}
		expectedReaders++
	}
	if len(request.Provision) > 64 {
		return fail("too many provisioning packages")
	}
	expectedReaders += len(request.Provision)
	if args[1].Length() != expectedReaders {
		return fail("incorrect number of read callbacks")
	}
	for i := 0; i < expectedReaders; i++ {
		if args[1].Index(i).Type() != js.TypeFunction {
			return fail("expected read callbacks")
		}
	}
	sources := make([]rootfs.Source, len(request.Sources))
	for i, source := range request.Sources {
		if source.Size <= 0 || source.Size > assemblyLimit {
			return fail("archive exceeds 8 GiB or size is invalid")
		}
		sources[i] = rootfs.Source{Reader: jsReader{args[1].Index(i), source.Size}, Size: source.Size}
	}
	ctx := context.Background()
	entries, err := rootfs.ScanLayers(ctx, sources, rootfs.Options{})
	if err != nil {
		return fail(err)
	}
	options := ufs.Options{SizeMiB: request.SizeMiB}
	if request.Mode == "quadra-console" {
		entries, err = recipes.QuadraRoot(entries, jsReader{args[1].Index(len(request.Sources)), request.KernelSize}, request.KernelSize)
		if err != nil {
			return fail(err)
		}
		options.Timestamp = 723000000
	}
	if len(request.Provision) > 0 {
		readers := make([]io.ReaderAt, len(request.Provision))
		start := expectedReaders - len(request.Provision)
		for i, a := range request.Provision {
			if a.Size <= 0 || a.Size > 256<<20 {
				return fail("invalid package size")
			}
			readers[i] = jsReader{args[1].Index(start + i), a.Size}
		}
		entries, err = provision.Stage(ctx, entries, request.Provision, readers)
		if err != nil {
			return fail(err)
		}
	}
	report, err := ufs.Build(ctx, jsWriterAt{args[2]}, options, entries)
	if err != nil {
		return fail(err)
	}
	b, _ := json.Marshal(map[string]any{"ok": true, "report": report})
	return string(b)
}
