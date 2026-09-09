# Visual parity review

Everything Scorched3D draws in the 3D scene, against what this port draws,
with the aim of looking identical wherever it is technically possible.
Sources are upstream's `src/client` rendering tree (`sky`, `landscape`,
`land`, `water`, `tankgraph`, `sprites`, `graph`) and the client bodies of
`src/common/actions`, against `jni/renderer_jni.cpp` and `porting/`.

The HUD, dialogs, fonts and menus are UI, not scene, and are out of scope
here; so are sound and music.

Status words: **match** = same method and numbers; **close** = same effect,
different means, no visible difference expected; **gap** = visibly
different; **missing** = not drawn at all.

## 1. Light, sun, fog

| Item | Upstream | Port | Status |
|---|---|---|---|
| Sun position | `Sun::setPosition`: 900 units out from the map centre at `<skysunxy>`/`<skysunyz>` | Same maths in `SkyDescription` | match |
| Land light | `GL_LIGHT1` at the sun, `<skyambience>` + `<skydiffuse>`, per vertex direction, times shadow | Same (G5) | match |
| Water light | `GL_LIGHT0` at the sun, per fragment | Same | match |
| **Model light** | `GL_LIGHT1` at the sun with the mesh's own ambient/diffuse/specular material colours; optional precomputed per-vertex light | **A fixed direction (0.4, 0.82, 0.35) and `0.45 + 0.75·N·L`**, no sky colours, no materials | **gap** (V3) |
| Fog colour | Landscape `<fog>` | Same (ff0ffd4) | match |
| Fog, land and water | `max(z − 350, 0)`, `exp(−3·d·coord)` | Same (W9) | match |
| Fog, everything fixed-function | `GL_EXP2`, `exp(−(d·z)²)` from zero: sky, clouds, sun, models, trees, particles, beams | Same (ddc86a5, ab0c3ab) | match |
| Sun billboard | 60×60 at the sun, `<suncolor>`, additive unless `<nosunblend>`, fogged unless `<nosunfog>` | Same | match |

## 2. Sky

| Item | Upstream | Port | Status |
|---|---|---|---|
| Dome | Ellipsoid 2000 across, 225 tall, centre 15 below sea level under the camera; colour rows by elevation angle from the colour map's time-of-day column; `(dot + 1)/4` horizon glow; fogged | Per-pixel backdrop solving the same dome, same rows, same glow, same fog. Was not drawing at all until eca6fb5 | match |
| Stars | Hemisphere 1990/215, texture scaled 9×, alpha 0.7, fog off | The star image on the cloud plane at 1/700 scale, fog off | close |
| **Cloud layers** | **Two** layers on hemispheres 1980/210 and 1980/170, texture 0.7 alpha, tinted by the sun colour, scrolled by wind at two speeds (`xy_` and `xy_/1.5`), fogged | **One** plane layer, scrolled by wind, fogged | gap (V6) |
| Sky line | `<skyline>` cylinder texture at 1000 radius | Not drawn | none: no shipped landscape sets `<skyline>` |
| Sky flash | Hemisphere flashed white by `flashTime_` | Whole sky lifted to white by `uFlash` | close |
| Roof (cavern) | `SkyRoof`: `<roof>` image once across the map, detail texture, half-lambert, skirt hemisphere | Same image once across (G8), detail (G2), half-lambert, skirt | match |

## 3. Land

| Item | Upstream | Port | Status |
|---|---|---|---|
| Heightmap | common code | same code | match |
| Ground texture | 1024², four bands, rock, shore, sources rescaled | Same (G1) | match |
| Detail texture | `((ground·3.5) + detail)/4` at 1/16 | Same (G2) | match |
| Light map (no shadows) | 256², ×1.2, bilinear | Same (G3) | match |
| Shadow map | 2048², perspective from the sun, land + targets cast | Same framing; land, scenery and tanks cast | match |
| Normals | `HeightMap::getNormal` | Same (G4) | match |
| Texture filtering | linear, no mipmaps | mipmaps + 4× anisotropy | close (deliberate: see ground plan G6) |
| Mesh LOD | 32-unit patches, five levels by screen error | Full grid; slider for budget | close (deliberate) |
| Surround | eight quads to 1536 out, first layer tiled 64 | Same | match |
| Scorch marks | Painted into the texture with the deform map's falloff | Same | match |
| Arena markers | Wall-type model every 32 units, on land and water | Same (G7) | match |
| **Arena wall** | `Wall.cpp`: when a side is hit, a translucent quad on that side in the wall colour with the `bordershield/grid.bmp` texture scrolling, fading over time; `WallActionRenderer`: a `hit.bmp` flash at the impact | A fading flat quad on the side; no textures | gap (V8) |
| Movement overlay | `MovementMap::movementTexture` tint where the tank may drive | Same | match |
| Placement shadows | Objects darken the texture under them when no hardware shadows | Not painted; the shadow-circle sprites stand in | close |

