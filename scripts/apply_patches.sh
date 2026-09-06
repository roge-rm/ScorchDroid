#!/usr/bin/env bash
# Idempotently applies patches/scorched3d/*.patch onto the pinned
# third_party/scorched3d submodule checkout. Run automatically by the CMake
# configure step (app/src/main/cpp/CMakeLists.txt) before any Scorched3D
# sources are compiled.
#
# The submodule itself is never edited directly and never committed with
# these changes; patches are the tracked, reviewable record of every
# Android-portability change made to upstream, and are re-applied fresh
# any time the submodule checkout is reset (e.g. `git submodule update`).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUBMODULE_DIR="$REPO_ROOT/third_party/scorched3d"
PATCH_DIR="$REPO_ROOT/patches/scorched3d"

cd "$SUBMODULE_DIR"

shopt -s nullglob
for patch in "$PATCH_DIR"/*.patch; do
    if git apply --reverse --check "$patch" 2>/dev/null; then
        # Already applied.
        continue
    fi
    echo "Applying $(basename "$patch") to third_party/scorched3d"
    git apply "$patch"
done
