#!/usr/bin/env python3
"""Splits the game data for the browser: what every page loads, and packs it
loads only when a game needs them.

    stage_data.py DATA STAGE OUT

DATA is upstream's data/ folder. STAGE/base becomes the page's preloaded data
(web/engine/CMakeLists.txt), and each mod in LAZY_MODS becomes OUT/<mod>.pack,
listed in OUT/packs.json with its size.

A lazy mod keeps its XML in the base: that's all the menus read (the mod list,
its presets, its bots, maps and tanks). Its textures, models and sounds are
only read once a game with it starts, and the page fetches the pack first
(prepareModData in web/app).

A pack is a 4-byte big-endian header length, a JSON header listing each file's
path and size, then the files one after another.
"""
import json
import os
import shutil
import struct
import sys

# The Apocalypse mod: 22MB, 21MB of it outside its XML.
LAZY_MODS = ["apoc"]


def main():
    data, stage, out = sys.argv[1:4]
    base = os.path.join(stage, "base")
    if os.path.isdir(base):
        shutil.rmtree(base)
    os.makedirs(out, exist_ok=True)

    lazy_roots = {os.path.join(data, "globalmods", m): m for m in LAZY_MODS}
    packs = {m: [] for m in LAZY_MODS}
    for root, dirs, files in os.walk(data):
        dirs.sort()
        mod = next((m for r, m in lazy_roots.items() if root == r or root.startswith(r + os.sep)), None)
        for name in sorted(files):
            source = os.path.join(root, name)
            relative = os.path.relpath(source, data)
            if mod is not None and not name.lower().endswith(".xml"):
                packs[mod].append((os.path.relpath(source, os.path.join(data, "globalmods", mod)), source))
                continue
            target = os.path.join(base, relative)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copy2(source, target)

    listing = {}
    for mod, entries in packs.items():
        header = json.dumps({"mod": mod, "files": [[path, os.path.getsize(src)] for path, src in entries]}).encode()
        path = os.path.join(out, f"{mod}.pack")
        with open(path, "wb") as pack:
            pack.write(struct.pack(">I", len(header)))
            pack.write(header)
            for _, src in entries:
                with open(src, "rb") as f:
                    shutil.copyfileobj(f, pack)
        listing[mod] = {"file": f"{mod}.pack", "size": os.path.getsize(path)}
        print(f"{mod}.pack: {len(entries)} files, {os.path.getsize(path) / 1048576:.1f}MB")
    with open(os.path.join(out, "packs.json"), "w") as f:
        json.dump(listing, f, indent=2)


if __name__ == "__main__":
    main()