## 4. Water

| Item | Upstream | Port | Status |
|---|---|---|---|
| Sea shape, normals, foam, breakers, transparency, reflection, shadows | Water2 subsystem | W5–W11 | match |
| Wind at level start vs live | fixed at level start | follows the live wind | deliberate (dan's choice, wind plan X2) |
| Reflection contents | Sky, land, targets, particles mirrored; clear (0, 1/16, 1/8) | Same by the Water reflections setting | match |

## 5. Tanks and targets

| Item | Upstream | Port | Status |
|---|---|---|---|
| Tank model | `.ase` hull/turret/gun, turret and gun rotate with aim | Same | match |
| **Model textures** | Meshes draw their `*BITMAP` textures; tanks draw their skin `.jpg` (e.g. `tanks/a7v.jpg`) | **Every model is a flat colour**: tanks the player colour, scenery a fixed colour; no UVs uploaded | **gap** (V2) |
| **Model materials** | Per-mesh ambient/diffuse/specular from the `.ase` | Not read | gap (V3, with the lighting) |
| Recoil | `fireOffSet_` = −0.25 on firing, the gun slides back and recovers over ~6 s | None | gap (V7) |
| Shield | Textured sphere or hemisphere (`shield.bmp`, `grid2`, `shield2` magnetic), shield colour, flash on hit | Sphere/box/hemisphere drawn, hit ring of sparks | close; textures missing (V5) |
| Parachute | Model drawn above a falling tank | Same | match |
| Names, life bars | Name in the player colour, green/black bars for life and shield, billboarded | Same, drawn by the HUD layer at projected positions | close |
| Tank arrow | `arrow.bmp` billboard 4–7 units above a tank that is not in its normal state, in the player colour | Off-screen arrow only | gap, small (V9) |
| Sight | Protractor ring + bearing marker + blade, or the old sight | Both (M22) | match |
| Shadow circles | `ShadowMap::addCircle` under tanks, targets and smoke when no hardware shadows | Sprites under tanks and targets | close |
| Burnt targets | Target swaps to its burnt model after napalm | Trees swap (snow/burnt variants); other targets via the model store | match |
| Boids, ships | Targets moved by the engine, drawn as models | Drawn as targets | match |
| Damaged-tank smoke | `Smoke` puffs | Same (X4a) | match |
| Damage text | `TextActionRenderer`: the number in red, billboarded, fading over 6 s | Same idea | close |
| Speech | `talk.bmp` bubble particle over the tank | A glyph through the label path | close |

## 6. Shots and projectiles

| Item | Upstream | Port | Status |
|---|---|---|---|
| Projectile | Weapon's model along its velocity, `<projectilescale>`, spinning if it says so | Same | match |
| Flame and smoke trail | Emitters on the shot, wind-blown | Same (X1) | match |
| Ranging tracers | `RenderTracer`: quad strips along the path in the player colour, per round | Same store, drawn as lines | close |
| Smoke tracer | `drawSmokeTracer` for the last shot's smoke path | Present | close |

## 7. Explosions and effects

Upstream's particle engine draws **textured billboards**: a quad facing the
camera with one of four texture quadrants, from a *texture set* (`data/
textureset.xml`). Explosions are the animated `exp00`–`exp0N` sets (ten
frames each, chosen by particle age), smoke is `smoke01`/`smoke02`,
napalm the animated `flames` set, teleports `trans`, rings `ring`, generic
sparks `particle`. The port draws **round point sprites** with a radial
falloff for all of them. This is the largest remaining visible difference
in the scene: an upstream explosion is a boiling fireball of frames, the
port's is a cloud of soft dots.

| Item | Upstream | Port | Status |
|---|---|---|---|
| **Explosion** | `emitExplosion`: `4·size` particles, velocity `size·2.5`, sizes to `2·size+2`, colour `<explosioncolour>`, alpha 0.8→0.1, life min–max, **animated texture set** | Core + burst of point sprites, upstream's counts | **gap** (V1) |
| Explosion ring | `emitExplosionRing` on an axis, `ring` set | Approximated | gap (V1) |
| Mushroom | `ExplosionNukeRenderer`: rising, rolling textured sprites | Silhouette from sprites | gap (V1) |
| Debris | Rock models thrown | Same | match |
| **Napalm** | Three `flames`-set particles per burning point, size 1.5, whole napalm time; scorch under | Untextured flame sprites | **gap** (V1) |
| Smoke | `smoke01` textured, alpha-blended, ground shadow circle | Round alpha sprite | gap (V1) |
| Laser | Textured beam (`waves.bmp`), tube of `sides` segments, `particle` set sparks | A line | gap (V4) |
| Lightning | Segments with texture | Lines | gap (V4) |
| Teleport | `trans` set column | Column of sprites | close |
| Shield hit | `SphereActionRenderer` flash | Ring of sparks | close |
| Splash, rain, snow, whitecaps, breakers | see water and wind plans | done | match |
| **Camera shake** | `GLCamera::addShake(<shake>)` on explosions: random offset decaying 0.06 per frame, capped at 5 | None | gap (V10) |
| Sky flash | done | done | match |

## The plan

V-numbers, ordered by how much they change the picture.

### V1 – Textured, animated particles (explosions, napalm, smoke, rings, mushroom)

- Load `data/textureset.xml` at start into named sets, each a list of
  textures (an atlas per set, or a 2D array texture: ten 64² frames is
  nothing). Keep the four-quadrant convention (`textureCoord_` picks a
  quadrant of the image).
- The particle draw becomes textured camera-facing quads rather than
  `GL_POINTS`: two triangles per particle, corners built from the view
  matrix's right/up, uv from the quadrant and, for animated sets, the frame
  from `percent_` (age over life) as `ParticleRendererQuads` does. Additive
  or alpha per particle as now.
- `Particle` gains `set`, `frame`/`quadrant`; the explosion, ring, mushroom,
  napalm, smoke and teleport spawners set them with upstream's attributes
  (all already transcribed in comments at those sites). Explosion colour,
  alpha and sizes are upstream's; the point-sprite counts stay.
- Smoke keeps its ground shadow circle (upstream adds one per smoke
  particle when `shadow_`).
- Budget: same particle counts as now; a textured quad costs no more than
  a point sprite on a tile GPU. No new setting; Effects detail already
  caps the count.

### V2 – Textured models

- `uploadModel` carries UVs; meshes with a `*BITMAP` draw it; tanks draw
  their skin `.jpg` from the `ModelID`. `ModelRendererMesh` also uses
  `GL_TEXTURE_GEN` sphere mapping for meshes flagged that way - check the
  flag and honour it.
- Untextured meshes use their material diffuse colour, which is V3.

### V3 – Model lighting and materials

- Mesh shaders light from the sun's position with `<skyambience>` and
  `<skydiffuse>`, times the mesh's ambient/diffuse material (and specular
  with its shininess), as fixed-function `GL_LIGHT1` does. Drops the fixed
  (0.4, 0.82, 0.35) direction and the `0.45 + 0.75` constants.
