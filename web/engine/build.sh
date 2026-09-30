#!/bin/bash
# Builds the engine as WebAssembly: web/engine/out/scorchdroid.{js,wasm,data},
# and the mod packs the page loads when a game needs them (stage_data.py).
set -e
cd "$(dirname "$0")"
. "${EMSDK:-$HOME/.local/share/emsdk}/emsdk_env.sh" >/dev/null 2>&1
embuilder build zlib >/dev/null
B="${SCORCHDROID_ENGINE_BUILD:-build}"
command -v ccache >/dev/null && export EM_COMPILER_WRAPPER=ccache
mkdir -p "$B" out
python3 stage_data.py ../../third_party/scorched3d/data "$B/stage" out
emcmake cmake -S . -B "$B" -DCMAKE_BUILD_TYPE=Release -DSCORCHDROID_WEB_DATA="$(cd "$B/stage/base" && pwd)" >/dev/null
# The preloaded data is packaged at link time, and CMake can't see the staged
# files change, so the link always runs.
rm -f "$B/scorchdroid.js"
cmake --build "$B" -j"$(nproc)"
cp "$B/scorchdroid.js" "$B/scorchdroid.wasm" "$B/scorchdroid.data" out/
ls -la out
