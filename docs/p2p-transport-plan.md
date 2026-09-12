# Peer-to-peer transports

Multiplayer originally required everyone to already be on a shared IP network.
Two people sitting next to each other with no router, or on a guest network
with client isolation, could not play. This is the plan for the Android radios
that carry a game between devices with no infrastructure, and what each one
actually cost.

Google's Nearby Connections is deliberately excluded despite having the best
pairing experience of any option here: it needs `play-services-nearby`, a
proprietary dependency in a GPLv2 app that ships a corresponding-source offer,
and it would not work on a de-Googled device. Decided 2026-09-09.

## The dividing question

Everything below splits on one thing: **does the transport give us a real IPv4
network?**

- **Yes** — the engine changes zero lines. `NetServerTCP3.start(port)` and
  `connect(host, port)` work verbatim; all that is needed is Kotlin to bring
  the link up and produce a host address. Wi-Fi Direct and the hotspot paths.
- **No** — a new `NetInterface` subclass has to bridge C++ to Kotlin over JNI.
  Bluetooth.

Two things make the second case cheaper than it looks. `NetInterface` is
genuinely polymorphic — no downcasts anywhere, and `ScorchedContext::
setNetInterface()` is public, which `host-tests/main.cpp` already exploits to
swap the host's interface after `startServer()`. And a non-IP transport can
report `ipAddress == 0`: `NetLoopBack` already does, and the server's ban and
`AllowSameIP` checks are guarded on `!= 0` (`ServerMessageHandler.cpp:58,90`).

One thing makes a third case *more* expensive — see Phase 4.

## Phase 0 — what the game actually costs on the wire (done)

Before building a Bluetooth transport, establish whether the traffic fits in
one. Nothing new had to be instrumented: the protocol layer already counts
every byte into `NetInterface`'s static `bytesIn_`/`bytesOut_`. Sampled in
`testClientJoin`'s child process, whose counters only ever see the one
connection — the host's carry every other test's traffic too.

Measured 2026-09-09 across a 90s soak covering several rounds:

| | |
|---|---|
| Join handshake | 46,247 B out / 987 B in over 2.5s (~150 kbit/s burst) |
| Steady play | 12,056 B in / 2,744 B out over 90s = **1.07 kbit/s in, 0.24 kbit/s out** |
| Peak any one second | 3.1 kbit/s in, 1.9 kbit/s out |

**Play itself is essentially free** — a Bluetooth RFCOMM link, in the low
hundreds of kbit/s, has around two orders of magnitude of headroom. The entire
cost of a join is one burst, and that burst is `ComsHaveModFilesMessage`: the
client enumerating all 1130 files of the `none` global mod as name + length +
CRC, which is 46,247/1130 = 41 bytes each. Nothing else comes close. The
landscape is not in it at all — it travels as a definition both ends
regenerate, so the level message is under a kilobyte.

Gate passed. The only consequence for Phase 3 is that a join over Bluetooth
takes a second or two and wants a progress indicator.

The soak is opt-in via `SCORCHDROID_NET_SOAK_SECONDS` so the normal suite does
not get slower to carry a measurement.

## Phase 1 — Wi-Fi Direct (working on two devices, 2026-09-11)

`app/src/main/java/com/rm/scorchdroid/WifiDirectTransport.kt`, a sibling of
`LanDiscovery.kt`: a rendezvous mechanism with no JNI, because Wi-Fi P2P has
nothing to do with the engine. The devices form their own group, the group
owner sits at a fixed address, and the game then runs over the same sockets a
LAN game uses.

The host owns its group outright (`createGroup`) rather than negotiating one at
connect time. A negotiated group decides ownership by bidding, and a host that
loses has no address to publish.

Both discovery mechanisms advertise the same `_scorchdroid._tcp` service and
merge into one "Find Games" list, because to a player they are one feature and
differ only in which radio carried the announcement. They reach different
people, so hosting does both: NSD finds anyone already on this network,
including a PC; Wi-Fi Direct reaches someone with no network at all.

A Wi-Fi Direct row carries a `p2pDeviceAddress` and no host — there is no IP
until a group forms — so picking one negotiates the group first, under its own
status line, since that takes seconds and puts an invitation prompt on the
host's screen.

### First device test, 2026-09-11: neither device found the other

An Android 15 phone and an Android 11 phone. Both appeared to be hosting over
Wi-Fi Direct; neither turned up in the other's search. No location permission
prompt was seen on either device.

