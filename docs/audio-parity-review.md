# Audio parity review

Everything Scorched3D plays, against what this port plays. The last subsystem
to get one of these: the ground, the water, the wind, the scene visuals and
`src/client` have all been swept, and sound never was — which is exactly how
the audio spam of 2026-09-09 happened, a mechanism upstream had, dropped as
unnecessary, with nothing checking.

Reviewed 2026-09-19 against the pinned upstream commit.

Sources: upstream's `src/client/sound/` (the mixer, the listener, the virtual
sources), every `SoundUtils::play*` and `VirtualSoundSource` call site in
`src/client` and `src/common`, `src/client/landscape/LandscapeMusicManager.cpp`,
and `OptionsDisplay`'s audio entries — against `porting/SoundEventQueue.*`,
`porting/AmbientSound.*`, `SoundPlayer.kt`, `MusicPlayer.kt`,
`AmbientPlayer.kt` and the sound hooks in `patches/scorched3d/`.

## How upstream plays a sound

`Sound::init` opens OpenAL and creates exactly `SoundChannels` sources (8 by
default) — a hard ceiling, since a ninth simultaneous sound has nowhere to
play. Every request becomes a `VirtualSoundSource` carrying a priority, a
position, a gain, a reference distance and a rolloff; `Sound::updateSources`
sorts the playing ones by priority and then by distance and hands the real
channels to the winners. The listener is the **camera**, updated every frame
from `MainCamera` with a position, an orientation and a velocity, so OpenAL
attenuates with distance, pans with direction and shifts pitch with velocity.

This port reproduces the first half exactly and the second half partly, which
is the theme of what follows.

## 1. Sounds raised from `src/common` — all wired

These are in code this build compiles, and each one is hooked through
`ScorchDroidAudio::pushSoundEvent*` (patches 0006, 0018, 0022). Verified by
reading every `pushSoundEvent` call site in the patched tree against every
upstream sound call in the same files.

| Site | What it plays | Port |
|---|---|---|
| `SoundAction` | any weapon's `<sound>`, positioned or not | wired (0006) |
| `Explosion` | the explosion sound, at the blast | wired (0006) |
| `WeaponNapalm` | napalm's own sound | wired (0018) |
| `WeaponAnimation` | the death column's sound (`ExplosionLaserBeamRenderer`'s `data`) | wired (0022) |
| `Lightning` | the weapon's `<sound>` at the strike | wired |
| `ShieldHit` | the shield's own hit sound | wired |
| `Teleport` | the weapon's `<sound>` at both ends | wired |
| `TanketMovement` | `movement/tankmove.wav` while a tank drives | wired |
| `TankDefenseSimAction` | shield up, shield down, battery use | wired (three sites) |
| `PlayMovesSimAction` | a weapon's `<activationsound>` | wired (0018) |

Nothing in `src/common` raises a sound this port drops.

## 2. Sounds raised from `src/client` — the port's own job

`src/client` is never compiled, so each of these has to be reproduced. This is
where the gaps are.

| Upstream site | Sound | Port |
|---|---|---|
| `ShotCountDown` | `misc/beep.wav` each second under six, `beep2.wav` under three | **done** — `countdownBeeps` in `engine_jni.cpp`, off the port's own timer |
| `GLWChannelView` | the channel's text sound (`misc/text.wav`) | **done** — `ChatStore.cpp` |
| `Water::explosion` | `misc/splash.wav` for a blast under the sea | **done** — renderer, with X4's splash |
| `TankKeyboardControlUtil` | `movement/movement.wav` plus the `turn`/`elevate`/`power` loops | **done** — the aiming servo, 2026-09-10 |
| `MissileActionRenderer` | the projectile's `<enginesound>`, **looped**, positioned, gain 0.25 | **missing — A1** |
| `Wall::wallHit` | `shield/hit2.wav` where a shot hits the arena wall | **missing — A2** |
| `ClientStartGameHandler` | `misc/play.wav` as a new game commences | **missing — A3** |
| `MainCamera` | `misc/camera.wav` | **not applicable** — it is the screenshot key's sound, and this port has no screenshot key |

## 3. The mixer

