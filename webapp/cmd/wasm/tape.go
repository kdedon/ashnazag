//go:build js && wasm

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"syscall/js"

	"amigaux.org/imagebuilder/amixtape"
	"amigaux.org/imagebuilder/forge"
)

// amixTapeJS splits the AMIX tape parts and returns the segments a Quadra
// build layers, in forge.TapeSegments order, with each part's digest.
func amixTapeJS(_ js.Value, args []js.Value) (result any) {
	fail := func(err any) any { return map[string]any{"ok": false, "error": fmt.Sprint(err)} }
	defer func() {
		if err := recover(); err != nil {
			result = fail(err)
		}
	}()
	if len(args) < 2 || args[0].Type() != js.TypeString || !js.Global().Get("Array").Call("isArray", args[1]).Bool() || len(args[0].String()) > 1<<20 {
		return fail("expected request JSON and read callbacks")
	}
	var req struct {
		Names []string `json:"names"`
		Sizes []int64  `json:"sizes"`
	}
	if err := json.Unmarshal([]byte(args[0].String()), &req); err != nil {
		return fail(err)
	}
	if len(req.Sizes) == 0 || len(req.Sizes) > 64 || len(req.Names) != len(req.Sizes) || args[1].Length() != len(req.Sizes) {
		return fail("choose 1–64 tape parts")
	}
	parts := make([]amixtape.Part, len(req.Sizes))
	for i, size := range req.Sizes {
		if size <= 0 || size > assemblyLimit || args[1].Index(i).Type() != js.TypeFunction {
			return fail(fmt.Sprintf("tape part %d is empty or exceeds 8 GiB", i+1))
		}
		parts[i] = amixtape.Part{Name: req.Names[i], Reader: jsReader{args[1].Index(i), size}, Size: size}
	}
	var progress func(string)
	if len(args) > 2 && args[2].Type() == js.TypeFunction {
		progress = func(id string) { args[2].Invoke(id) }
	}
	res, err := amixtape.Scan(context.Background(), amixtape.Table, parts, forge.TapeSegments, progress)
	if err != nil {
		return fail(err)
	}
	if err = res.Need(forge.TapeSegments); err != nil {
		return fail(err)
	}
	segments := make([]any, len(forge.TapeSegments))
	for i, id := range forge.TapeSegments {
		data := js.Global().Get("Uint8Array").New(len(res.Data[id]))
		js.CopyBytesToJS(data, res.Data[id])
		segments[i] = data
	}
	digests, _ := json.Marshal(res.Parts)
	return map[string]any{"ok": true, "segments": segments, "parts": string(digests)}
}
