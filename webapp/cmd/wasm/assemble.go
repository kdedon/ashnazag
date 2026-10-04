//go:build js && wasm

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"syscall/js"

	"amigaux.org/imagebuilder/diskimage"
)

const assemblyLimit int64 = 8 * 1024 * 1024 * 1024

type jsReader struct {
	read js.Value
	size int64
}

func (r jsReader) ReadAt(p []byte, off int64) (int, error) {
	if off < 0 || off >= r.size {
		return 0, io.EOF
	}
	n := len(p)
	if int64(n) > r.size-off {
		n = int(r.size - off)
	}
	data := r.read.Invoke(float64(off), n)
	if data.Get("byteLength").Int() != n {
		return 0, io.ErrUnexpectedEOF
	}
	js.CopyBytesToGo(p[:n], data)
	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}

type jsWriter struct{ write js.Value }

func (w jsWriter) Write(p []byte) (int, error) {
	data := js.Global().Get("Uint8Array").New(len(p))
	js.CopyBytesToJS(data, p)
	w.write.Invoke(data)
	return len(p), nil
}

func assembleJS(_ js.Value, args []js.Value) (result any) {
	fail := func(err any) string {
		b, _ := json.Marshal(map[string]any{"ok": false, "error": fmt.Sprint(err)})
		return string(b)
	}
	defer func() {
		if err := recover(); err != nil {
			result = fail(err)
		}
	}()
	if len(args) != 6 {
		return fail("expected sizes and read/write callbacks")
	}
	sizes := [3]int64{}
	for i := range sizes {
		if args[i].Type() != js.TypeNumber {
			return fail("invalid size")
		}
		v := args[i].Float()
		if v <= 0 || v > float64(assemblyLimit) || v != float64(int64(v)) {
			return fail("invalid size or 8 GiB limit exceeded")
		}
		sizes[i] = int64(v)
	}
	if sizes[0]+sizes[1]+sizes[2] > assemblyLimit {
		return fail("prepared-image assembly is limited to 8 GiB")
	}
	for i := 3; i < 6; i++ {
		if args[i].Type() != js.TypeFunction {
			return fail("expected callbacks")
		}
	}
	_, err := diskimage.Assemble(context.Background(), jsWriter{args[5]}, jsReader{args[3], sizes[0]}, sizes[0], jsReader{args[4], sizes[1]}, sizes[1], diskimage.Options{SwapBytes: sizes[2]})
	if err != nil {
		return fail(err)
	}
	return `{"ok":true}`
}
