//go:build js && wasm

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"syscall/js"

	"amigaux.org/imagebuilder/packagebuild"
	"amigaux.org/imagebuilder/packages"
	"amigaux.org/imagebuilder/sources"
	"amigaux.org/imagebuilder/svr4"
)

func packagesJS(_ js.Value, args []js.Value) (result any) {
	fail := func(err any) string {
		b, _ := json.Marshal(map[string]any{"ok": false, "error": fmt.Sprint(err)})
		return string(b)
	}
	defer func() {
		if err := recover(); err != nil {
			result = fail(err)
		}
	}()
	if len(args) == 0 || args[0].Type() != js.TypeString {
		return fail("expected JSON request")
	}
	raw := args[0].String()
	if len(raw) > 32<<20 {
		return fail("request exceeds 32 MiB")
	}
	var request struct {
		Action        string             `json:"action"`
		Catalog       []json.RawMessage  `json:"catalog"`
		Requests      []packages.Request `json:"requests"`
		Lock          json.RawMessage    `json:"lock"`
		EnvironmentID string             `json:"environmentID"`
		PackageID     string             `json:"packageID"`
		Tier          string             `json:"tier"`
		Info          svr4.Info          `json:"info"`
		Limits        sources.Limits     `json:"limits"`
		Inputs        []struct {
			SHA256 string `json:"sha256"`
			Size   int64  `json:"size"`
		} `json:"inputs"`
	}
	decoder := json.NewDecoder(bytes.NewReader([]byte(raw)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&request); err != nil {
		return fail(err)
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF {
		return fail("expected one JSON value")
	}
	switch request.Action {
	case "resolve":
		if len(args) != 1 {
			return fail("resolve requires one JSON argument")
		}
		if request.Catalog == nil || request.Requests == nil {
			return fail("catalog and requests required")
		}
		catalog := []packages.Recipe{}
		for _, b := range request.Catalog {
			r, err := packages.DecodeRecipe(b)
			if err != nil {
				return fail(err)
			}
			catalog = append(catalog, r)
		}
		lock, err := packages.Resolve(catalog, request.Requests)
		if err != nil {
			return fail(err)
		}
		b, _ := json.Marshal(map[string]any{"ok": true, "lock": lock})
		return string(b)
	case "build":
		if len(args) != 3 || !js.Global().Get("Array").Call("isArray", args[1]).Bool() || args[2].Type() != js.TypeFunction {
			return fail("build requires read callbacks array and output callback")
		}
		lock, err := packages.DecodeLock(request.Lock)
		if err != nil {
			return fail(err)
		}
		var binding *packages.Binding
		for i := range lock.Bindings {
			b := &lock.Bindings[i]
			if b.EnvironmentID == request.EnvironmentID && b.Recipe.ID == request.PackageID && b.Tier == request.Tier {
				binding = b
				break
			}
		}
		if binding == nil {
			return fail("binding not found")
		}
		if args[1].Length() != len(request.Inputs) {
			return fail("read callback count mismatch")
		}
		inputs := map[string]packagebuild.Input{}
		for i, in := range request.Inputs {
			if in.Size <= 0 || args[1].Index(i).Type() != js.TypeFunction {
				return fail("invalid source callback or size")
			}
			if _, ok := inputs[in.SHA256]; ok {
				return fail("duplicate source digest")
			}
			inputs[in.SHA256] = packagebuild.Input{Reader: jsReader{args[1].Index(i), in.Size}, Size: in.Size}
		}
		out, err := packagebuild.Build(context.Background(), *binding, inputs, request.Info, request.Limits)
		if err != nil {
			return fail(err)
		}
		data := js.Global().Get("Uint8Array").New(len(out.Package))
		js.CopyBytesToJS(data, out.Package)
		args[2].Invoke(data)
		b, _ := json.Marshal(map[string]any{"ok": true, "bytes": len(out.Package), "receipt": out.Receipt})
		return string(b)
	default:
		return fail("unknown package action")
	}
}
