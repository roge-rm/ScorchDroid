# Ground parity plan

How Scorched3D makes and draws its ground, against what this port does, and the
steps to close the gaps. Sources: `src/common/landscapemap` (shape),
`src/client/GLEXT/GLImageModifier.cpp` and `src/client/landscape/Landscape.cpp`
(texture), `data/shaders/land.{v,f}shader`, `src/client/land/*` (mesh, LOD,
surround), `src/client/sky/SkyRoof.cpp` (cavern roof), and the port's
`porting/LandscapeTextureBuilder.cpp` and the terrain code in
`jni/renderer_jni.cpp`.

## How upstream makes the ground

**Shape.** `LandscapeMaps::generate` and `HeightMapModifier` are common code
that runs on the server; the port hosts the same server and compiles them
unchanged. Every crater, the arena, the roof heightmap: identical by
construction. Nothing to do.

**Texture** (`Landscape::generate`, once per landscape):

1. A square RGB image of `TexureSize`: 256 at Low, **1024 at the default**,
   2048 at High. Generated landscapes go through `addHeightToBitmap`: for
   every texel, the interpolated height (jittered by the height at the
   mirrored point, up to 40%) picks one of four height bands over 0-30
   units, cross-fading over the last 60% of each band; slopes whose normal
   z drops below 0.8 fade to the rock texture over 0.3; near-flat ground
   between heights 3.5 and 5.5 fades to the shore texture. The source
   textures are read **one source pixel per output texel**, wrapping - so a
   256-pixel texture repeats every 256 texels, which at 1024 is every 64
   map units; `bitmapScale = size / 1024` rescales the sources so the
   repeat in map units is the same at every size. File-based landscapes
   just resize their image to 1024.
2. **Without hardware shadows only**: a 256² light map (half-lambert
   against the sun, ray-cast self-shadowing with a 3-unit soft edge,
   `diffuse × light + ambient`) is scaled up bilinearly and multiplied in
   **at 1.2×**. With hardware shadows the texture stays unlit and the land
   shader lights it per fragment.
3. Placement shadows are painted in, then the texture is uploaded
   **without mipmaps** (`replace(mainMap_, false)`: linear min and mag).
4. The landscape's `<detail>` image (every landscape declares one, usually
   `default/detail.jpg`, 512², baseline JPEG) is uploaded with mipmaps.

**Draw** (`land.vshader` / `land.fshader`, with hardware shadows):
`lightcolor = diffuse × (N·L × shadow) + ambient`, where L is the direction
to `GL_LIGHT1` at the sun's position, 900 units out from the map centre, so
it varies slightly across the map; colour is
`((mainmap × 3.5) + detail) / 4 × lightcolor`, with the detail sampled at
`position / 16`, one tile per 16 units. Then the same fog as the water. The
same shader draws the **surround** (the eight quads to 1536 units out, on
the first ground layer tiled 64 units, with the detail) and, via `SkyRoof`,
the cavern **roof** (the `<roof>` image once over the map, the detail on
unit 2, half-lambert lighting).

**Mesh.** One vertex per heightmap cell, normals from
`HeightMap::getNormal` (an average of cross products at distances 1 and 3),
in 32-unit patches with five LOD levels chosen by screen-space error and
stitched at the borders. Every 32 units around the arena a small model of
the wall type stands on the ground (`LandscapePoints`) and on the water
(`WaterMapPoints`).

## What the port does, and where it differs

`LandscapeTextureBuilder::build` is a texel-for-texel transcription of
`addHeightToBitmap` with the same constants; `applyLightMap` of
`addLightMapToBitmap`; the surround of `LandSurround`; the mesh is the full
grid at Full detail; the shader's lit path is upstream's formula; the bake-
or-not decision follows upstream's. The differences, ranked by what a player
sees:

| # | Upstream | Port | What it looks like |
|---|---|---|---|
| 1 | Detail texture blended into ground, surround and roof, one tile per 16 units | **Not loaded at all** (attempt parked in `docs/wip`) | Close up, the ground is smooth colour blotches with nothing at high frequency; worst on a cavern ceiling |
| 2 | Texture 1024², sources rescaled so a 256-px texture repeats every 64 units at any size | **512²**, sources at native pixel size | Half the texel density, and every source texture repeats every 128 units instead of 64 - grass and rock are twice as large-grained and blurrier |
| 3 | Light map multiplied in at 1.2×, bilinearly upscaled from 256² | 1.0×, nearest-sampled | With shadows *off* only: ground 17% darker than upstream, with a faintly blocky light map |
| 4 | Vertex normals from `HeightMap::getNormal` (crosses at distance 1 and 3, averaged) | Central difference at ±1 grid cell | Slightly sharper shading than upstream; also the mesh shades from different normals than the texture was built from |
| 5 | Light from the sun's *position* (a point 900 units out), per vertex | One directional vector | Up to ~8° of light-direction difference across a map; barely visible |
| 6 | Main texture drawn without mipmaps | Trilinear mipmaps | Upstream's distant ground is sharper and shimmers; the port's is smoother |
| 7 | Arena marked by wall-type models every 32 units, on land and on water | A sprite at each point on land, nothing on water | Different markers |
| 8 | Roof image once over the map (`SkyRoof`: coord = position / map size) | Tiled by image size | Identical on a 256 map with a 256 image; differs otherwise |

Already matching and needing nothing: the heightmap itself, the band and
slope blending, the shore band, the shadow-or-bake decision, the surround's
geometry and texture choice, the scorch marks, the fog.

