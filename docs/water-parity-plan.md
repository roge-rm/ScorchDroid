# Water parity plan

Goal: with every water setting at its top position, the sea looks the same as
Scorched3D's. The comparison that produced this list is against
`src/client/water/*` (Water2, Water2Patch(es), Water2Renderer, WaterWaves,
ocean_wave_generator.hpp) and `data/shaders/water.{v,f}shader`.

The generator (`porting/OceanWaves.cpp`) is already a faithful transcription of
upstream's spectrum. Every gap is in what is done with its output, so nothing
below touches gameplay and all of it is in `porting/` and `jni/renderer_jni.cpp`
plus the settings screen.

Steps are ordered so that each one is visible on its own and can be verified
before the next. W5-W12 continue the W-numbering of the earlier water work.

**Status (2026-09-09): all steps done**, one commit each - W5 05aa08c, W9
674e130, W8 00a33d5, W6+W7 46bddea (which also retired the sine sea and its
switch, since the fine detail needs the tile's normal texture from that step
on), W10a 3a2de54, W10c 9000467, W11 bc753fc. W12 needed nothing
beyond the update rate following the slider. Verified on the emulator per
step: shader compiles, the "Ocean tile uploaded" and "Water grid" log lines
carry the expected numbers, screenshots at each stage.

## Settings

Two controls, both under "Scorched3D's ocean" in Settings.

**Water detail** (slider, three positions, default Full - per the rule that a
performance trade-off is exposed, not chosen, and defaults to quality):

