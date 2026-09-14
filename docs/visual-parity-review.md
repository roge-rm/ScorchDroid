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

### V4 – Laser and lightning geometry — **laser done 2026-09-14, lightning built but unseen**

~~Laser: a tube of `sides` quads around the beam with `waves.bmp` scrolling
and `particle` sparks at both ends, as `ExplosionLaserBeamRenderer`.~~
**That was wrong** - it describes `ExplosionLaserBeamRenderer`, the blue
column a tank dies in, not the laser weapon. `Laser::draw` is:

- two `gluCylinder` tubes along the beam, untextured and additive: an inner
  one of `0.05/2 x hurtRadius` with **3** sides in white, and an outer of
  `0.2/2 x hurtRadius` with **5** sides in the weapon's own `<color>`. A
  bright core inside a coloured sheath.
- alpha `(1 - age/totaltime) * 0.5`, which under additive blending is the
  same as dimming the colour by it.
- if `<ringradius>` is set, textured quads of that size threaded along the
  beam every 1 unit, double-sided, from the `<ringtextureset>` (default
  `"ring"`). Both shipped lasers set it: 1.5 and 1.75.

Lightning is a camera-facing textured ribbon per branch: half-width
`0.4 x segment.size`, laid across the segment direction and the direction of
the eye, white and additive, over `data/textures/lightning.bmp`.

**Done:** the laser's two tubes and its rings, and both beams now fade over the weapon's
real `<totaltime>` rather than the 0.5s/0.6s constants the port invented when
it had nothing better. The laser also draws in its actual colour at last -
the event hardcoded a pale pink, so the shipped Laser, which is **red**, had
never looked it.

**The rings are done too**, and the atlas turned out to be the answer rather
than the obstacle. They are quads from a named texture set, blended
additively - which is what a particle is here. So they go into the particle
buffer and ride the particle program, which already samples that 2D array by
layer. No second program, no copy of the texture, no extra draw call. The
only thing that differs from a particle is orientation: a particle faces the
camera, a ring stands square across the beam, a gate the beam passes through.

The set name travels in the event's `texture` field - the one the explosion
events already use for exactly this - and the renderer resolves it through
the atlas' own name table, taking the set's texture 0 as upstream does rather
than a frame chosen by age.

Worth knowing when looking for them: **from the side a ring is edge-on**, so
a beam viewed across its length shows the tubes and little else. They read as
squares when the beam runs towards or away from the camera. That is
upstream's geometry, not a fault.

**Lightning is built but has not been seen.** The ribbon, its texture and its
fade are all in, and the texture loads - but the Lightning weapon is
`<armslevel>10</armslevel> `and does not appear in a Target Practice shop, so
there was no way to fire one. It wants a game whose arms level reaches it.

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

### V7 – Recoil — spiked 2026-09-14, **not worth building**

- `fireOffSet_` −0.25 on firing, recovering at `frameTime/25`; the gun mesh
  slides along its axis by it. Tiny.

Built it to find out, then reverted it. The code is easy and faithful -
upstream translates by `fireOffSet_` *after* the elevation rotation
(`ModelRendererTank::draw`), so the gun slides along the barrel it is
actually pointing; in this port the barrel is -Z rather than upstream's +Y
after the upload's axis remap, so the sign flips with it. The trigger is the
`#else` this port already owns in `PlayMovesSimAction::tankFired`, exactly
where upstream calls `renderer->fired()`.

