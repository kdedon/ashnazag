#!/bin/sh
set -eu
cd "$(dirname "$0")"
GO=${GO:-go}
mkdir -p build/site
cp ../etc/default/mac ../etc/default/tos ../etc/default/amiga provision/policies/
"$GO" test ./...
"$GO" build -trimpath -o build/auxplan ./cmd/auxplan
"$GO" build -trimpath -o build/ashbuild ./cmd/ashbuild
"$GO" build -trimpath -o build/ashfs ./cmd/ashfs
"$GO" build -trimpath -o build/ashpkg ./cmd/ashpkg
GOOS=js GOARCH=wasm "$GO" build -trimpath -o build/site/planner.wasm ./cmd/wasm
goroot=$("$GO" env GOROOT)
runtime="$goroot/lib/wasm/wasm_exec.js"
if [ ! -f "$runtime" ]; then
    runtime="$goroot/misc/wasm/wasm_exec.js"
fi
cp "$runtime" build/site/wasm_exec.js
cp index.html style.css app.js worker.js assembly.js image-worker.js storage.js filesystem.js filesystem-worker.js quadra.js quadra-worker.js build/site/
touch build/site/.nojekyll
printf 'Site: %s/build/site\n' "$PWD"
