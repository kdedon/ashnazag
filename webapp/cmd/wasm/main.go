//go:build js && wasm

package main

import (
	"amigaux.org/imagebuilder/planner"
	"syscall/js"
)

func main() {
	packageAPI := js.FuncOf(packagesJS)
	defer packageAPI.Release()
	js.Global().Set("auxPackages", packageAPI)
	forgeAPI := js.FuncOf(forgeJS)
	defer forgeAPI.Release()
	js.Global().Set("auxForge", forgeAPI)
	forgeRecipe := js.FuncOf(forgeRecipeJS)
	defer forgeRecipe.Release()
	js.Global().Set("auxForgeRecipe", forgeRecipe)
	boot := js.FuncOf(quadraBootJS)
	defer boot.Release()
	js.Global().Set("auxQuadraBoot", boot)
	rootFilesystem := js.FuncOf(rootFilesystemJS)
	defer rootFilesystem.Release()
	js.Global().Set("auxRootFilesystem", rootFilesystem)
	filesystem := js.FuncOf(filesystemJS)
	defer filesystem.Release()
	js.Global().Set("auxFilesystem", filesystem)
	assembler := js.FuncOf(assembleJS)
	defer assembler.Release()
	js.Global().Set("auxAssemble", assembler)
	fn := js.FuncOf(func(this js.Value, args []js.Value) any {
		if len(args) != 1 || args[0].Type() != js.TypeString {
			return `{"ok":false,"error":"expected a JSON string"}`
		}
		text := args[0].String()
		if len(text) > 1024*1024 {
			return `{"ok":false,"error":"request exceeds 1 MiB"}`
		}
		return string(planner.Handle([]byte(text)))
	})
	defer fn.Release()
	js.Global().Set("auxPlanner", fn)
	if ready := js.Global().Get("auxPlannerReady"); ready.Type() == js.TypeFunction {
		ready.Invoke()
	}
	select {}
}