| Position | Grid cell over the map | Ocean updates per second |
|---|---|---|
| Full | 2 units (upstream's own) | 24 (upstream's own phase rate) |
| Half | 4 | 12 |
| Quarter | 8 (today's) | 6 |

The grid is the only part with a real GPU cost (see W11 for the triangle
counts); the update rate is the only CPU cost. One slider covers both because
a device that cannot afford one cannot afford the other.

**The existing "Scorched3D's ocean" switch is retired.** Its off position was
this port's two sine waves. Keeping it would mean carrying a second normal
path, a second foam path and a second breaker path through the shader for a
sea that is not upstream's. The Quarter position above is the cheap option in
its place: same look, a quarter of the triangles, a quarter of the FFTs. This
is a reversible call; if the sines are wanted back they are one commit away.

Nothing else needs a control. The normal map replaces four cosines with one
texture fetch, the fog change is free, the whitecaps are one more fetch, and
the breakers are a few hundred additively blended quads.

## W5 - Amplitude and axis (small; do first)

**Amplitude.** `OceanWaves::generate` normalises the tile to a unit peak and
the renderer draws it at 0.45. Upstream's height is `a = 128e-8` applied in
`phillips()` on an unnormalised transform, which comes out as:

| Generator wind (game wind × 2 + 3) | Upstream peak height (units) |
|---|---|
| 3 | 0.23 |
| 7 | 1.4 |
| 13 | 3.9 |

- `OceanWaves.cpp`: put `a` back into `phillips()` as upstream has it, delete
  the peak normalisation, and document that heights are now world units.
- `renderer_jni.cpp`: the ocean's `uWaveAmplitude` becomes 1.0. Keep the
  existing "Ocean tile uploaded ... peak height" log line; it is the check.
- `host-tests`: replace the "normalised to a unit peak" check with one that
  asserts the peak lands in the measured band for wind 3 and wind 13 and grows
  between them.

**Axis.** The tile is sampled with world z, but the spectrum's ky is engine y,
and `worldZFromEngineY` is `mapHeight - y`. So the geometric waves run
mirrored in z against the wind and against the noise scroll (which already
negates `dir[1]`). Fix at the seed: `reseed()` gets the wind as
`(dir.x, -dir.y)`, same as the noise scroll does. One line; the host-test for
wind alignment (crests perpendicular to the wind) still passes because it is
symmetric.

Verify: log shows peak ~0.2 in a calm round and ~4 in a gale; crests move with
the wind and with the fine ripples rather than against them.

## W6 - Choppy displacement (generator)

Upstream calls `compute_displacements(-2.0)`: each point is also moved
sideways by `-2 × IFFT(-i · k/|k| · h)`, Tessendorf's lambda. That is the
sharp-crest / broad-trough profile, and the whitecaps in W10 are computed from
it.

- `OceanWaves.cpp`: one more packed complex IFFT (x in the real part, z in the
  imaginary, as the slopes are packed today), scaled by upstream's -2 and by
  the same `(-1)^(x+y)` shift. Note the z component's sign flips with the axis
  fix in W5.
- Because the surface is now displaced horizontally, the analytic slope of h
  is no longer the surface normal. Upstream never uses slopes: `Water2Patch`
  takes cross products across the four 2-unit neighbours of each displaced
  point. Do the same on the CPU after the IFFTs and output `normal` (nx, ny,
  nz) instead of `slopeX/slopeZ`. This is what W7 uploads as the normal map,
  so it is not extra work.
- `Tile` becomes height, dispX, dispZ, nx, ny, nz. Two RGB16F textures
  (height + displacement; normal) instead of one.
- Vertex shader: `world.xz += disp`, `vNormal = normal`. Drop the slope path.
- `host-tests`: displacement is real and zero-mean; its peak is about twice
  the height peak; the CPU normals correlate with a central difference across
  the displaced heights.

Verify: crests visibly sharper than troughs at game wind 3+; the "peak
height" log gains a "peak displacement" figure of roughly 2×.

## W7 - The wave's own normal map for the fine detail

Upstream's `tex_normal` is not noise: `Water2Patches::generateNormalMap`
writes the tile's per-vertex normals as RGB8 with hardware mipmaps, and the
fragment shader samples that same map twice - at 1/8 tile scale (repeats every
64 units) and 1/32 (every 16 units), scrolled by the wind, re-uploaded every
phase. The fine detail is therefore wind-aligned and moving with the sea. Ours
is an isotropic lattice of cosines.

- Upload the W6 normal as RGB8 (`n × 127 + 128`, exactly upstream's encoding)
  with `glGenerateMipmap` after each `glTexSubImage2D`, `GL_REPEAT`,
  trilinear. The 16-unit layer needs the mips or it shimmers at distance.
- Fragment shader: delete `rippleSlope`. Sample the map at
  `vWorld / 2.0 * uNoiseN.z + uNoiseN.xy` (upstream halves the vertex
  position; ours did not, which is why our repeats were 32/8 rather than
  64/16), decode `* 2 - 1`, swap to Y-up, multiply by the fog factor, and
  combine as upstream does: `N = normalize(vNormal + N0 + N1)`. N0 and N1 are
  full unit normals, so this also reproduces upstream's ~3× flattening of
  every slope - which is intended, not a bug to correct.
- The second wind is randomised per landscape upstream
  (`windSpeed2 = speed1 + rand(-1,1)`, direction jittered ±0.2 on each axis,
  normalised). Replace the fixed 15° turn and 1.15× with that, seeded from the
  landscape so it is stable for a round.

Verify: a screenshot at game wind 0 shows streaks aligned across the wind, not
a regular checker of glints; the specular path is broken up the same way as
in the user's upstream islands screenshot.

## W8 - Opacity

`water.fshader`'s depth-transparency branch never runs: `landfoam` is set from
a default `Vector` (all zero), so `aoftexcoord.z` is 0 and the test fails.
Alpha is therefore `transparency`, which is the landscape's
`<watertransparency>` (1.0 everywhere shipped) unless the hide-water key is
held. Upstream water is opaque.

- `waterAlpha = water->waterTransparency` with no 0.82. Blending stays on, as
  upstream leaves it on.
- Delete the shore mask (`buildShoreMask`, `waterShoreTexture`, `uShore`) and
  the foam band with it: upstream has no shore band. Its job is taken by the
  breakers in W10c.

Verify: the drowned terrain no longer shows through in deep water; the
shoreline is the terrain meeting an opaque surface, as in upstream.

## W9 - Fog (scene-wide)

Both `land.vshader` and `water.vshader` set
`gl_FogFragCoord = max(z - 350, 0)` and the fragment shaders compute
`exp2(-density × coord × 3 × 1.442695)`, i.e. `exp(-3 × density × coord)`.
Ours is `exp(-density × depth)` from zero in every shader. With the usual
`<fogdensity>` of 0.001 that is 30% fog at 350 units where upstream has none,
and a soft horizon where upstream's sea fades hard into the fog colour.

- One shared GLSL snippet, `fogFactor(depth)`, used by the terrain, water,
  mesh, tree, instanced-mesh and roof shaders:
  `clamp(exp(-3.0 * uFogDensity * max(depth - 350.0, 0.0)), 0, 1)`.
- Upstream's coordinate is clip-space z, which for its far plane is view depth
  to within a couple of units. Use `gl_Position.w` as now.

Verify: with fog density logged, a point 350 units out is unfogged; the
horizon meets the fog colour where the far plane clips the sea.

## W10 - Foam

### W10a - Whitecaps (amount-of-foam, "AOF")

Upstream's `generateAOF`: per tile point the Jacobian of the horizontal
displacement, `J = (1 + dDx/dx)(1 + dDz/dz) - (dDz/dx)(dDx/dz)`, spawns foam
where `J < 0` (the surface folds), spreads half of it to the four neighbours,
and the whole field decays by `4/256` per phase plus a small per-cell random
term. The result is the red channel of a 128² texture sampled at `world/256`,
faded by fog, reduced by shadow, and masked by the landscape's `<foam>`
bitmap (`foam.png`, blue channel) tiled 25 times across the map. It is then
mixed towards the sun's diffuse colour, as the old shore band already was.

- Ocean worker: after the IFFTs, run the Jacobian and the accumulate/decay
  step. The decay is per upstream phase (1/24 s); ours runs at the rate set
  by the Water detail slider, so scale the decay by `dt × 24`.
- Upload as the alpha channel of the W7 RGBA8 normal texture - upstream's own
  layout (rgb normal, a foam) minus the dead depth channel. One texture, one
  upload.
- Load `foam.png`'s blue channel as an R8 texture through the vendored libpng
  path the ground texture already uses.
- Fragment shader, verbatim from `water.fshader`:
  `aof = texture(normalFoam, vWorld/256).a * fog;`
  `foam = max(min(aof, 1) - (1 - s0) * 0.5, 0) * texture(foamMask, vWorld/uMapSize * 25).r;`
  `water = mix(water, uSunDiffuse, foam);`
- `host-tests`: a folded region (J < 0) produces foam, a flat one does not;
  foam decays to zero in the expected number of phases.

Verify: whitecaps appear on the crests only at wind 3+, and fade over about a
second; a calm round has none.

### W10b - Shore band

Removed in W8. Nothing further.

### W10c - Breakers (`WaterWaves.cpp`)

Upstream draws animated breaking-wave sprites along every shoreline: it walks
the heightmap for cells just under the waterline with a neighbour just over,
chains them into paths, cuts each path into segments of random length 1-10,
and for each segment builds a quad pushed 6 units seaward along the path's
perpendicular (flipped so it faces the water). Each frame it draws three
phases of `waves.bmp` and three of `waves2.bmp` on a 6-second cycle, additive
(`GL_SRC_ALPHA, GL_ONE`), depth write off, white at alpha × 0.3, each quad
sliding seaward and back and riding the current wave height. Segments whose
perpendicular points with the wind are skipped.

- `porting/ShoreBreakers.{h,cpp}`: GL-free port of `findPoints`,
  `findNextPath`, `findPath`, `constructLines` producing a list of
  `{ptA, ptB, ptC, ptD, perp}` in engine coordinates. Deterministic seed so a
  host-test can assert on it. Rebuilt with the landscape, and after a
  deformation that touches the waterline (the deform queue already reports
  the region).
- Renderer: one VBO of the segments; vertex shader takes the phase time and
  `frontlen/endlen/alpha` from upstream's `drawBoxes` maths as uniforms, and
  samples the height texture for its z so the sprite rides the sea. Six draws
  a frame. `waves.bmp` and `waves2.bmp` through `loadAlphaImage` (luminance
  to alpha), as upstream.
- `host-tests`: on a heightmap with one island, every segment's perp points
  away from land; segment count is in the expected range; nothing off-map.

Verify: breakers along the islands' shores, fading in and out over six
seconds, none on the lee side.

## W11 - Geometry

Upstream's mesh is the tile at 2 units per vertex in 64×64 patches with
distance LOD, drawn out to the horizon, never faded. Ours is an 8-unit grid
over the map plus 400 units, faded to nothing at its edge to meet four flat
skirt quads. At upstream's amplitudes the fade would flatten a gale's sea
half way to the shore, and 8 units drops every wavelength under 16.

- Inner grid over the map plus 64 units at the Water-detail cell size. Sizes:

  | Cell | Triangles, 256-unit map (inner grid 384 units square) |
  |---|---|
  | 2 | ~74k |
  | 4 | ~18k |
  | 8 | ~4.6k |

  The 16-unit outer ring to the far plane adds about 36k at any setting.

- Outer ring to the far plane at 16-unit cells, also displaced (the tile
  repeats, so it needs no fade). The inner extent is a multiple of 16 and the
  seam is closed with a stitch strip (each outer edge segment fanned to the
  inner vertices along it), the standard clipmap join. No amplitude fade
  anywhere.
- Indexed triangles rather than the current six-vertex quads, so the 2-unit
  grid is 150k vertices not 900k.
- Distance LOD as upstream (skip levels by camera distance) is deliberately
  left out: the Water detail slider covers the same budget with far less code,
  and the stitch strip is the only join to get right.

Verify: no seam or crack at the ring boundary from a low camera; at Full the
short wavelengths that the log's peak slope implies are visible in the
geometry, not just the normal map.

## W12 - Time

Upstream bakes 256 phases over its 10.24 s cycle and steps them at 24 a
second (a 10.67 s visible cycle). Our worker runs on a real clock, which at
Full's 24 updates/s is the same motion. Nothing to change beyond making the
worker's rate follow the slider; the generator's own cycle time is already
upstream's.

## Order and verification

1. W5, W9, W8 - each a few lines, each immediately visible. Commit separately.
2. W6 + W7 together (the normal comes from the displaced geometry).
3. W10a (needs W6's displacement), then W10c.
4. W11, with the Water detail slider added when the inner grid gains its
   cell-size parameter.
5. Retire the ocean switch last, once the sine path is unused everywhere.

Each step: host-tests for anything in `porting/`; the "Ocean tile uploaded"
log line for the numbers; a screenshot beside the upstream islands
screenshots for the look. Build delivered to `/srv/downloads/temp/debug`.
