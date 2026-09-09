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

## Phase 1 — Wi-Fi Direct (built, not yet device-tested)

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

**Still to verify, and it cannot be done on an emulator:** two physical devices
with Wi-Fi *disconnected from any router*, so that it is the no-infrastructure
path being tested and not the existing LAN one. Confirm the group-owner address
in logcat and play a full round. Known risk to watch for: some devices route
poorly with Wi-Fi and P2P up together — if connecting to the group owner fails
while normal Wi-Fi is associated, bind the socket to the P2P network with
`ConnectivityManager.bindProcessToNetwork()`.

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

## Phase 3 — Bluetooth RFCOMM via a transport bridge (not started)

Deliberately gated behind Phase 1's device test: Wi-Fi Direct removes the
"we need a router" problem for the large majority of cases at a fraction of
this cost, so it should be proven on hardware before any C++ is written.
Phase 0's numbers say the transport itself is viable whenever we want it.

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

### Verification

Implement a second `BridgeTransport` in `host-tests/main.cpp` over a Unix
domain socket pair and run `NetBridge` through the existing `testClientJoin`
fork+exec harness. That exercises the whole real handshake against the new
transport with no Android involved, and keeps the clock-drift residual
assertion as a check on the bridge's timing. Then two physical devices in
airplane mode with only Bluetooth on.

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