| Item | Upstream | Port | Status |
|---|---|---|---|
| Channel ceiling | `SoundChannels`, 8 OpenAL sources | 8, the same number, enforced when the queue drains | match |
| Arbitration | priority first, then distance | the same, in `drainSoundEvents` | match |
| Priorities | `eAction` 10000, `eRotation` 500, `eText` 100 (and bands this port has no subsystem for) | the same three | match |
| Attenuation | OpenAL inverse-distance from reference distance and rolloff | the same formula, clamped at the source gain | match |
| Listener position | the camera, every frame | the camera, read on the GL thread before the engine lock | match |
| **Listener orientation** | set every frame; OpenAL pans with it | **not used — every sound plays centred** | **gap — A4** |
| **Velocity / Doppler** | listener and source velocities are set | not used | gap — A5 |
| Master gain | `SoundVolume` on the listener | effects volume on every stream | match |

## 4. Music

Upstream reads a global `music.xml` mapping each client state (loading, wait,
buying, playing, shot, score) to a file and a gain, then lets a landscape
override entries through its own includes. The port reads the same file and
keys the same six states off its own game state, crossfading between loops.

The per-landscape override is **not** reproduced — and it is a confirmed
non-gap: `music.xml` is the only file in the shipped data that defines music
at all, base game and Apocalypse alike.

## 5. Ambient

Upstream's landscape `<sound>` entries become looping ambient sources
(`LandscapeSound`), gated on `NoAmbientSound` and scaled by
`AmbientSoundVolume`. The port has the same, from the same landscape XML, with
its own switch and volume (M21).

## 6. Options

| Upstream | Port |
|---|---|
| `NoSound` | "Sound effects" switch |
| `SoundVolume` | "Effects volume" |
| `NoAmbientSound` | "Ambient sound" switch |
| `AmbientSoundVolume` | "Ambient volume" |
| `SoundChannels` | fixed at upstream's default of 8, not exposed |
| `NoCountDownSound` | **no equivalent — A6** |
| `NoChannelTextSound` | **no equivalent — A6** |
| `NoBoidSound` | confirmed non-gap: it is `depricatedNoBoidSound_` upstream and nothing reads it |

Music has a switch and a volume here, which upstream keeps in `music.xml`'s
gains rather than in options.

## The plan

### A1 — the projectile engine sound

The headline. `WeaponProjectile::engineSound_` **defaults to
`data/wav/misc/rocket.wav`**, and of the 55 projectile weapons in the base
mod exactly two turn it off; Apocalypse turns it off everywhere. So in
upstream nearly every shell that flies hums while it flies, looped, positioned
on the shell, at gain 0.25 and `eMissile` priority — and this port is silent
for all of them. It is the most-heard sound in the game and the one nobody
noticed was missing, which is the sort of thing this review exists to catch.

What it needs: a looping, moving sound. Everything the port plays today is a
one-shot fired at a position, so this is the first sound that has to start,
follow a shot and stop when it lands — the shot positions are already
published to the renderer (patch 0009), and `SoundPlayer` already runs loops
for the aiming servo, so both halves exist.

### A2 — the arena wall hit

`Wall::wallHit` plays `shield/hit2.wav` where a shot strikes the arena wall.
V8 drew that flash without ever playing it. The event already reaches the
renderer, so this is a line beside the flash.

### A3 — a new game commences

`ClientStartGameHandler` plays `misc/play.wav` once as a game starts. The port
knows the same moment — it is where the music already switches state.

### A4 — stereo

Upstream hands OpenAL a listener orientation, so a shell landing on your left
is heard on your left. Every sound here plays centred: distance is honoured,
direction is not. `SoundPool.play` takes a left and a right volume, and the
renderer already publishes the camera basis, so this is a pan computed from
the camera's right vector against the sound's direction — one small change in
the queue, and it affects every sound at once.

### A5 — Doppler

Upstream sets velocities on both the listener and each source. `SoundPool`
has a playback-rate parameter, so it is *possible*, but it only matters for
sounds that move, which today is nothing and after A1 is the shell. Worth a
look once A1 lands, not before.

### A6 — the two missing mutes

`NoCountDownSound` and `NoChannelTextSound` turn off exactly the beep and the
chat blip without silencing the game. Two switches beside the audio ones.

## Order

1. **A1**, which is most of what a player would notice.
2. **A4**, which changes every sound rather than one.
3. A2 and A3, a line each.
4. A6.
5. A5, only if A1 makes it worth it.

## Verifying any of this

There are no host-tests for audio and there cannot usefully be: what these
would assert is that a file was named, which is what the logcat line already
says. The port logs one line per drained batch naming each file, so "is that
sound wired" is answerable from `adb logcat` without listening — that is how
the table above was checked, and it is the right check for A2 and A3. A1 and
A4 are about *how* something sounds, and want ears on a phone.
