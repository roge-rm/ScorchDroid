# src/client parity review

`src/client` is never compiled — the architecture replaced it with Compose and
a from-scratch GLES3 renderer, and that stands. But "replaced" was never
audited feature by feature, and the audio spam fixed on 2026-09-09 came
directly from that: a mechanism upstream had, dropped as unnecessary, with
nothing checking.

`docs/` had parity reviews for the ground, the water, the wind and the scene
visuals. This is the pass for everything else in `src/client`.

Reviewed 2026-09-10 against the pinned upstream commit.

## Method

`src/client` is 17 directories. Four of them — `land/`, `landscape/`, `sky/`,
`water/`, plus `sprites/` and `geomipmap/` — are the scene, already covered by
the visual, ground, water and wind reviews. `GLEXT/`, `GLSL/` and `GLW/` are
the desktop widget toolkit and its GL helpers, replaced wholesale by Compose,
with no features of their own. That leaves `client/`, `graph/`, `dialogs/`,
`tankgraph/`, `console/` and `serverbrowser/`, which is where the features
live. `dialogs/` was read as a feature inventory rather than as UI.

## Already present, confirmed

Worth recording so the next sweep does not re-check them: the shop and weapon
select, camera presets, resign, skip, chat and its log, mod and map selection,
player identity (name, model, colour, avatar), profiles, scores, the tutorial
(this port's own, deliberately — upstream's `tutorial.xml` is keyed to the GLW
widget tree), connect and join, quit, settings, the FPS readout, and
simulation speed control (`getSimulator().setFast`, already wired at
`engine_jni.cpp:2332`).

## Gaps found

### Audio — the aiming servo sounds

`TankKeyboardControlUtil` holds four sound sources this port has no analogue
for: `startSound_` (`movement.wav`, one-shot) plus looping `rotateSound_`
(`turn.wav`), `elevateSound_` and `powerSound_`, all at `eRotation` priority
and positioned at the tank. Upstream plays them while a key is held to swing
the turret or wind up the power.

The port's aiming is sliders and a drag gesture rather than held keys, so the
analogue is a loop that runs while a control is being dragged. Needs loop
support in the sound path, which is one-shot today.

This is the one gap the audio parity pass missed — it was found here, not
there, because the sounds live in `tankgraph/` rather than `sound/`.

### Admin — a whole subsystem, server side already built

`ComsAdminMessage` carries login, kick, slap, poor, ban, flag, mute, permanent
mute, kill-all, admin talk, broadcast message, show-banned and sync check.
`common/coms` and `server/server` are both compiled here, so
`ServerAdminHandler`, `ServerAdminCommon` and `ServerAdminSessions` are all
already in the binary and working. **Only the client-side UI is missing** —
this is a UI job over a subsystem that already runs, not a port.

Matters for anyone hosting a game other people can join.

### Plan drawing

`ComsLinesMessage` carries a list of points per player: upstream lets players
draw on the plan view to point things out to each other, gated on
`OptionsDisplay`'s `NoPlanDraw` and drawn by `GLWPlanView`. Nothing in this
port handles the message.

Depends on the minimap, which is already on the deferred list — the plan view
*is* the minimap, so the two are one piece of work.

### Auto-defense selection

`TanketAutoDefense` is engine-side and compiled, and auto-defense accessories
already appear in this port's shop (`engine_jni.cpp:602`). Upstream adds a
dialog for choosing *which* shield, parachute or battery is used
automatically. The buying works; the choosing does not.

### Tooltips

`TipDialog` is not tips-of-the-day — it is the settings for upstream's two
tooltip kinds (help and info). Related to the already-deferred tank tooltip
work; one feature, not two.

## Confirmed non-gaps

- **Boid / flock sound.** Upstream's option is `depricatedNoBoidSound_` and
  nothing reads it. There is no such sound to wire.
- **Kibitzing.** A canned "No kibitzing, please." chat shortcut. Nothing more.
- **`serverbrowser/`.** The public master-server list, already an explicit
  deferred decision, not an oversight.
- **`console/`.** The desktop developer console, same category as the excluded
  web admin server.

## The structural finding

`OptionsDisplay` holds 150 entries and is never compiled, so **every limit and
default it carries is dropped unless something here re-checks it**. That is
what hid `SoundChannels`. The graphics entries are covered by the visual
parity review; the audio ones are now covered; the rest are behavioural
toggles worth a look if anything else turns up odd.
