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
| Model textures | Meshes draw their `*BITMAP` textures; tanks draw their skin `.jpg` (e.g. `tanks/a7v.jpg`); `/sphere_` textures are sphere-mapped | Same: UVs from `Face::tcoord`, textures by name with the alpha image, the skin on every `.ase` mesh, sphere map in the vertex shader (V2) | match |
| Model materials | Per-mesh ambient/diffuse/emissive (specular is zero for `GL_LIGHT1`) from the `.ase`/MilkShape material, the "no texture" set when the model has no textures | Same, per mesh range; lit per vertex by the sun at its position with `<skyambience>`/`<skydiffuse>` and the 0.2 global ambient; no player-colour tint, as upstream has none (V3) | match |
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

**Status (2026-09-09): V1 acc9ae0, V2+V3 the commit after e3b6717.** V2
and V3 add no setting: the textures are the models' own small images and
the light is per vertex, cheaper than the per-fragment shade it replaced;
draw calls rise only by the number of distinct materials per model
(a T-55 3, a Bradley 5). Upstream's tree material (ambient 0.4, diffuse 1)
and light are applied to the port's own tree program too. Emulator: the
T-55 draws its olive skin, turret and barrel shaded by the sun.

**0.7.0 on a phone (2026-09-09) showed three things the emulator never
did, all in the image loaders rather than the renderer:** a greyscale
JPEG (the whole storm set, so the cavern's roof and ground) came out of
`ImageJpgFactory` with one channel and was read as three - rainbow static
on the ceiling, a cyan wash on the ground - fixed by widening to RGB at
load (`LandscapeTextureBuilder::toRGB`); the port's BMP loader turned rows
over, so every `.bmp` skin was upside down against the JPEG/PNG loaders
and the grey destroyer drew in its hull's navy (patch 0021); and `rand()`
was never seeded, so every launch replayed the same maps (Quick Game most
visibly). The giant carrier and destroyer on the snow and default maps
are upstream's own size: `<modelscale>` 0.04 on a 2,567-unit model is a
103-unit ship, and every tank model is now host-tested through the
sizing rule to prove bounds were not the cause. The white sea and the
"giant navy object" (blocks of broken water, not a model) were the phone's
Mali-G52 running `mediump` as real 16-bit float; every fragment shader is
highp since 0.7.2, with `setprop debug.scorchdroid.highp 0` to compare.

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

## Open: banded water on a Unihertz Titan Pocket (2026-09-11)

Reported once and **not reproducible since**, so this is a record of what was
ruled out rather than an investigation in progress. The sea rendered as broad
horizontal bands of saturated colour - green, blue, yellow, pink - each
stippled, on a landscape whose upwelling colours are two blues. The same
device also showed "a floating tank image" once; whether that is the same
fault is unknown.

The device, from the GPU line the renderer now logs at startup:

    GPU: ARM / Mali-G72 MP3
    GL: OpenGL ES 3.2 v1.r26p0-01eac0..., GLSL ES 3.20
    Fragment highp float: range 2^+-127..127, 23 bits of mantissa
    Fragment mediump float: 10 bits of mantissa

Ruled out:

- **Precision.** `highp` is real and full 32-bit float in the fragment shader,
  so the water's `exp()` fog and its `pow(x, -8)` Fresnel are not quantising.
  This was the leading theory and the log killed it.
- **The reflection target being undefined.** It is cleared, colour and depth,
  at the top of its pass. Its texture is RGB8/LINEAR/CLAMP_TO_EDGE with no mip
  filter, so it is sample-complete. Its framebuffer reported complete
  (`0x8cd5`) on the device, at 358x360 - half of that phone's near-square
  716x720 screen.
- **The sea's own colour inputs.** The landscape's upwelling colours logged as
  two blues, and the ocean tile uploaded with sane peaks (height 3.67,
  displacement 6.25, foam 0.96).

Found while looking, and fixed: the inner water grid read `textureLod` level 1
or 2 of the height tile at Water detail Low or Medium, and that tile has no mip
chain - undefined, and it feeds vertex *positions*. Garbage there does not tint
the sea, it throws the surface's vertices apart, which would show as stretched
regions of flat colour.

**Not seen since that fix** (reported 2026-09-12), which makes this the leading
explanation without being proof: it only fires at Water detail Low or Medium,
and the one log captured from the device says detail 2, where the level is
already 0 - but that log came from a different session than the screenshot, so
what the detail was set to when it banded is not known.

If it never returns, this is why. If it returns at detail 2, it was never
this, and the three tests below still stand.

If it returns, the three tests that separate the remaining possibilities, in
order of cost - the first two need no PC:

1. **Water detail** between Low and High. If the banding changes shape, the
   fault is in the surface's geometry (the vertex texture fetch), not its
   colour.
2. **Water reflections -> Sky**, which stops the shader sampling the
   reflection texture at all. If the bands go, it is the reflection target.
3. `adb shell setprop debug.scorchdroid.water 1`, which paints the water flat.
   If the bands survive that, they are not the water shader at all - and the
   floating tank becomes much more likely to be the same root cause. `0`
   restores it. The property is polled once a second, so it works on a running
   game with no rebuild.


## Closed: the pale sky, and how it was closed (2026-09-12)

Several desktop/phone pairs looked wrong in the same way - the port's sky pale
where upstream's was dark, and the sea pale with it. Every term turned out to
be faithful, and the answer is that the pale ones were taken facing the sun.

Worth recording because four plausible theories died on the way, each of which
would have been a real bug: precision (the GPU has full highp), a wrong sun
height (the sun direction is engine-space, and the height is the *third*
component - misreading the second turns a dusk sun into one below the horizon),
foam saturating the near sea (`peak foam 0.00`), and the fog colour being
defaulted rather than read (the landscape genuinely asks for 0.8 grey).

What settled it was `debug.scorchdroid.sky`, which shows one term of the sky
instead of the sum, polled once a second so a landscape already on screen can
be taken apart. On texvulcano, gradient (0.39, 0.32, 0.22) to (0.06, 0.08, 0.22),
fog 0.8 grey at density 0.001, sun 0.26 above the horizon:

- **Mode 8** (the gradient as uploaded) matched the log exactly - the sixteen
  colours arrive intact.
- **Mode 6** (the gradient row as a ramp) was a clean horizon-to-zenith ramp,
  so the height-to-colour mapping is right.
- **Mode 7** (the ray as colour) varied smoothly, `d.y` from 0.44 at the top of
  the frame to 0.1 at the bottom - about twenty degrees of elevation, as the
  field of view says it should be.
- **Mode 2** (the glow alone) was near-uniform at about 0.46. Which is
  *correct*: normalising the rays mode 7 gave and dotting them with the sun
  yields 0.93 top-left and 0.76 top-right, so the glow is 0.48 and 0.44 - a ten
  per cent spread, invisible to the eye.

So the sky is `(dark olive-navy) + 0.46 everywhere + fog toward 0.8 grey`,
which is pale, and upstream's `Hemisphere.cpp` computes the same `(dot + 1) / 4`
lift from the same normalised sun direction. Its screenshots show the same pale
halo around the moon; the moon is simply not centred in them.

The one structural difference left, if this is ever revisited: upstream applies
the glow **per vertex** of a coarse 10x10 hemisphere and interpolates, where
this port applies it **per fragment**. That changes how the lift is spread
across a triangle, not its peak, so it is not a candidate for this - but it is
the only place the two implementations still differ.

**How to check a sky again:** point the camera *away* from the sun. If it goes
dark there, the glow is behaving; if it stays pale in every direction, it is
not. That test costs nothing and would have been the first thing to ask for.

### Reopened the same day: it was not the glow either

The away-from-the-sun test was run and the sky stayed pale, where the glow
there is 0.025 and contributes nothing. So the conclusion above is wrong in
its cause, and a fifth theory died.

What the pairs actually have in common is that **none of them matched the
camera**. The desktop shots look down at the island from thirty or forty
degrees up, so their visible sky spans twenty to sixty degrees of elevation.
The phone shots sit near sea level looking horizontally, so the whole visible
sky is within about twenty-five degrees of the horizon - which is precisely
where the 2000-unit fog term dominates, in either renderer, and where both
should be pale. Earlier pairs also differed in landscape and in azimuth.

So the position is: every term in the sky has been measured and each matches
upstream, and no comparison yet taken can distinguish a real difference from
a difference in where the camera was pointing. That is not the same as
"faithful", and it is not the same as "broken".

**What would actually settle it**, if it is worth the trouble: one pair on the
same landscape with the camera at a matched height and pitch - or better,
sample the pixel colour at a known elevation angle in both and compare the
numbers rather than the impressions. Everything up to here has been an
argument about photographs.
