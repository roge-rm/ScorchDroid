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

# If the checkout already has local modifications, assume they're from
# active development (patches already applied, plus in-progress edits not
# yet folded into a patch file) and leave it alone - re-applying patches
# against a tree that has drifted from any single patch's exact context
# lines fails even when the patch is effectively already present. Only a
# fully clean checkout (matching the pinned commit exactly) gets patches
# applied automatically.
if [ -n "$(git status --porcelain)" ]; then
    exit 0
fi

shopt -s nullglob
for patch in "$PATCH_DIR"/*.patch; do
    echo "Applying $(basename "$patch") to third_party/scorched3d"
    git apply "$patch"
done
