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

    local marker="$submodule_dir/.scorchdroid-patches-applied"

    [ -d "$patch_dir" ] || return 0
    cd "$submodule_dir"

    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        # If the checkout already has local modifications, assume they're
        # from active development (patches already applied, plus
        # in-progress edits not yet folded into a patch file) and leave it
        # alone - re-applying patches against a tree that has drifted from
        # any single patch's exact context lines fails even when the patch
        # is effectively already present. Only a fully clean checkout
        # (matching the pinned commit exactly) gets patches applied
        # automatically.
        if [ -n "$(git status --porcelain)" ]; then
            return 0
        fi
    else
        # No git metadata. That happens inside a Docker build context: a
        # submodule's .git is a file pointing at the superproject's
        # .git/modules, which is not in the context, so git has nothing to
        # read. `git apply` itself works perfectly well outside a
        # repository - only the dirty check above needs replacing, and a
        # marker file does that job. Written only on this path, so a normal
        # checkout never gains an untracked file.
        if [ -f "$marker" ]; then
            return 0
        fi

        # A .git that git could not read is a dangling pointer, not a
        # missing one - and git apply refuses to run beside it even though
        # it needs no repository. Say so, because git's own message names
        # only the path it could not find.
        if [ -e ".git" ]; then
            echo "third_party/$submodule_name/.git points at a repository that is not here." >&2
            echo "In a Docker build, exclude **/.git from the context (see .dockerignore)." >&2
            return 1
        fi
    fi

    shopt -s nullglob
    for patch in "$patch_dir"/*.patch; do
        echo "Applying $(basename "$patch") to third_party/$submodule_name"
        git apply "$patch"
    done

    if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        touch "$marker"
    fi
}

apply_patches_for scorched3d
apply_patches_for libjpeg-turbo
