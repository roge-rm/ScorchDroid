#!/usr/bin/env bash
# Idempotently applies patches/<submodule>/*.patch onto the corresponding
# pinned third_party/<submodule> checkout. Run on every build by the
# "applyScorchedPatches" Gradle task (app/build.gradle.kts) - not from
# CMake's execute_process(), which only re-runs on reconfigure and can
# silently skip this if the submodule checkout is reset in between builds.
#
# Submodules are never edited directly and never committed with these
# changes; patches are the tracked, reviewable record of every change made
# to upstream, and are re-applied fresh any time a submodule checkout is
# reset (e.g. `git submodule update`).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

apply_patches_for() {
    local submodule_name="$1"
    local submodule_dir="$REPO_ROOT/third_party/$submodule_name"
    local patch_dir="$REPO_ROOT/patches/$submodule_name"

    [ -d "$patch_dir" ] || return 0
    cd "$submodule_dir"

    # If the checkout already has local modifications, assume they're from
    # active development (patches already applied, plus in-progress edits
    # not yet folded into a patch file) and leave it alone - re-applying
    # patches against a tree that has drifted from any single patch's exact
    # context lines fails even when the patch is effectively already
    # present. Only a fully clean checkout (matching the pinned commit
    # exactly) gets patches applied automatically.
    if [ -n "$(git status --porcelain)" ]; then
        return 0
    fi

    shopt -s nullglob
    for patch in "$patch_dir"/*.patch; do
        echo "Applying $(basename "$patch") to third_party/$submodule_name"
        git apply "$patch"
    done
}

apply_patches_for scorched3d
apply_patches_for libjpeg-turbo