**The problem is the magnitude, and it is upstream's own.** `fireOffSet_` is
a fixed value in *model* units, applied inside the tank's own scale beside
`gunOffset_` - but tank models differ enormously in what a model unit means.
`uploadModel` normalises every model to a 2.2-unit diagonal (`scale = 2.2 /
size`, upstream's `ModelRendererTank::setup` rule), so the recoil as a
fraction of the tank is simply `0.25 / rawSize`. Measured across the three
tank models in one game:

| uploaded scale | raw diagonal | recoil as a fraction of the tank |
|---|---|---|
| 0.114 | 19 | 1.3% |
| 0.006 | 367 | 0.07% |
| 0.004 | 550 | 0.05% |

So on one tank it is a pixel or two at a normal camera distance, and on the
next it is nothing at all - a thirty-fold spread nobody chose, because a
constant in model units is meaningless across models whose coordinates
differ by that much. Faithfully reproducing it reproduces the inconsistency.
If recoil is ever wanted, it should be a fraction of the model's own size
rather than upstream's constant, and that is a deliberate deviation to
decide rather than a port of V7.

**Two claims in the first version of this note were wrong, and are corrected
here.** Both came from testing entirely inside the buying phase:

- Tanks upload no model while the buying period is on, so "tanks rendered no
  model at all" was the buying phase behaving normally, not a bug.
- Consequently every `Model uploaded` line seen was an arena marker, a ship
  or a missile, none of which has a turret. The hull/turret/gun split **does**
  work on real tanks - the same game reports `turret 48, gun 120`,
  `turret 12, gun 1608` and `turret 100, gun 316` once play starts.

**For checking anything about how tanks are drawn, start a Target Practice
game**: it begins immediately, with no buying phase to sit through.

### V8 – Arena wall — **done 2026-09-14**

- The `grid.bmp` texture on the wall quad, scrolling by `int(fade·75) % 2`,
  in the wall colour with alpha = fade; and `hit.bmp` at the impact point.

"Scrolling" overstates it. Upstream's `rot` is commented out and pinned to
zero (`int rot = 0;//int(fade * 75) % 2;`); what survives is `pos`, which
flips between 0 and 5 as the fade counts down and is added to **only the two
top corners**, so the grid shears back and forth rather than sliding. At
`int(fade * 75) % 2` that alternates about 37 times a second over the two
seconds a panel lasts - a shimmer, not a scroll. The asymmetry is upstream's
and is reproduced; the commented-out block above it says plainly that it was
being experimented with.

Both textures load with the file as **its own mask**, which is how upstream
builds them (`Wall`'s `ImageID` and `WallActionRenderer::init` each pass the
path twice), so the alpha is the image's own luminance.

The panel also **stopped being additive**. This port had it on
`GL_SRC_ALPHA, GL_ONE` with the fade multiplied into the colour, where
upstream leaves `GLSetup`'s ordinary `GL_SRC_ALPHA,
GL_ONE_MINUS_SRC_ALPHA` alone and puts the fade in the alpha - so a
concrete-grey wall was glowing rather than tinting.

The splash is upstream's own: a 40-unit square lying in the wall's plane,
centred on the impact, drawn twice - a little either side of the plane, with
the winding and the texture reversed between them - so it reads from both
directions without fighting the panel for depth. Its offset walks 0.1 to 0.4
in steps of 0.02 per hit and wraps, as upstream's static does, so two
splashes near each other do not land in the same plane. It fades over two
seconds (`frameTime / 2`). Upstream's texture coordinates put `u` on the
*height* and `v` on the horizontal, turning the splash a quarter turn;
reproduced rather than tidied.

All three of these now share one textured-quad program with V9's arrow, since
each is a flat tinted picture standing in the world.

**Verified as far as the emulator allows.** Forcing the fade on shows the
grid over the whole upper view where an unforced frame has clean sky, and
both passes build the right geometry - 24 vertices for four panels, 12 for
the splash drawn twice. What has *not* been seen is a real in-game wall hit:
getting a shot to reach the arena boundary through canyon terrain defeated a
dozen attempts, and from inside the arena the splash is a 40-unit square
several hundred units away. The event path itself is unchanged from the
flash this port already had.

### V9 – Tank arrow — **done 2026-09-14**

- `arrow.bmp` billboard from 4 to 7 units above a tank not in its normal
  state, in the player colour.

That one-liner was half the story. `drawArrow()` is called from **two**
places in `drawParticle`: once for a tank that is visible but not `sNormal`,
which given `getVisible()` means a tank still shopping; and once during play
for any tank that is not yours - and for yours too, once the camera is not
one of `CamAim`/`CamShot`/`CamTank`/`CamAction`/`CamExplosion`, the five that
already frame it. Both sit under `OptionsDisplay::getDrawPlayerColor()`.

Drawn in GL rather than Compose, unlike the name plate and the health bar
beside it: those are text, which the renderer has no font for, while this is
a world-space textured billboard that upstream depth-tests. It keeps
upstream's `glDepthMask(GL_FALSE)` with the test left on, so ground in front
hides it and it occludes nothing itself.

Three deviations, each marked in the code:

- **Width is fixed at 1.4 units** rather than upstream's
  `aspect * 0.8`. That factor is what a 16:9 desktop works out to; on a phone
  held in portrait it would pinch the arrow to a third of the width
  Scorched3D's own players see.
- **Your own arrow appears from Free and Top only**, this port's two presets
  that stand back, since its camera modes are its own rather than upstream's
  five.
- It is skipped for a tank that is not visible, which upstream gets for free
  by bailing out of `drawParticle` before it ever reaches this.

Settable: **Display → HUD → Tank arrows**, its own switch beside Name plates
and Health bars. A switch rather than one slider folding all three together,
because upstream keeps its three equivalents apart (`getDrawPlayerColor`,
`getDrawPlayerName`, `getDrawPlayerHealth`) and the combinations are not an
ordered scale - arrows without names is as reasonable as names without
arrows.

Two traps worth recording. `data/images/arrow.bmp` and its `arrowi.bmp` mask
live in the **base data directory**, not under a mod, so they need
`eDataLocation`; `loadSkyTexture` assumed `eModLocation` and silently found
nothing. And every `Image` here is bottom-up, so `v = 0` belongs at the
*bottom* of the quad - mapped the other way the arrow drew upside down,
which is exactly what it did first time.

### V10 – Camera shake — **done 2026-09-14**

- `addShake(<shake>)` from explosions: a random offset of up to `shake_`
  each frame, `shake_` decaying by 0.06 per frame and capped at 5. Ten
  lines in the camera.

**Every explosion asks; almost none gets it.** `addShake` is the last line of
Explosion's client body, so it runs for every blast - but `shake_` defaults
to 0 and comes only from a weapon's `<explosionshake>`, which appears five
times in the shipped mod and once as `0.0`:

| Weapon | `<explosionshake>` |
|---|---|
| Nuke | 4.0 |
| Baby Nuke | 2.0 |
| Death's Head | 2.0 |
| Funky Bomb | 1.0 |
| Baby Ring | 0.0 |

So four weapons shake the screen and nothing else does - a reward for the
expensive ordnance rather than ambient feedback. It accumulates and pins at
5, which is what stops a cluster weapon like a Death's Head, exploding many
times over, from tearing the view apart.

Three details reproduced rather than tidied:

- **The decay is not per frame.** Upstream runs it inside a fixed 0.03s
  accumulator, commented "constant changes, regardless of framerate", losing
  0.06 a step - 2.0 a second, so a Nuke shakes for about two seconds at any
  frame rate.
- **The offset rides on the look-at only**, never the eye
  (`GLCamera::draw` adds `shakeV_` to `look`), so the camera stays put and
  its aim shudders.
- **`RAND` is 0..1, not -1..1**, so the offset is one-sided: the view leans
  into one corner as it shakes rather than jittering about its centre.
  Lopsided, and easy to "fix" by accident, so it is called out in the code.

The value reaches the renderer on the explosion event (patch 0025) in its own
field, since explosions already spend `value` on patch 0019's splash flag.

Verified on the emulator with a Nuke: the budget arrives as 4.0 and decays,
the offsets are all positive as upstream's are, and consecutive frames of a
**land-only** crop - static geometry, no sky or water to animate - differ
3.6x more during the shake than at rest. One caveat, and it is this port's
not upstream's: `delta` is clamped to 0.1s a frame, so on a frame heavier
than that the shake decays in frame-time rather than wall-clock. A nuke on
the software renderer drops well below 10fps and stretched a two-second
shake to about six. At 60fps nothing clamps.

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