The test could not say why, and that is the first thing this uncovered: **every
way this can fail was silent**. A refused permission, a device without the
hardware, Wi-Fi switched off, the location master toggle off on API ≤ 32, and
simply nobody being there all produced the same empty list — `advertise` and
`startDiscovery` both returned early with no reason anyone could see. Worse,
`showFindGames` skipped the Wi-Fi Direct scan entirely on a device that was
hosting, changing only the dialog title, so a phone searching from its own
hosted game looked like a phone searching and finding nothing.

Fixed together, since the next two-device session should be diagnostic rather
than another blind run:

- `unavailableReason()` gives one sentence for each of the five states, and the
  hosting label and the "No games found" title both carry it. There is now no
  path on which Wi-Fi Direct quietly does not happen.
- **The service request is no longer typed.** It asked for `_scorchdroid._tcp`
  and had the supplicant match it; several stacks answer a typed request with
  nothing while answering an untyped one with the very same service. It now
  asks for everything and matches in the listener, which costs one string
  compare. This is the likeliest single cause of the failure.
- **`discoverPeers()` runs alongside `discoverServices()`.** It never did. A
  device that is not running peer discovery does not answer other devices'
  probes either, so this is also what makes a searching phone findable.
- **The query is re-issued every 5s** for a 20s window, up from one attempt
  over 8s. A service query is a single round of probes; a peer whose radio was
  elsewhere for that round was simply missed and nothing retried.
- Devices seen by peer discovery that answered no service query are listed as
  "nearby, no game seen (try anyway)" and can be joined on the default port.
  That separates "the other phone is not there" from "the other phone is there
  and DNS-SD over P2P is not working" — which is most of the diagnosis — and it
  is a usable fallback, since service discovery is the flakiest part of this
  path.
- Group formation logs its SSID, owner flag and **operating frequency**. A
  group owner on a 5GHz channel is findable in theory and often not in
  practice. If the logs show that, the lever is `setGroupOperatingBand(
  GROUP_OWNER_BAND_2GHZ)` — which, per `WifiP2pConfig.Builder.build()`, forces
  a fixed network name and passphrase too, so it is not a free change and is
  not made on spec.

### Second device test, 2026-09-11: they see each other, no group forms

The discovery fixes worked - both phones now list the other - and both then
failed with "couldn't form a Wi-Fi Direct group".

The cause is in the platform documentation for `connect()`: *"if the current
device is part of an existing p2p group or has created a p2p group with
createGroup, an invitation to join the group is sent to the peer device"*. A
device that owns a group does not join anyone else's. Both phones had hosted,
so both owned one, and each `connect()` merely invited the other; neither was
joining, and both timed out. Groups outlive the game that created them and the
process that asked for one, so a flag tracking whether this app is hosting is
not enough to know.

`connectToOwner` now stops scanning, stops advertising, and **waits for its own
group to actually be gone** - polling `requestGroupInfo` rather than trusting
`removeGroup`'s callback, because a `connect()` issued while the old group is
still tearing down behaves as though it were still a member. It also reports
why it failed instead of only that it did: the framework's refusal reason, a
group that formed with the wrong device as owner, and the timeout now saying
that the other phone may be showing an invitation prompt - which lands on the
device the player is not looking at.

### Third device test, 2026-09-11: connect refused with reason 0

A regression from the fix above, and the reason is worth keeping: everything
done to get into a fit state to connect - stopping the scan, stopping peer
discovery, leaving this device's own group - empties the framework's list of
known peers, and `connect()` to a device that is no longer in that list is
refused outright with `ERROR` (0) before anything goes on the air.

So the peer is now found *again*, immediately before connecting, rather than
the address being trusted because it was true when the player tapped it:
`awaitPeer` runs peer discovery and polls `requestPeers` for up to fifteen
seconds, re-issuing discovery every four. Leaving a group also gets a second
and a half to settle before a connect goes in, and an immediate refusal is
retried up to three times with a fresh peer lookup each time. A *timeout* is
not retried - it has already cost the player thirty seconds and the second
thirty would look identical.

### Fourth device test, 2026-09-11: connected

Two phones, no router, a game joined and played. Wi-Fi Direct works, and the
`ConnectivityManager.bindProcessToNetwork()` risk noted below did not
materialise on this pair.

