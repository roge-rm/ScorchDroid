# Wind parity plan

How Scorched3D creates wind and everything it does with it, against what this
port does, and the steps to close the gaps. Sources: `src/common/engine/Wind.cpp`
and every `getWind()` caller in `src/common` and `src/client`.

## How upstream creates wind

`Wind` lives in `src/common/engine`, is owned by the `Simulator`, and the port
compiles it unchanged - so creation is already identical. For the record:

- **Per level** (`Wind::newLevel`, called from `Simulator::newLevel`): the speed
  comes from the `WindForce` option - Random 0-5, Wind1..Wind5 fixed 1-5,
  Breezy 0-2, Gale 3-5, None 0 - and is always a whole number. If the speed is
  above zero the angle is random 0-360; direction is `(sin, cos)` in engine x, y,
  and that first direction is kept as the *starting* direction.
- **Over the level** (`Wind::simulate`, called every simulator step on host
  and client alike): if `WindType` is not Never, a timer counts down - 30-60 s
  for SomeTimes, 15-30 Frequently, 5-10 Constantly, 0-2 Always - and on expiry
  the angle becomes `start + 25 × (1 − cos(rand(−90..90)°))`, a swing of 0-25°
  to one side of the start. **Speed never changes during a level.** The random
  calls go through the simulator's shared generator, so host and clients agree
  without any message.
- The port exposes both options in Game Setup (World tab, as sliders) and its
  engine runs the same simulator, so changes over time already happen here.

## How upstream uses wind

| Consumer | Where | What it does | Port |
|---|---|---|---|
| Shot physics | `PhysicsParticleObject::setForces` (common) | `windDir × speed / 2.5 × windFactor`, added to gravity, /70, applied per iteration. Captured when the projectile is created. | Same code, unchanged |
| AI aiming | `TankLib::getShotTowardsPosition` (common) | Angle and power corrected by the wind's cross and along components, scaled by speed/5. | Same code, unchanged |
| Clouds | `SkyDome::simulate` | Period 100-500 s by speed; runs *against* the starting direction. | Done (`advanceClouds`), matches |
| Sea shape | `Water2::generate` | Spectrum seeded with `speed × 2 + 3` and the direction **at level start**; never regenerated. | Seeded from the **live** wind and re-seeded on every change (see gap 2) |
| Sea ripples | `Water2Renderer::generate` | Two scroll winds captured **at level start**. | Read live every frame (gap 2) |
| Breakers | `WaterWaves::draw` | Skips segments facing with the *starting* direction. | Done, matches |
| Particles | `ParticleEngine::simulate` | Any emitter with `windAffect`: `velocity += dir × speed × 80 × dt²` each frame. | **Nothing** - the port's particles ignore wind (gap 1) |
| ...which emitters | | Missile flame and smoke (`MissileActionRenderer`), landscape smoke (`Smoke.cpp`: burning tanks, napalm), water splash spray (`Water::explosion`), rain and snow (`TargetCamera`). Explosions, lasers, teleports, speech are *not* wind-affected. | Shot flame/smoke and napalm smoke exist; damaged-tank smoke, splash spray, rain, snow do not (gap 4) |
| HUD text | `GLWScorchedInfo` eWind | "Force N" (integer) or "No Wind". | "Wind: 3.0 ↗ 45°" or "Wind: none" (gap 3) |
| Wind view | `GLWWindView` | A miniature plan of the landscape with `wind.ase`, an arrow model, rotated by `−angle` over it; tooltip "Current Wind Force: N (out of 5)" plus the wall type; tapping it cycles the camera. | An 8-way text arrow; no plan view (gap 3) |

No upstream code sways trees, moves flags, or plays a wind sound.

## The gaps, ranked by what a player notices

**1. Particles ignore the wind.** Upstream's smoke streams downwind from a
burning tank and a rocket's trail bends with the wind; here every trail is
straight in any weather. This is the one gap that reads as "wind is
decorative" - the shot is pushed, the smoke that would show you why is not.

**2. The sea follows the live wind; upstream's is fixed at level start.**
With any `WindType` other than Never, every wind swing here re-seeds the ocean
spectrum: a brand-new random sea appears in one frame, and the ripple scroll
offset (computed as `speed × t`) jumps with it. Upstream generates the water
once per level from the wind at that moment and never touches it again;
its sea is oblivious to the swings. With `WindType` Always that is a jump
every second or two here.

**3. The indicator is not upstream's.** Upstream shows a whole-number force
and a continuously rotated arrow over a plan of the map; the port shows a
decimal that is always `.0`, a bearing in degrees upstream never shows, and
an arrow quantised to 45°. Upstream's dialog is also its minimap, which the
port plan lists as unscheduled work.

**4. Wind-blown things that do not exist yet.** Damaged tanks smoking
(`TargetRendererImplTank`, one puff every 0.08-0.4 s scaled by remaining
life), splash spray when a shot lands in water, and rain or snow on the three
landscapes that ask for it. None is wind code, but each is a wind consumer,
and until they exist the wind has less to blow.

Already matching and needing nothing: wind creation, the change timer,
shot physics, AI compensation, clouds, breakers, the ocean's own wind mapping
and the setup options.

## Status (2026-09-09)