## Plan

G-numbers, in the order to do them; each is visible on its own.

### G1 - Upstream's texture size and source tiling

- `LandscapeTextureBuilder::build(ctx, size)` gains upstream's
  `bitmapScale = size / 1024`: the four sources, rock and shore are resampled
  by that factor before tiling (nearest is what upstream's `createResize`
  does), so a source repeats every 64 map units at every size. File-based
  landscapes resize to `size` as now.
- Size follows the existing **Landscape detail** slider, which today only
  sets the mesh grid: Quarter 256 (upstream's Low), Half 512, Full **1024**
  (upstream's default, the faithful one). Upstream's High of 2048 is worth
  a fourth position only if the slider has room; it is not what a PC player
  sees by default.
- **Performance, and why no new setting:** 1024² is four times the work of
  today's 512² - about a million texels, each an interpolated normal and
  height. That is a second or two on a phone, done today on the GL thread
  at round start. The builder is GL-free already, so it moves to a worker
  thread started when the landscape arrives (with the light map, when it
  applies), and the GL thread only uploads when it is done; until then the
  terrain draws with the height-ramp fallback it already has. Upstream
  shows a progress bar for exactly this step. The slider covers the
  device that cannot afford it.
- Verify: log the size; a 256-pixel grass texture repeats every 64 units
  (count the repeats across a 256 map in a top-down screenshot: four).

### G2 - The detail texture

The largest visible gap. Upstream's blend, verbatim from `land.fshader`:
`((ground × 3.5) + detail) / 4`, before the lighting, at `position / 16`.

- Load `tex->detail` **on the worker of G1**, decoded to RGB bytes with the
  same `ImageFactory::loadImage` the ground sources already go through
  there; upload it on the GL thread with mipmaps, `GL_REPEAT`, trilinear,
  when the ground texture is uploaded. The parked attempt hung the GL
  thread by loading in a per-frame build step; `detail.jpg` is a baseline
  JPEG like every texture that loads fine, so the file is not the problem.
  If the hang comes back on the worker, bisect the vendored libjpeg-turbo
  build (`WITH_SIMD OFF`) against a different JPEG of the same size.
- Fragment shader: the parked patch's shader half is right and can be
  reused: `vDetailCoord = aPosition.xz / 16`, `uHasDetail`, the blend before
  the lighting branch. It applies to the ground, the surround and the roof
  in one place, as upstream applies it to all three.
- Verify: close-up ground shows the grain; a cavern ceiling stops being a
  magnified blur; no hang (the "Starting local game" stall).

### G3 - The light map's 1.2 and its upscale

Shadows-off path only. Multiply by `light × 1.2` as upstream does, and
sample the 256² map bilinearly when applying it (upstream's
`gluScaleImage`). Two lines each.

### G4 - Vertex normals from the heightmap's own

`writeTerrainVertex` (and the roof's twin) takes `hMap.getNormal(x, y)` -
upstream's own, already computed for the texture builder - instead of its
central difference. Same call gives the roof its inverted normals. At
lower Landscape detail, where the grid skips cells, it still reads the
normal at the sampled cell, which is what upstream's LOD does too.

### G5 - The sun as a positional light

Pass the sun position (already in `skyDescription.sunPosition`) and let the
terrain vertex shader compute `lightDir = normalize(sunPos − position)`, as
`land.vshader` does with `GL_LIGHT1`. The fragment shader's `uSunDir` path
for the terrain becomes that varying. Tiny.

### G6 - Texture filtering (deliberate deviation, flagged)

Upstream draws the main map with plain linear filtering and no mipmaps,
which on a phone screen at oblique angles shimmers. Keep the mipmaps, and add
anisotropic filtering (`EXT_texture_filter_anisotropic`, present on
essentially every GLES3 device) at 4× so oblique ground keeps upstream's
sharpness without the shimmer. Say so in the comment. If dan wants it
exact, it is one line to drop the mipmaps.

### G7 - The arena markers

Upstream's `MapPoints` models for the wall type (wrap, bounce, concrete)
at each 32-unit point around the arena, scaled 0.15, on the ground
(`LandscapePoints`) and, riding the sea, on the water (`WaterMapPoints`).
The port has the ground positions already and draws a sprite there. Load
the three meshes through the existing model loader and draw them instanced
at those points; the water set at the ocean tile's height, as the breakers
do. Small.

### G8 - Roof texture coordinates

Match `SkyRoof`: the `<roof>` image once across the map (`position /
mapSize`), not tiled by its pixel size. One line; only shows on the one
512-wide landscape.

### Not planned

- **Mesh LOD.** Upstream coarsens far patches by screen-space error; the
  port draws the full grid at Full, which looks the same up close and
  better at distance. The Landscape detail slider is the port's budget
  control. Reproducing the popping would be work to look worse.
- **Heightmap generation, scorch painting, the shadow/bake rule, fog:**
  already upstream's.
- **The plan and arena overlay textures:** minimap and UI, with the minimap
  work.

## Order and verification

1. G1 (with the worker) and G2 together, since the detail rides the same
   worker and upload; one commit each.
2. G3, G4, G5 - small, each a commit.
3. G6, G7, G8.

Checks: the "Ground texture built" log line for the size, screenshots
top-down for the tiling count and close up for the grain, host-tests for
the builder (it is GL-free and already tested there: add the rescale and
the 1.2). Build to `/srv/downloads/temp/debug`.
