# patches/scorched3d

Android-portability patches applied to the pinned `third_party/scorched3d`
submodule checkout by `scripts/apply_patches.sh` (invoked automatically from
the CMake configure step). Applied in filename order onto the exact pinned
commit - never edit the submodule checkout directly; add a new numbered
patch instead (see the porting plan for why: this keeps "upstream commit +
these patches" as a clean, reproducible GPLv2+ corresponding-source story).

Patches fall into three kinds, and it is worth knowing which you are adding:

- **Portability** (0001-0005): make upstream code compile and run without
  SDL, without a desktop toolchain, and without a desktop process lifecycle.
- **Correctness** (0007): a real upstream bug this port is the first
  configuration to hit.
- **Hooks and visibility** (0006, 0008-0010): upstream guards
  presentation-layer work behind `#ifndef S3D_SERVER`, and this port is a
  build that *is* `S3D_SERVER` but still has a renderer and a speaker. Each
  of these adds an `#else` branch (or widens visibility) so app-owned code
  under `app/src/main/cpp/porting/` can react. They add no game logic.

The general rule for a new hook patch: keep it to the smallest possible
`#else` next to an existing `#ifndef S3D_SERVER` split, and put the actual
work in a new file under `porting/` rather than in the submodule.

## Portability

- `0001-android-libcxx-portability-fixes.patch` - libc++ vs. legacy-GCC
  differences (`fixed.hpp`'s SDL-only `Sint64` typedef, `LangString`'s
  `basic_string<unsigned int>` needing an explicit `char_traits`
  specialization under libc++).
- `0002-android-common-common-module.patch` - `src/common/common` compiling
  for Android (`S3D_SERVER=1`, SDL mutex/timer/byte-order calls replaced).
- `0003-android-lang-and-net-modules.patch` - `src/common/lang` and
  `src/common/net`; the latter's real socket/thread portability work is
  mostly in `app/src/main/cpp/porting/SDL_{net,thread}_compat.*` instead of
  this patch, since `NetBuffer.hpp` transitively supplies those compat
  headers to the whole module (see that patch's commit message).
- `0004-android-engine-module.patch` - `src/common/engine`: `SDL/SDL.h`
  swapped for the `SDL_GetTicks()` compat shim in `GameState.cpp` and
  `Simulator.cpp`, plus a forward declaration of `SDL_Event` in
  `Keyboard.hpp`. `Keyboard.cpp` is deliberately *not* compiled (it is a
  live SDL_Event-polling input tracker, which needs real Android touch
  design work rather than a portability fix - see `porting/Keyboard_stub.cpp`),
  but engine code still references the class, so only the header needed
  fixing.
- `0005-android-image-module-and-m2-fixes.patch` - `src/common/image`
  (`Image::writeToFile`'s SDL BMP writer replaced with a minimal 24bpp one
  matching `ImageBitmapFactory`'s reader) plus, importantly,
  `DefinesAssert.cpp`: `dialogAssert`/`dialogExit` called `exit(64)`, which
  is fine for a desktop binary and catastrophic in an Android process - it
  runs atexit handlers and static destructors while ART-owned threads are
  still live, surfacing as a confusing "destroyed mutex" crash on an
  unrelated thread. Now `abort()`, and errors go to logcat since `printf`
  output does not. **Consequence worth remembering: upstream "load or die"
  helpers abort rather than return null in this port.**

## Correctness

- `0007-android-client-tankaddsimaction-servermode-guard.patch` -
  `TankAddSimAction::invokeAction()`'s admin-ban block calls
  `ScorchedServer::instance()` whenever `S3D_SERVER` is defined, without the
  `context.getServerMode()` guard the logging block directly above it has.
  Upstream never hits this because a real client is never compiled with
  `S3D_SERVER`; this port is the first configuration where "`S3D_SERVER` is
  defined" and "I am the authoritative server" are different questions (see
  `porting/ClientContext.*`). Manifested as an intermittent segfault.

## Hooks and visibility

- `0006-android-audio-event-hook.patch` - `SoundAction` and `Explosion` are
  entirely client-only upstream, so nothing plays sounds in an
  `S3D_SERVER` build. Both now also push to `porting/SoundEventQueue.*`, so
  an Android audio layer can react. Note `Explosion` is the path that
  actually fires on nearly every hit; `SoundAction` looks like the
  sound-trigger class by name and almost never runs.
- `0008-android-serverloadlevel-expose-setloaded.patch` - makes
  `ServerLoadLevel::setLoaded` public. ScorchDroid's local human player is
  added directly via `TankAddSimAction` rather than by a network connect, so
  it has a non-zero destination id that no `ComsLevelLoadedMessage` will
  ever answer for - without this it is stuck in `TankState::sLoading`
  forever from the second round onwards. Real network destinations still go
  through the full message round-trip and never touch this.
- `0009-android-actioncontroller-expose-shot-positions.patch` - widens
  `ActionController::getShotAndExplosionPositions()` to also report the
  firing player id, the shot velocity, and the weapon's accessory id, all of
  which were already public on the underlying objects. The renderer needs
  them to pick the right projectile mesh (weapon model first, tank
  `projectilemodel` as fallback - that is upstream's precedence) and to
  orient it along its flight path.
- `0010-android-terrain-deform-event-hook.patch` - terrain destruction is
  simulated under `S3D_SERVER`, but the "the ground changed shape, redraw
  it" notification (`Landscape::recalculateLandscape()` +
  `VisibilityPatchGrid::recalculateLandscapeErrors`) is client-only, so
  nothing tells a renderer a crater appeared. Both `deformLandscape()` and
  `flattenArea()` now report to `porting/DeformEventQueue.*`. Two call
  sites, not one: flattening is terrain destruction too - it is how tanks
  bed into the ground at round start and after moving, falling or
  teleporting.
- `0011-android-weapon-effect-event-hook.patch` - the five weapon effects
  whose Actions simulate correctly here but whose drawing half is
  client-only: Explosion, Napalm, Laser, Lightning, ShieldHit. Each pushes
  one record to `porting/EffectEventQueue.*` carrying type, position, end
  position, size and colour; the renderer owns the look entirely.
- `0012-android-ranging-tracer-hook.patch` - `RenderTracer`'s ranging marks,
  left by the two Tracer weapons, recorded in `porting/TracerStore.*`.
- `0013-android-target-model-hook.patch` - `TargetDefinition::createTarget`
  works out a non-tank target's model, scale, brightness and rotation and
  hands all four straight to a client renderer this build compiles out, so
  they were computed and discarded. They are recorded in
  `porting/TargetModelStore.*` instead, keyed by player id.
- `0014-android-skyflash-teleport-hooks.patch` - SkyFlash (`Sky::flashSky()`)
  and Teleport (a `TeleportRenderer` sprite), both client-only objects that
  do not exist here, raised as events instead. Teleport raises two, at the
  tank's old and new positions.
- `0015-android-landscape-smoke-hooks.patch` - upstream's lingering smoke
  (`Landscape::getSmoke().addSmoke()`), raised from three client-only sites
  with three different gates, all of which are preserved: a gun's muzzle
  flash (WeaponMuzzle only), a napalm fire (rate-limited, skipped on
  `<nosmoke>`), and a tank driving (tanks only, and only when the tank
  model sets `<movementsmoke>`).
- `0016-android-remaining-visual-events.patch` - the last five effects
  upstream draws and this port did not: the mushroom cloud, thrown debris,
  the arena wall flash, the speech bubble over a tank that speaks, and the
  floating damage number over a target that is hurt. Two traps documented
  in the patch itself and worth knowing before writing another hook:
  upstream's `!getServerMode()` guards exist to stop a *hosting client*
  doing presentation twice and switch it off entirely on this port, and
  `TargetLife::getFloatPosition()` is a mirror only maintained when
  `!serverMode_`, so it reads (0,0,0) here.
- `0017-android-between-rounds-scoreboard.patch` - `ShowScoreAction` holds
  the game at the end of every round (`RoundScoreTime`) and of the match
  (`ScoreTime`); upstream's client half of it raises the score table, so
  without this the port sat through the pause showing nothing.
