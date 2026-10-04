//go:build js && wasm

package main

import (
	"amigaux.org/imagebuilder/bootimage"
	"context"
	"fmt"
	"io"
	"syscall/js"
)

func quadraBootJS(_ js.Value, args []js.Value) (result any) {
	fail := func(err any) any { return map[string]any{"ok": false, "error": fmt.Sprint(err)} }
	defer func() {
		if err := recover(); err != nil {
			result = fail(err)
		}
	}()
	if len(args) != 4 || args[0].Type() != js.TypeNumber || args[1].Type() != js.TypeNumber || args[2].Type() != js.TypeFunction || args[3].Type() != js.TypeFunction {
		return fail("expected donor/kernel sizes and read callbacks")
	}
	ds, ks := args[0].Float(), args[1].Float()
	if ds < 512 || ds > float64(assemblyLimit) || ds != float64(int64(ds)) || ks < 52 || ks > 32*1048576 || ks != float64(int64(ks)) {
		return fail("invalid donor or kernel size (kernel maximum 32 MiB)")
	}
	kernel := make([]byte, int(ks))
	reader := jsReader{args[3], int64(ks)}
	for off := 0; off < len(kernel); {
		n := min(65536, len(kernel)-off)
		got, err := reader.ReadAt(kernel[off:off+n], int64(off))
		if err != nil && err != io.EOF {
			return fail(err)
		}
		if got != n {
			return fail(io.ErrUnexpectedEOF)
		}
		off += n
	}
	disk, err := bootimage.Build(context.Background(), jsReader{args[2], int64(ds)}, int64(ds), kernel, bootimage.Options{CommandLine: "root=c0d0s1"})
	if err != nil {
		return fail(err)
	}
	data := js.Global().Get("Uint8Array").New(len(disk))
	js.CopyBytesToJS(data, disk)
	return map[string]any{"ok": true, "bytes": len(disk), "data": data}
}
