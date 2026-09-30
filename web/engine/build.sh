#!/bin/bash
# Builds the engine as WebAssembly: web/engine/out/scorchdroid.{js,wasm,data}.
set -e
cd "$(dirname "$0")"
. "${EMSDK:-$HOME/.local/share/emsdk}/emsdk_env.sh" >/dev/null 2>&1
embuilder build zlib >/dev/null
B="${SCORCHDROID_ENGINE_BUILD:-build}"
command -v ccache >/dev/null && export EM_COMPILER_WRAPPER=ccache
emcmake cmake -S . -B "$B" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$B" -j"$(nproc)"
mkdir -p out
cp "$B/scorchdroid.js" "$B/scorchdroid.wasm" "$B/scorchdroid.data" out/
ls -la out