It immediately found a bug that has nothing to do with Wi-Fi Direct and
everything to do with never having had a second real device to play on: the
Fire button's locked state did not stick on the joining device. The HUD asked
"is there a move id on my tank", and there is - the client's own copy is set by
`TankStartMoveSimAction` when the move is granted and cleared by
`TankStopMoveSimAction` only once the server has the move, a round trip later.
So the button unlocked on the next tick after firing. It now remembers the id
it committed against and unlocks on a *different* one. Skipping locks the
button too, which it never did: a skip is a committed move like any other, and
it is the one move that otherwise looks like nothing happened at all.

**Still open, and it cannot be done on an emulator:** the same on more than one
pair of handsets. Known risk to watch for: some devices route poorly with Wi-Fi
and P2P up together — if connecting to the group owner fails while normal Wi-Fi
is associated, bind the socket to the P2P network with
`ConnectivityManager.bindProcessToNetwork()`.

If a future pair sees each other and still cannot connect, the autonomous group
owner is the next suspect — the official DNS-SD sample never calls
`createGroup`, and advertise-then-negotiate is a small change, since the joining
side already asks for `groupOwnerIntent = 0` and reads the owner address out of
its own connection info.

## Phase 2 — hotspot play (done)

This always worked and was simply never mentioned anywhere. A "How do I
connect?" row in Find Games now covers all four routes, placed there because
"No games found" is the moment a player concludes multiplayer is broken.

The hosting address is also ranked now rather than being whichever interface
enumerated first — normal Wi-Fi, then the hotspot interface, then Wi-Fi
Direct's own last.

Not done, and only worth it if Phase 1 disappoints: `startLocalOnlyHotspot()`
to bring a hotspot up in-app and show its credentials as a `WIFI:` QR code,
with clients joining via `WifiNetworkSpecifier`. That API has grown fragile —
`getWifiConfiguration()` is deprecated and returns null on API 30+, needing
`getSoftApConfiguration()`, and the whole path needs location services actually
switched on.

## Phase 3 — Bluetooth RFCOMM via a transport bridge (working on two devices, 2026-09-11)

Parked 2026-09-09, unblocked by Phase 1 working on two devices on 2026-09-11,
and built the same day. The design below is what was built; what changed in
the building is noted against each piece.

It was deliberately gated behind Phase 1's device test, and that gate was
worth having: Wi-Fi Direct removes the "we need a router" problem for the
large majority of cases at a fraction of this cost.

Bluetooth is the genuine fallback — no Play Services, no Wi-Fi at all, works on
every Android device.

### Design

Split so the risky part is testable without Android:

- **`BridgeTransport.hpp`** — abstract C++ interface: `startListening()`,
  `connectTo(endpoint)`, `send(destId, bytes, len)`, `disconnect(destId)`,
  `stop()`, plus a sink for `onPeerConnected` / `onPayload` /
  `onPeerDisconnected`.
- **`NetBridge.{hpp,cpp}`** — `class NetBridge : public NetInterface` over a
  `BridgeTransport`, holding all the engine-facing semantics and no Android.
  Reuse `NetMessageHandler` rather than hand-rolling a queue — it is already
  the thread-safe hand-off `NetServerTCP3` uses between its socket thread and
  `processMessages()`. Build messages from `NetMessagePool` with
  `ipAddress = 0`; synthesise `ConnectMessage` / `BufferMessage` /
  `DisconnectMessage`; assign unique non-zero destination ids per peer.
- **`JniTransport.{hpp,cpp}`** — the Android `BridgeTransport`, calling into
  Kotlin and receiving callbacks from its reader threads.
- **`BluetoothTransport.kt`** — `listenUsingRfcommWithServiceRecord()` /
  `connectToServiceRecord()`, one fixed UUID, one reader thread per socket.
  Length-prefix framing lives here so C++ only sees whole messages.

### Seams

`connect(host, port)` is already virtual, so a transport that treats `host` as
a MAC address and ignores `port` needs no signature change.

- Host (`engine_jni.cpp:380-406`): keep `startServer(settings, false, …)`, then
  `setNetInterface(new NetBridge(...))` and **re-call**
  `setMessageHandler(&server->getComsMessageHandler())` before `start()` —
  `startServerInternal` set the handler on the interface it built, so replacing
  it requires re-setting it. `host-tests/main.cpp` already does exactly this.
  No patch to `third_party/` is needed.
- Client (`ClientContext.cpp:84`): declare the local as `NetInterface*` and
  pick the implementation. Nothing else in that function changes.
