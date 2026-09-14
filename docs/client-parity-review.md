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

### Audio — the aiming servo sounds — DONE 2026-09-10

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

### Admin — a whole subsystem, server side already built — DONE 2026-09-10

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

**Deferred again 2026-09-14, by dan's choice**, after the mini-map landed in
4f450a2 and removed the dependency: "leave the plan drawing for a potential
future date". The reasoning, for whoever picks it up:

- It is **team-only and multiplayer-only**. `ServerLinesHandler` relays a
  message solely to tanks sharing the sender's team (plus admins), so in a
  solo game against bots the feature does nothing at all. Its worth is tied
  to how much team play actually happens over Wi-Fi Direct or Bluetooth.
- It cannot be verified on one device. Two phones on the same team are the
  minimum to test anything past "my own stroke appears", so the natural time
  to build it is when there is a team game to test it in.
**Update 2026-09-14: the local half is built, and it is wire-compatible.**
What is left is the networking itself.

The interaction dan specified, and what is now in the app: the enlarged map
*is* draw mode. A tap places the first point of a line and shows it as a
hollow dot; a second tap completes a straight line in your tank colour. Tap
no longer looks the camera anywhere while enlarged - the small map keeps
that. Shrinking is the chevron's job, because two quick taps on the enlarged
map are a short line, not a double tap.

The gesture is ours, because a phone has no second mouse button. **Everything
the wire can see is upstream's**, at dan's instruction - "I want to preserve
cross compatability":

- a line is two points and a pen-up, exactly what `GLWPlanView` produces for
  the shortest possible drag, so a PC client renders what this sends;
- lines **expire three seconds after they are drawn** and fade as they go, on
  upstream's own `1 - age/3`;
- **there is no delete.** An earlier draft had a long press remove a line;
  it is gone, because `ComsLinesMessage` is append-only and cannot express
  one - a delete would either desync the two maps or need a message upstream
  does not have. A stroke that removes itself does not need removing.

The three seconds run from when the line was *completed*, not from its first
tap, and that is the faithful reading rather than a shortcut: upstream's
receivers stamp points with their own arrival time (`simulateLine` sets
`first[2] = totalTime_`), so a whole stroke appears at once at the far end
and fades together. It is what a teammate sees anyway, and it stops a line
that took a moment to place from being born half faded. A half-drawn line
abandons itself on the same clock, which is what replaced the long press.

**What the networking still needs**, none of it a design question any more:
a JNI send on line completion, a `ClientLinesHandler` equivalent to receive,
and the points converted between landscape coordinates and upstream's
widget-normalised 0-1 at the boundary. The relay needs no new thinking -
`ServerLinesHandler` already drops anything over 150 points, checks the
sender's destination, honours the mute gate and fans out to the sender's
team alone - and `common/coms` and `server/server` are both compiled into
this port already. It cannot be verified on one device, so it wants two
phones on the same team.

- ~~The open design question is the gesture, not the code.~~ **Answered**,
  by dan, and built in cc1eb37: a double tap enlarges the map and another
  shrinks it. Drawing becomes drag *inside* the enlarged state, so the map's
  drag is not spent globally and a thumbnail-sized scribble - which would be
  useless anyway - never arises. Upstream draws with the **right** mouse
  button (left is the look-at the port already has), which a phone does not
  have; a mode entered on purpose is the phone's answer to a second button.
  What is left to decide is only what a *tap* does once drawing is live:
  look-at, as it does now, or nothing, so a stray tap cannot jerk the camera
  mid-scribble. The latter, probably.

The mechanics, already read out of upstream so the next person need not:
points are normalised 0–1 widget coordinates with a timestamp in `z`,
decimated at 5px of movement, a null vector for pen-up; batched and sent
every 2s; the server drops anything over 150 points, checks the sender's
destination and honours the mute gate; the receiver stamps arrival time so
the **3-second** fade is local, alpha is `1 − age/3` in the sender's tank
colour, and the whole map dims to 0.2 and climbs back at 0.2/s when lines
first arrive.

### Auto-defense selection — DONE 2026-09-10

Corrected on implementing it: nothing is *automatic*, despite the name -
`TanketAutoDefense::newMatch()` and `changed()` are both empty upstream. "Auto
Defense" is a buyable accessory (armslevel 7, cost 3000) whose description is
the whole feature: "Allows the tank to activate shields and parachutes before
the round begins." Upstream spends it once, when the shop closes:
`AutoDefenseDialog::windowInit` runs `if (haveDefense()) displayCurrent(); else
finished();`.

Wired to "Done buying" for that reason, and deliberately *not* to a long press
on the Defences button - that button is reachable at any time, so hanging it
there would give every player the accessory's benefit for free.

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

## Remaining

Plan drawing (`ComsLinesMessage`) — no longer blocked by the minimap, which
landed in 4f450a2, but deferred on its own merits 2026-09-14 (see above) —
and tooltip settings, which is one job with the deferred tank tooltip.

## The structural finding

`OptionsDisplay` holds 150 entries and is never compiled, so **every limit and
default it carries is dropped unless something here re-checks it**. That is
what hid `SoundChannels`. The graphics entries are covered by the visual
parity review; the audio ones are now covered; the rest are behavioural
toggles worth a look if anything else turns up odd.
