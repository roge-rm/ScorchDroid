# Web port plan

ScorchDroid in a browser, playing online with phones through the dedicated server.
Started 2026-09-29. The shape follows Acidulous (Emscripten engine, Compose for
Kotlin/Wasm UI), with one big difference: no threads.

## Why one thread

A threaded wasm build needs SharedArrayBuffer, which needs the page to be
cross-origin isolated, which only works over https or on localhost. The
dedicated server's web container serves the game over plain http on a LAN, so a
threaded build would simply not start there. The engine never needed threads for
itself anyway: the phone's Kotlin drives it with a tick call ten times a second
and a draw call each frame, and the page can do the same from one
requestAnimationFrame loop.

What made threads of its own, and what it does on the web instead
(all under `__EMSCRIPTEN__`):

- `NetBridge`'s send thread: the outgoing queue is drained in `processMessages()`,
  on the engine's tick. A WebSocket send never blocks.
- The ground texture worker: the build runs inline, and the next frame adopts it
  the same way it adopts a finished worker's.
- The ocean worker: `pumpOceanInline()` generates a tile on the frame, at the rate
  the worker sleeps to.
- `NetServerTCP3` when hosting: a page can't listen, so the host side is a
  `NetBridge` over a transport that never listens, the way Bluetooth hosting
  replaces the TCP interface.

## How the engine is built

`web/engine` compiles `jni/engine_jni.cpp` and `jni/renderer_jni.cpp` unchanged
against a fake `jni.h` (from Acidulous, minus its threads), with shims for
`<android/log.h>` and `<sys/system_properties.h>`. The page calls the same
`Java_*` functions `NativeBridge` and `GameRenderer` do on a phone. The game data
is preloaded into Emscripten's file system at `/scorched_root`, where the phone
extracts it.

`./web/engine/build.sh` builds it (needs emsdk in `~/.local/share/emsdk` or
`$EMSDK`). `web/engine/test/index.html` is a bare test page: solo against bots
with `?solo`, or `?join=ws://host:8080/play/ws`.

Sizes: wasm 2.0MB (0.7MB gzipped), data 87MB (39MB gzipped).

## Online

The browser's `JniTransport` is a WebSocket (`web/engine/src/WebSocketTransport.cpp`).
`startJoinGameBluetooth(url)` joins through `NetBridge` exactly as a phone does
over Bluetooth, so the join handshake, mod manifest, level and simulation are the
same code.

The web container relays it (`web-admin/app/play.py`, `/play/ws`). Upstream's TCP
framing is a 4-byte big-endian length and the buffer, so each WebSocket message is
one frame: the relay adds the length on the way in and strips it on the way out.
Nothing on the wire changes, and the server can't tell a browser from a phone.

Two things to know as an operator:

- Every browser player reaches the server from the relay's address (127.0.0.1 with
  host networking). `AllowSameIP` defaults to on, which is what lets two browsers
  play at once; turning it off, or banning by IP, hits all browser players together.
- A page served over https can only open `wss://`, so a server behind TLS needs the
  proxy in front to pass WebSockets through too.

The web container also serves the game at `/play/`, gzipped ahead of time where
the build did it. Neither route is behind the admin login.

## Stages

- **W1, engine in a page.** Done 2026-09-29: solo against bots, and joining the
  dedicated server through the relay, both verified in headless Chromium.
- **W2, the real UI.** Move the Compose screens into a shared Compose Multiplatform
  module (android + wasmJs), as Acidulous does. GameHud, HudDialogs,
  GameSetupScreen, MainMenu and Tutorial import nothing from Android; MainActivity's
  tick loop and glue get split into a common controller and the Android shell.
  `NativeBridge` becomes expect/actual. Material icons need replacing (the extended
  set isn't published for wasm). Name plates are drawn from text bitmaps the UI
  makes, which the web side makes with a 2D canvas.
- **W3, the web side of the rest.** Sound through WebAudio (the engine already
  hands over "path|gain|priority|pan"), music, saves and settings in IndexedDB,
  the build in the web container's image, and the page split so the Apocalypse mod
  (22MB) only loads for players who pick it.
