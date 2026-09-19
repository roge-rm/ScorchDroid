# Audio parity review

Everything Scorched3D plays, against what this port plays. The last subsystem
to get one of these: the ground, the water, the wind, the scene visuals and
`src/client` have all been swept, and sound never was — which is exactly how
the audio spam of 2026-09-09 happened, a mechanism upstream had, dropped as
unnecessary, with nothing checking.

Reviewed 2026-09-19 against the pinned upstream commit, and everything it
found was built the same day - see the plan at the end for what each item was
and how it was checked.

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
from `MainCamera` with a position and an orientation, so OpenAL attenuates
with distance and pans with direction. It sets a listener *velocity* too, and
that one goes nowhere: `SoundListener::setVelocity`'s body is commented out
upstream, "to prevent linux crackling sounds". Source velocities are set for
real, so upstream's doppler shift is the source's alone.

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
| `MissileActionRenderer` | the projectile's `<enginesound>`, **looped**, positioned, gain 0.25 | **done — A1** |
| `Wall::wallHit` | `shield/hit2.wav` where a shot hits the arena wall | **done — A2** |
| `ClientStartGameHandler` | `misc/play.wav` when a move of yours is granted | **done — A3** |
| `MainCamera` | `misc/camera.wav` | **not applicable** — it is the screenshot key's sound, and this port has no screenshot key |

## 3. The mixer

| Item | Upstream | Port | Status |
|---|---|---|---|
| Channel ceiling | `SoundChannels`, 8 OpenAL sources | 8, the same number, enforced when the queue drains | match |
| Arbitration | priority first, then distance | the same, in `drainSoundEvents` | match |
| Priorities | `eAction` 10000, `eRotation` 500, `eText` 100 (and bands this port has no subsystem for) | the same three | match |
| Attenuation | OpenAL inverse-distance from reference distance and rolloff | the same formula, clamped at the source gain | match |
| Listener position | the camera, every frame | the camera, read on the GL thread before the engine lock | match |
| **Listener orientation** | set every frame; OpenAL pans with it | the camera's right vector, projected onto each sound (A4) | match |
| Velocity / Doppler | the **source's** velocity is set; the listener's is commented out upstream | the same shift, from the source's velocity alone (A5) | match |
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
| `NoCountDownSound` | "Countdown beeps" switch (A6) |
| `NoChannelTextSound` | "Message sound" switch (A6) |
| `NoBoidSound` | confirmed non-gap: it is `depricatedNoBoidSound_` upstream and nothing reads it |
| *(none)* | "Turn sound" — the port's own, by dan's ask. `play.wav` is the sound heard most often here, once every round, and upstream gives it no switch; this is a client-only change and touches nothing the engine or the wire sees |

Music has a switch and a volume here, which upstream keeps in `music.xml`'s
gains rather than in options.

## The plan

### A1 — the projectile engine sound — **done 2026-09-19**

The headline. `WeaponProjectile::engineSound_` **defaults to
`data/wav/misc/rocket.wav`**, and of the 55 projectile weapons in the base
mod exactly two turn it off; Apocalypse turns it off everywhere. So in
upstream nearly every shell that flies hums while it flies, looped, positioned
on the shell, at gain 0.25 and `eMissile` priority — and this port is silent
for all of them. It is the most-heard sound in the game and the one nobody
noticed was missing, which is the sort of thing this review exists to catch.

What it needed was a looping, *moving* sound, where everything the port played
before was a one-shot fired at a position. The renderer publishes what is in
the air each frame and the UI reconciles that list against the loops it has
running: new shells start, live ones move, landed ones stop. A shell's key is
its tank, its sound and how many of that pair are already flying, which is
stable without the engine having to hand out shot ids - two shells of the same
weapon from the same tank can swap keys as they cross, which is inaudible.
Capped at four, because loops hold their channels for a whole flight where
one-shots do not.

### A2 — the arena wall hit — **done 2026-09-19**

`Wall::wallHit` plays `shield/hit2.wav` where a shot strikes the arena wall.
V8 drew that flash without ever playing it. The event already reaches the
renderer, so this is a line beside the flash.

### A3 — the sound of your turn — **done 2026-09-19**

`ClientStartGameHandler::startGame` plays `misc/play.wav`, and its own comment
- "a new game is commencing" - is wrong, which this review repeated before
checking. The function hangs off `TankStartMoveSimAction`: it runs when a move
of *yours* is granted, so it is the sound of your turn arriving, and a player
hears it every round rather than once a game. The port raises it from where it
already tracks that same moment, next to the countdown beeps.

### A4 — stereo — **done 2026-09-19**

Upstream hands OpenAL a listener orientation, so a shell landing on your left
is heard on your left. Every sound here plays centred: distance is honoured,
direction is not. `SoundPool.play` takes a left and a right volume, and the
renderer already publishes the camera basis, so the pan is the cosine between
the camera's right vector and the direction to the sound - computed in the
queue, beside the attenuation it belongs with, and applied to every sound and
every loop at once. A sound with no position is upstream's relative case and
stays centred, because something at the listener has no side.

### A5 — Doppler — **done 2026-09-19**

Worth doing once A1 gave it something that moves, and worth *reading* first:
upstream sets a velocity on each source but not on the listener, because
`SoundListener::setVelocity` is commented out with a note about Linux
crackling. So its doppler shift is the source's own motion and nothing else,
and matching that is both simpler and more faithful than computing a listener
term upstream throws away.

OpenAL's formula, with the listener term at zero:

    f' = f * (c - DF*vls) / (c - DF*vss),  vls = 0

where `vss` is the shell's speed along the direction from it to the listener,
`c` is 343.3 and `DF` is 1 - both OpenAL defaults, neither of which upstream
changes. `SoundPool` takes a playback rate, clamped to 0.5..2, which is also
what keeps a shell at the speed of sound from dividing by zero.

### A6 — the two missing mutes — **done 2026-09-19**

`NoCountDownSound` and `NoChannelTextSound` turn off exactly the beep and the
chat blip without silencing the game. Two switches beside the audio ones.

## Order

Built in this order on 2026-09-19: A4 first (it is what the others are heard
through), then A1, then A2, A3 and A6, and A5 last - once the shells were
humming there was something for it to shift.

## How these were verified

There are no host-tests for audio and there cannot usefully be: what they
would assert is that a file was named, which is what the log line already
says. The port logs one line per drained batch naming each file, and that line
now carries the gain **and the pan**, so "is that sound wired", "why was it
quiet" and "why did it come from the wrong ear" are all answerable from
`adb logcat` without listening. Each loop logs when it starts, for the same
reason.

On Pixel_5-2, from those lines: a volley starts three `rocket.wav` loops, one
per shell in the air, and the next volley starts three more - so they stop
when the shells land. `shoot/small.wav` and `explosions/small.wav` arrive at
pans of +0.52, +0.38, +0.29 and -0.01 while `play.wav` and `text.wav` stay at
+0.00, which is the relative case staying centred. `play.wav` plays once per
move granted. With "Message sound" off, a game start that previously logged
six `text.wav` logged none, while everything else still played.

A shell's hum also rises as it comes at the camera and falls as it goes away:
the loops start at rates up to 1.104 and end as low as 0.93, logged at both
ends for exactly this reason.

Two things are wired but unheard: A2's wall hit needs a shot that reaches the
arena wall, which no emulator round has thrown yet, and the countdown mute
needs a game with a shot clock - the quick-game presets have none. Both are
the same one-line gate as the message sound, which is proven above, but
neither has been heard.
