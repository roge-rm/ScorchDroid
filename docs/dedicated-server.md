# Running a ScorchDroid dedicated server

A permanent server anyone can put on a spare machine with `docker compose up`,
plus a web page to configure and run it.

It is the same engine the phone runs. `dedicated-server/` builds upstream
Scorched3D's own `src/common` + `src/server` against this port's portability
shims - the identical source list `app/src/main/cpp/CMakeLists.txt` compiles -
so a ScorchDroid client, or a desktop Scorched3D 44.3 client, connects to it
exactly as it would to any other Scorched3D server. Nothing about the game is
different because it is running in a container.

## Quick start

```bash
git clone --recurse-submodules https://github.com/roge-rm/ScorchDroid.git
cd ScorchDroid
cp .env.example .env          # set ADMIN_PASSWORD
docker compose up --build     # first build takes a few minutes
```

Then open `http://<that machine>:8080` and sign in with the password you set.
Phones on the same network will find the server by itself under
**Multiplayer → Join Game**, with no address to type.

Submodules are not optional - upstream's source and its `data/` tree are not
vendored into this repository, and the build needs both.

## The two containers

| | |
|---|---|
| **server** | The game. Listens on 27270. Owns `/config` (settings, ban list, saved games) and publishes a control socket on `/run/scorchdroid`. |
| **web** | The admin page. Owns nothing. Every page is a rendering of what the server said over that socket, and every button is one control command. Restart it, or leave it out, and the game does not notice. |

They share two named volumes: `config` (the server's own, though the web admin
writes settings through the server rather than touching the file) and
`control` (the socket, and nothing else).

## Networking

The default compose file uses **host networking**, because that is what lets a
phone discover the server. `LanDiscovery.kt` finds games by looking for the
mDNS service `_scorchdroid._tcp.`, and the web container publishes exactly
that service on the game's port while the server is up. Multicast cannot cross
a Docker bridge, so on a bridge network no phone would ever see it.

The cost is that `GAME_PORT` and `WEB_PORT` are taken on the host directly
rather than published, and host networking is a Linux feature. On Docker
Desktop, or if you want explicit port mapping:

```bash
docker compose -f docker-compose.yml -f docker-compose.bridge.yml up --build
```

That turns discovery off and maps the ports instead. Players then use **Enter
address manually** in the join dialog, the same way they reach a desktop
Scorched3D host.

There is no public server list. Upstream's master-server announcement
(`ServerBrowserInfo`) is not compiled into this port and this does not change
that, so a server on the internet is found by its address, not by browsing.

## Settings

The **Settings** page is generated from the engine, not written by hand. The
server reports every option `OptionsGame` defines with upstream's own
description, type, range and list of values, and the page draws whatever
control that type asks for - a number with its real bounds, a switch, a list
of the values upstream accepts. Validation is upstream's too: a value it
refuses is refused here, with the reason.

The first tabs - Game, Players, Arms, World - are the same grouping the phone's
own setup screen uses, including which options it considers advanced.
Everything upstream defines that the phone never exposed lands under
**Server**: ban lists, logging, publishing addresses, sync checking, the
message of the day. Options upstream has retired are shown, marked, and
collapsed, rather than hidden - a config file written years ago still parses,
and it is better to see why a setting does nothing than to wonder where it
went.

Two save buttons, because the engine makes the distinction:

- **Save — applies next round** writes the config and pushes the values into
  the running server. Nobody is disconnected. The values take effect when the
  next round starts.
- **Save and restart now** writes the config and restarts the server. Everyone
  connected is dropped.

**Mods** always take the second path. Upstream loads a mod partway through its
own startup, long before anything could be applied to a running server, so a
mod change is a restart by definition. Switching mods also substitutes any bot
the new mod does not define - the Apocalypse mod has no "Moron", and a slot
naming one would wait forever for a bot that can never be created.

To add a mod, mount it into `globalmods` and pick it on the Settings page:

```yaml
    volumes:
      - ./mods/mymod:/opt/scorchdroid/data/globalmods/mymod:ro
```

## Backups

Everything worth keeping is in the `config` volume: `server.xml`, the ban list
and any saved games.

```bash
docker run --rm -v scorchdroid_config:/config -v "$PWD":/out debian:bookworm-slim \
    tar czf /out/scorchdroid-config.tar.gz -C /config .
```

Deleting the volume is safe - the next start writes a fresh `server.xml`
holding every option upstream defines at its default, each with upstream's own
comment above it.

## Environment

Set these in `.env`.