- Manifest: `BLUETOOTH_CONNECT`, `BLUETOOTH_SCAN` (`neverForLocation`) and
  `BLUETOOTH_ADVERTISE` for 31+; `BLUETOOTH`/`BLUETOOTH_ADMIN` with
  `maxSdkVersion="30"` and `ACCESS_FINE_LOCATION` below that.

### Constraint

`host-tests/main.cpp` documents that two `NetInterface` instances in one
process misroute messages through the shared `NetMessagePool` singleton. One
instance per process stays the rule, which `engine_jni.cpp`'s mutually
exclusive `kHost`/`kClient` modes already enforce.

### What the building changed

- **Peer ids are the transport's to allocate**, not NetBridge's. The
  transport is the side that discovers peers, and an id it hands back is
  used verbatim as a Scorched3D destination id. The contract - non-zero,
  unique, never reused - is in `BridgeTransport.hpp`, and NetBridge rejects
  0 and `UINT_MAX` because both mean something else to the engine.
- **A client's "server" is simply the first peer that connects**, since a
  client only ever has one. That removed the reserved-id machinery the
  design implied.
- **`onTransportFailed` was added to the sink.** A client that cannot reach
  the host at all has no peer to be disconnected from, and the join screen
  would otherwise wait for a connection that is not coming.
- **`ClientContext::connectToServer` takes an optional `NetInterface`**
  rather than always building a TCP one, which is the whole of the client
  seam. The host seam is what the design said it would be: `setNetInterface`
  plus re-setting the message handler.
- **Hosting over Bluetooth is its own menu entry**, not an option inside
  hosting. One NetInterface per process means a Bluetooth game is not also a
  Wi-Fi game, so the choice cannot be made after the fact, and `getServerPort`
  reports 0 because there is genuinely no port to tell anyone.
- **Joining over Bluetooth is its own search**, not a third row of "Find
  Games". Because the scan cannot be narrowed to devices running the game
  (below), it lists every speaker, headset and car in range, and burying the
  two phones it exists for in that was worse than one more button. The
  network search keeps NSD and Wi-Fi Direct, where every row is a real game.
- Discovery lists **paired devices immediately and scanned ones as they
  arrive**, and does not ask each device whether it serves our UUID first.
  An SDP lookup per device is slow and frequently answers nothing for a
  device that is in fact hosting - and Phase 1's first failed test is the
  standing reminder that an empty list nobody can explain is the worst
  outcome available. A device that is not a host fails the connect in a
  second or two and says so.

### Verification

Done, and it is the part of this worth keeping: `host-tests` implements a
second `BridgeTransport` over a Unix domain socket pair and runs `NetBridge`
through the existing `testClientJoin` fork+exec harness. A real
`ClientContext`, in a real separate process, completes the real handshake -
auth, the 1130-file mod manifest, the level definition - sees every tank the
host has, and converges its clock on the host's, with no TCP socket anywhere.
There is also a smaller in-process test for the things a hand-written
transport gets wrong: a 100KB message arriving whole and byte-identical
rather than in read-sized pieces, the destination id surviving the round
trip, and a peer that leaves being reported exactly once.

### Device test, 2026-09-11: a game over Bluetooth

Two phones, no Wi-Fi, a game hosted and joined - **if the two were paired by
hand first**. An unpaired host never appeared in the other phone's list, which
is two separate faults, both now fixed:

- **The location toggle was never checked.** Below API 31 a classic Bluetooth
  scan needs the master location switch on as well as the permission; with it
  off, discovery starts, succeeds and reports nothing. Paired devices still
  list, because they need no scan - so the failure looks exactly like "only
  pairing works". Wi-Fi Direct got this check after its own first failed
  test; Bluetooth was written later and did not inherit it, which is the
  argument for both transports answering the same `unavailableReason`
  question in the same words.
- **The visibility window was being spent on the setup screen.** Android
  grants discoverability for five minutes at most (the documented default is
  two) and the clock starts when the prompt is answered. It was asked on the
  way *into* game setup, so choosing options and waiting for a landscape came
  out of the other player's time. It is now asked at the moment the game
  starts, and the hosting label says the window exists.

### Unpaired discovery, 2026-09-11: the broadcasts were being dropped

Unpaired never worked, in either direction, and the cause was in this code.
`startDiscovery()` returned true, the adapter reported `isDiscovering` for a
full twelve-second inquiry, the host reported itself discoverable - and not
one `ACTION_FOUND` arrived. The receiver was registered `RECEIVER_NOT_EXPORTED`.

