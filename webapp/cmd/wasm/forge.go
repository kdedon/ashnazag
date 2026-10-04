//go:build js && wasm

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"syscall/js"

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
		Selection forge.Selection `json:"selection"`
		Sizes     []int64         `json:"sizes"`
		Expect    string          `json:"expect"`
	}
	if err := json.Unmarshal([]byte(args[0].String()), &req); err != nil {
		return jsonResult(nil, err)
	}
	roles := forge.Roles(req.Selection)
	if len(req.Sizes) != len(roles) || args[1].Length() != len(roles) {
		return jsonResult(nil, fmt.Errorf("expected %d inputs", len(roles)))
	}
	inputs := make([]forge.Input, len(roles))
	for i, role := range roles {
		if req.Sizes[i] <= 0 || req.Sizes[i] > assemblyLimit || args[1].Index(i).Type() != js.TypeFunction {
			return jsonResult(nil, fmt.Errorf("invalid %s input", role))
		}
		in, err := forge.Digest(context.Background(), role, jsReader{args[1].Index(i), req.Sizes[i]}, req.Sizes[i])
		if err != nil {
			return jsonResult(nil, err)
		}
		inputs[i] = in
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