- Precomputed vertex lighting (`lightintense`) only applies when the option
  is on upstream; ignore.

### V4 – Laser and lightning geometry

- Laser: a tube of `sides` quads around the beam with `waves.bmp` scrolling
  and `particle` sparks at both ends, as `ExplosionLaserBeamRenderer`.
- Lightning: textured segments rather than lines.

### V5 – Shield textures

- `shield.bmp`/`shielda.bmp` alpha sphere, the `grid2` and `grid22` wire
  textures for the two shield sizes, `shield2.bmp` for magnetic shields;
  hemisphere for half shields. Geometry exists; add the textures and
  upstream's colours.

### V6 – Second cloud layer on a dome

- Two cloud layers at upstream's two speeds (`xy_` and `xy_/1.5`), on a
  hemisphere (1980 radius, 210 and 170 tall) rather than a plane, so the
  layer curves down to the horizon and fogs the way upstream's does. Tint
  by the sun colour, alpha 0.7.

### V7 – Recoil

- `fireOffSet_` −0.25 on firing, recovering at `frameTime/25`; the gun mesh
  slides along its axis by it. Tiny.

### V8 – Arena wall

- The `grid.bmp` texture on the wall quad, scrolling by `int(fade·75) % 2`,
  in the wall colour with alpha = fade; and `hit.bmp` at the impact point.

### V9 – Tank arrow

- `arrow.bmp` billboard from 4 to 7 units above a tank not in its normal
  state, in the player colour.

### V10 – Camera shake

- `addShake(<shake>)` from explosions: a random offset of up to `shake_`
  each frame, `shake_` decaying by 0.06 per frame and capped at 5. Ten
  lines in the camera.

### Not planned

- Sky line: no shipped landscape uses it.
- Mesh LOD and no-mipmap ground: deliberate, see the ground plan.
- Live-wind sea: dan's choice, see the wind plan.

## Order

1. V1 (biggest visible difference; touches every effect).
2. V2 + V3 together (one model-upload change, one shader).
3. V4, V5, V6.
4. V7–V10, each an hour.

Verification per step: emulator smoke for shader compile; screenshots
against PC captures dan can take of the same map, which is the only
judge of "identical".
