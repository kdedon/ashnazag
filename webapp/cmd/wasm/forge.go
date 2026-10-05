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

func jsonResult(v any, err error) string {
	if err != nil {
		v = map[string]any{"ok": false, "error": err.Error()}
	}
	b, _ := json.Marshal(v)
	return string(b)
}

// forgeJS lists presets and imports exported recipes.
func forgeJS(_ js.Value, args []js.Value) (result any) {
	defer func() {
		if err := recover(); err != nil {
			result = jsonResult(nil, fmt.Errorf("%v", err))
		}
	}()
	if len(args) != 1 || args[0].Type() != js.TypeString || len(args[0].String()) > 2<<20 {
		return jsonResult(nil, fmt.Errorf("expected a JSON request"))
	}
	var req struct {
		Action string `json:"action"`
		Recipe string `json:"recipe"`
	}
	if err := json.Unmarshal([]byte(args[0].String()), &req); err != nil {
		return jsonResult(nil, err)
	}
	switch req.Action {
	case "presets":
		return jsonResult(map[string]any{"ok": true, "formatVersion": forge.FormatVersion, "forgeVersion": forge.Version, "presets": forge.Presets()}, nil)
	case "import":
		r, err := forge.Decode([]byte(req.Recipe))
		if err != nil {
			return jsonResult(nil, err)
		}
		s, _ := r.Selection()
		out := map[string]any{"ok": true, "recipe": r, "selection": s}
		if err := s.Runnable(); err != nil {
			out["blocked"] = err.Error()
		}
		if r.ForgeVersion != forge.Version {
			out["warning"] = fmt.Sprintf("Recipe from forge %s; this is %s, so the image may differ.", r.ForgeVersion, forge.Version)
		}
		return jsonResult(out, nil)
	}
	return jsonResult(nil, fmt.Errorf("unknown action %q", req.Action))
}

// forgeRecipeJS hashes the inputs and returns the canonical recipe.
func forgeRecipeJS(_ js.Value, args []js.Value) (result any) {
	defer func() {
		if err := recover(); err != nil {
			result = jsonResult(nil, fmt.Errorf("%v", err))
		}
	}()
	if len(args) != 2 || args[0].Type() != js.TypeString || !js.Global().Get("Array").Call("isArray", args[1]).Bool() {
		return jsonResult(nil, fmt.Errorf("expected request JSON and read callbacks"))
	}
	var req struct {
		Selection forge.Selection   `json:"selection"`
		Sizes     []int64           `json:"sizes"`
		Expect    string            `json:"expect"`
		TapeParts []amixtape.Digest `json:"tapeParts"`
	}
	if err := json.Unmarshal([]byte(args[0].String()), &req); err != nil {
		return jsonResult(nil, err)
	}
	// The tape's segments come first, then one file per later role.
	roles := forge.Roles(req.Selection)
	n := len(forge.TapeSegments)
	if len(req.Sizes) != len(roles)-1+n || args[1].Length() != len(req.Sizes) {
		return jsonResult(nil, fmt.Errorf("expected %d inputs", len(roles)-1+n))
	}
	media := make([]forge.Media, len(req.Sizes))
	for i, size := range req.Sizes {
		if size <= 0 || size > assemblyLimit || args[1].Index(i).Type() != js.TypeFunction {
			return jsonResult(nil, fmt.Errorf("invalid %s input", roles[max(0, i-n+1)]))
		}
		media[i] = forge.Media{Reader: jsReader{args[1].Index(i), size}, Size: size}
	}
	parts := req.TapeParts
	if parts == nil {
		// Split segments are their own parts.
		for _, m := range media[:n] {
			in, err := forge.Digest(context.Background(), forge.TapeRole, m.Reader, m.Size)
			if err != nil {
				return jsonResult(nil, err)
			}
			parts = append(parts, amixtape.Digest{Size: in.Size, SHA256: in.SHA256})
		}
	}
	tape, err := forge.TapeInput(context.Background(), media[:n], parts)
	if err != nil {
		return jsonResult(nil, err)
	}
	inputs := []forge.Input{tape}
	for i, m := range media[n:] {
		in, err := forge.Digest(context.Background(), roles[i+1], m.Reader, m.Size)
		if err != nil {
			return jsonResult(nil, err)
		}
		inputs = append(inputs, in)
	}
	r, err := forge.NewRecipe(req.Selection, inputs)
	if err != nil {
		return jsonResult(nil, err)
	}
	text, err := forge.Encode(r)
	if err != nil {
		return jsonResult(nil, err)
	}
	out := map[string]any{"ok": true, "recipe": string(text)}
	if req.Expect != "" {
		want, err := forge.Decode([]byte(req.Expect))
		if err != nil {
			return jsonResult(nil, err)
		}
		out["mismatches"] = forge.Mismatches(want.Inputs, inputs)
	}
	return jsonResult(out, nil)
}