| Variable | Default | What it does |
|---|---|---|
| `ADMIN_PASSWORD` | *(none)* | Required. The web admin refuses to start without one rather than come up open. |
| `SERVER_NAME` | `ScorchDroid` | What players see in their Join Game list. |
| `GAME_PORT` | `27270` | Upstream's port. Anything else means players type `address:port`. |
| `WEB_PORT` | `8080` | The admin page. |
| `WEB_BIND` | `0.0.0.0` | `127.0.0.1` to keep the page to that machine and reach it over an SSH tunnel. |
| `LAN_DISCOVERY` | `1` | Publish the mDNS service. Needs host networking. |
| `HTTPS` | `0` | Set to 1 only behind a TLS-terminating proxy; it marks the session cookie `Secure`. |

The server binary takes the same settings as flags or environment variables -
`dedicated-server --help` lists them - so it runs perfectly well outside a
container:

```bash
cmake -S dedicated-server -B build/server -DCMAKE_BUILD_TYPE=Release
cmake --build build/server -j
./build/server/dedicated_server --config ~/scorchdroid/server.xml \
    --data-root third_party/scorched3d --control-socket /run/user/1000/sd.sock
```

## The control channel

The web admin talks to the server over a Unix domain socket - never a TCP
port, never reachable from the network, and completely invisible to a
connected game client. It carries no Scorched3D message and adds nothing to
`common/coms`, so the game's wire protocol, and with it compatibility with
desktop Scorched3D, is exactly what it was.

A request is one line of tab-separated fields. A reply is one line of JSON.
That asymmetry is deliberate: the server never has to parse JSON, so there is
no JSON library in the build.

| Command | What it does |
|---|---|
| `ping` | Game and protocol version. |
| `status` | State, round, map, mod, ports, and every tank with its score, money, life and ping. |
| `log <afterSeq> [limit]` | Log lines after a sequence number. The server keeps the last 2000. |
| `chat <afterId>` | Recent channel text, from upstream's own `getLastMessages()`. |
| `say <channel> <text>` | Speak into the game as the server. |
| `admin <verb> [player] [reason]` | `kick`, `ban`, `flag`, `mute`, `unmute`, `permmute`, `unpermmute`, `slap`, `poor`, `kill`, `changename`, `addbot`, `newgame`, `killall`, `stopwhenempty`, `setlogging`. |
| `options` | Every option with its type, range, values, default and description. |
| `options.set <name> <value>` | Staged, validated by upstream. |
| `options.save` / `options.reset` | Write the config file / reload it from disk. |
| `apply` | Push the staged options into the running server; takes effect next round. |
| `mods` / `setmod`, `landscapes` / `setlandscapes`, `bots` / `setbots`, `presets` / `loadpreset` | The same choices the phone's setup screen offers. |
| `restart` / `shutdown` | Restart in place, or exit and let the restart policy decide. |

Every one of these is a thin wrapper over something upstream already does.
`admin` is `ServerAdminCommon`, which has always worked and only ever lacked a
way to reach it; the options commands are `ScorchDroidSetup`, the same layer
the phone's setup screen is built on.

`host-tests`' `testControlChannel()` drives the whole surface, including a real
socket round trip, which is where it is pinned - see the note in
`host-tests/main.cpp`.

## Administration

The **Dashboard** shows the server's state, who is connected, the recent log
and the chat, all refreshed in place. Each player row carries kick, ban, mute,
slap, take-their-money and kill; above the table are new round, kill all tanks
and add a bot.

Two things worth knowing, both inherited rather than invented:

- A command against a player who has already gone is **refused**, quietly, by
  `ServerAdminCommon`. The page says so rather than claiming success.
- **Add a bot** is refused while bot balancing is on. With
  `RemoveBotsAtPlayers` set, `ServerStateEnoughPlayers` counts humans and bots
  together and would remove the new bot within a tick or two. Raise the player
  count instead.

There is one admin account and one password, guarding the whole page. Inside
the server the web admin acts as upstream's local account - the credential
upstream gives the machine running the server, holding every permission, which
is also what the phone's own admin menu uses.

## Security

The admin page can kick, ban and rewrite every setting. It is guarded by one
password, a signed session cookie and a CSRF token on every form, and it
refuses to start if `ADMIN_PASSWORD` is unset. That is proportionate to a game
server with one operator and no more; if it faces the internet, put it behind
a reverse proxy with TLS and set `HTTPS=1`, or bind it to `127.0.0.1` and use
an SSH tunnel.

The game port itself is what any Scorched3D server has always exposed. Set
`ServerPassword` on the Settings page if you want to keep strangers out.