Since Android 12 the Bluetooth stack is an APEX module with **its own UID**,
so its broadcasts come from a different app; Wi-Fi P2P's come from the system
server, which is why the identical flag works there and drops everything here.
Exporting this one costs nothing: all three actions are *protected* broadcasts
that the platform will not let any app but the system send.

Getting there took three rounds of instrumentation, and the shape of it is
worth keeping. Each round answered one either/or:

1. Does the host advertise and the joiner scan at all? (Both, yes - the host's
   label now asks the adapter whether it is *discoverable* rather than
   claiming it, since connectable and discoverable are different states.)
2. Did an inquiry begin? (No broadcast said so.)
3. Was the radio scanning anyway? (`adapter.isDiscovering` - yes, which moved
   the fault from the framework to this file in one step.)

A silent failure needs to be made to name itself before it can be fixed, and
none of these three questions could be answered from the outside.

Confirmed working unpaired, both directions, 2026-09-11.

**Still to do:** a handset pair that is not these two - accept/connect/pairing
is exactly the kind of path that works differently on every device.

## Cross-play with desktop Scorched3D, which was never a goal

Demonstrated 2026-09-12: a desktop client and this port playing together in a
PC-hosted game. Nothing here was done to achieve it. It rests on two things,
both of which live in the submodule and can be ended by a bump with no other
symptom, so `host_tests` pins both (`testDesktopCompatibility`):

- **The handshake's version strings**, `44.3` and protocol `"ew"`, because the
  port compiles upstream's own `DefinesScorched.hpp`. A desktop server compares
  the protocol string exactly and rejects anything else.
- **The 1129-file manifest of the global mod**, name, length and CRC each,
  because the shipped data is upstream's own and no patch touches `data/`.
  Drift there does not fail a join outright - the server sends whatever differs
  - so it would show as a handshake quietly turning into a file transfer.

The test cannot prove a desktop client will connect; only a desktop client can
do that. It exists to fail the moment either foundation moves, rather than
leaving that to be found by a player.

## Phase 4 — Wi-Fi Aware, investigated and shelved

Technically the most elegant option: publish/subscribe with no group formation,
and `WifiAwareNetworkSpecifier` yields a real socket network. It looks like it
should be as cheap as Wi-Fi Direct. It is not.

1. It hands back an **IPv6 link-local address with a scope id**, and the socket
   layer is IPv4-only — `app/src/main/cpp/porting/SDL_net_compat.h:23-26`
   defines `IPaddress` as `Uint32 host`. Widening it is not purely local
   either: `NetBufferUtil.cpp:39-67` reaches directly inside
   `struct _TCPsocket`, which `SDL_net_compat.h:33-39` documents as having to
   stay binary-identical.
2. `NetMessage::getIpAddress()` and `NetInterface::getIpName()` are 32-bit
   throughout the server.
3. Hardware support is genuinely sparse.

So Wi-Fi Aware costs *more* than Bluetooth despite looking cheaper. Revisit
only if Phase 1 proves unreliable across real devices.

## A solo game publishes nothing (2026-09-12)

Every game on this port is a hosted game - one engine, always running the
server - and for a while every game was also *advertised* like one: NSD
registration, a Wi-Fi Direct group, the player's own IP on the HUD, and the
two toasts reporting how those went.

Found the plain way, on the emulator, while checking the tutorial's new
wording. The tutorial's first card came up over the toast **"No Wi-Fi Direct:
the nearby devices permission was refused"** - a reasonable thing to tell
someone hosting, and nonsense to tell someone being taught to aim. The
Wi-Fi Direct group and the NSD service were both real, on a single-player
game, which is worth more than the wording: it holds the radio in a group and
announces a solo game to the room.

`MainActivity.hostForOthers` now decides, set on the way in beside
`hostOverBluetooth` and true only from Multiplayer's two host entries.
`updateHostingLabel()` returns immediately when it is false, ahead of
everything that publishes or reports on publishing. The listening socket
stays open - closing it would mean a second startup path through the engine
for no gain - so a solo game can still be joined by someone told the address
by hand. It just isn't announced.

Verified as an A/B inside one process on the API 34 emulator, filtering
logcat to `LanDiscovery` and `WifiDirectTransport`: the solo tutorial logs
nothing from either tag, and a Host Game started from the same process
logs `addLocalService succeeded`, `createGroup succeeded` and
`Registered LAN service`. The HUD's hosting line follows the same gate -
absent in solo, `Host 10.0.2.17` when hosting.