- **X1 done** and **X4 done** (all three effects), in the commit after this
  note. Dan confirmed the normalised drift: "it makes sense to normalise
  drift so it looks the same everywhere ... I want it to look consistent on
  multiple devices."
- **X2 deliberately left as is**: dan wants the sea to keep following the
  live wind.
- **X3 deferred**: the indicator work is tied to the minimap, which the port
  does not have yet; to be looked at with it.

## Plan

Steps in the order to do them; each is visible on its own. X-numbers are
new, to keep them apart from the water plan's W-numbers.

### X1 - Particles follow the wind

- `Particle` in `renderer_jni.cpp` gets a `windAffect` flag. Set it where
  upstream's emitters set it: the shot's flame and smoke (both `true` in
  `MissileActionRenderer`), `eSmoke` (upstream's landscape smoke is `true`).
  Leave it false for explosions, mushroom clouds, debris, lasers, teleports,
  shield hits, wall hits and speech - upstream's are false or not emitters.
- `updateEffects`: read the simulator's wind once per frame, map its direction
  to render space as `(dir.x, 0, −dir.y)`, and add the wind term to each
  affected particle's velocity.
- **The one formula that cannot be copied verbatim.** Upstream's term is
  `dir × speed × 80 × dt × dt` per frame, which is a frame-rate-dependent
  acceleration (twice as strong at 30 fps as at 60). Use what it evaluates to
  at upstream's usual 60 fps: an acceleration of `dir × speed × 80 / 60`
  units per second squared, applied as `× dt`. That is the same drift a PC
  player sees and does not change with the phone's frame rate. Say so in the
  comment.
- No setting: one multiply-add per particle.
- Verify: in a Gale round (setup: Wind Force Gale), a shot's smoke trail
  bends downwind and a napalm fire's smoke streams to one side; in a
  Wind None round both are as now.

### X2 - The sea takes the wind at level start, and only then

- `buildWaterIfNeeded` captures the wind (speed, direction, starting
  direction) into water-owned fields when the water is built, exactly when
  upstream's `Water2::generate` reads it. `updateOceanIfNeeded` seeds the
  ocean from those fields and never re-seeds for the level; the "reseed when
  the wind moved" comparison goes. The ripple scroll (`uNoise0/1`) uses the
  captured winds, so its `speed × t` offset is continuous, as upstream's is.
- The breakers already use the starting direction; nothing to do there.
- Verify: with `WindType` Always, the HUD's wind swings every second or two
  and the sea does not flinch; the "Ocean tile uploaded" wind log line
  appears once per landscape rather than once per swing.

### X3 - Upstream's indicator

X3a, small: the HUD line becomes upstream's text - "Wind: Force 3" or
"No wind" - and the arrow becomes a Compose arrow icon rotated continuously
by the wind's own angle (its convention is already "clockwise from up", the
same as the angle slider's, as the existing comment in `MainActivity`
notes). Drop the decimal and the degrees. Poll stays at the current 100 ms,
which is well inside the fastest change period.

X3b, larger and tied to the unscheduled minimap: upstream's `GLWWindView` is a
tilted plan view of the landscape (its plan texture over a low-relief
heightmap) with the `wind.ase` arrow model rotated over it, following the
camera's own heading, tooltip giving force and wall type, tap to cycle
cameras. When the minimap is built, put the arrow on it as upstream does and
retire the text arrow. Not a wind step on its own; noted here so the two are
designed together.

### X4 - Wind consumers the port lacks

In the order upstream players meet them:

- **Damaged-tank smoke** (`TargetRendererImplTank::simulate`): while a tank
  has taken damage, one `eSmoke` puff at the turret every
  `(rand × life × 10 + 250) / 3000` seconds, wind-affected by X1. The
  renderer already has the tank list and the smoke effect; the cadence and
  the life gate are the whole job.
- **Splash spray** (`Water::explosion`): when an explosion is below the water
  surface, a spray emitter at the point - upstream's attributes are in
  `Water.cpp:176`, wind-affected - plus the splash sound it already plays.
  Needs the explosion event to carry whether it was under the water, which
  the renderer can decide itself from `waterHeight`.
- **Rain and snow** (`TargetCamera`): two camera-attached emitters with
  upstream's attributes (`TargetCamera.cpp:125` and `:139`), wind-affected,
  on the three landscapes whose `<precipitation>` asks for them. Particle
  counts are real here, so these draw from the existing Effects detail
  budget rather than a new setting; at Low they would be the first thing
  thinned, which is what upstream's low setting does to them too.

### Not planned

- Wind creation, the change timer and its network agreement: already
  upstream's own code.
- The `dt²` quirk itself: reproducing a frame-rate dependence on purpose
  would make the same round look different on different phones.

## Order and verification

1. X1, X2 - each a small change in `renderer_jni.cpp`, each visible in a Gale
   round; separate commits.
2. X3a with them, since it is the thing a player looks at to check X1.
3. X4 as three small effect additions, damaged-tank smoke first.
4. X3b when the minimap is scheduled.

No host-tests are possible for any of these - they are all renderer and HUD -
so the checks are log lines and screenshots in a Gale round, and the build
goes to `/srv/downloads/temp/debug`.
