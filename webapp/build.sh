#!/bin/sh
set -eu
cd "$(dirname "$0")"
GO=${GO:-go}
mkdir -p build/site
cp ../etc/default/mac ../etc/default/tos ../etc/default/amiga provision/policies/
# Boot code and test fixtures are built from source (CROSS=<m68k prefix>).
sh ../kernel/atari/mkboot.sh build/atari
SVIDEL=0 sh ../kernel/atari/mkboot.sh build/atari-nosv
cp build/atari/axbload.bin build/atari/bootsec.bin atari/
cp build/atari-nosv/axbload.bin atari/axbload-nosv.bin
python3 atari/mkfixtures.py build/atari/bootsec.bin build/atari/axbload.bin atari/testdata
"$GO" test ./...
"$GO" build -trimpath -o build/auxplan ./cmd/auxplan
"$GO" build -trimpath -o build/ashbuild ./cmd/ashbuild
"$GO" build -trimpath -o build/ashfs ./cmd/ashfs
"$GO" build -trimpath -o build/ashpkg ./cmd/ashpkg
"$GO" build -trimpath -o build/ashforge ./cmd/ashforge
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
