# The ScorchDroid dedicated server

A permanent Scorched3D server you can leave running on a spare machine, with a
browser page to configure and administer it. Two containers, one command.

It is the same engine the phone runs. This directory builds upstream
Scorched3D's own `src/common` + `src/server` against the port's portability
shims — the identical source list the Android app compiles — so a ScorchDroid
client, or a desktop **Scorched3D 44.3** client, connects to it exactly as it
would to any other Scorched3D server. Nothing about the game is different
because it is running in a container.

- [Quick start](#quick-start)
- [Configuration](#configuration)
- [Docker Desktop, and bridge networking](#docker-desktop-and-bridge-networking)
- [Administering it](#administering-it)
- [Mods](#mods)
- [Updating](#updating)
- [Backups](#backups)
- [Running it without Docker](#running-it-without-docker)
- [Troubleshooting](#troubleshooting)

For how the thing is built — the control protocol, where the settings page
comes from, the reasoning behind each decision — see
[docs/dedicated-server.md](../docs/dedicated-server.md).

## Quick start

You need Docker with Compose v2, and a few gigabytes of disk for the build —
most of that is reclaimable build cache; the two images come to under 1 GB.
There is nothing to clone: the compose file's build context is the GitHub
repository itself, and the builder fetches it with its submodules.

```bash
mkdir scorchdroid && cd scorchdroid
curl -O https://raw.githubusercontent.com/roge-rm/ScorchDroid/master/dedicated-server/docker-compose.yml
echo 'ADMIN_PASSWORD=pick-something' > .env
docker compose up -d --build
```

The first build compiles the engine from source and takes a few minutes. After
that it is cached and a restart is immediate.

Then open `http://<that machine>:8080` and sign in with the password you set.

Phones on the same network find the server by themselves under
**Multiplayer → Join Game**, with no address to type. Desktop Scorched3D
players, and anyone off the LAN, use the machine's address on port `27270`.

To stop it:

```bash
docker compose down          # keep the settings and saved games
docker compose down -v       # and throw them away
```

### If you already have the repository

The repository root carries the same two services with a local build context,
so a checkout needs no download:

```bash
git clone --recurse-submodules https://github.com/roge-rm/ScorchDroid.git
cd ScorchDroid
cp .env.example .env          # set ADMIN_PASSWORD
docker compose up --build
```

Submodules are not optional there — upstream's source and its `data/` tree are
not vendored into this repository, and the build needs both. The standalone
file above avoids the question by letting the builder do the fetching.

## Configuration

Everything is set in a `.env` file beside `docker-compose.yml`. Only the first
line is required.

| Variable | Default | What it does |
|---|---|---|
| `ADMIN_PASSWORD` | *(none)* | **Required.** The web admin refuses to start without one rather than come up open. |
| `SERVER_NAME` | `ScorchDroid` | What players see in their Join Game list. It names the config the first time the server starts; after that the Settings page owns the name, so a rename there survives a restart. |
| `GAME_PORT` | `27270` | Upstream's port. Anything else means players type `address:port`. |
| `WEB_PORT` | `8080` | The admin page. |
| `WEB_BIND` | `0.0.0.0` | `127.0.0.1` to keep the page to that machine and reach it over an SSH tunnel. |
| `LAN_DISCOVERY` | `1` | Publish the mDNS service phones discover. Needs host networking. |
| `HTTPS` | `0` | Set to 1 only behind a TLS-terminating proxy; it marks the session cookie `Secure`, which breaks plain HTTP. |

Everything else about the game — rounds, lives, teams, weapons, the map, the
bots, the server password, the message of the day — lives on the **Settings**
page rather than in this file. That page is generated from the engine, with
upstream's own descriptions, bounds and validation, so it holds every option
Scorched3D defines and not a hand-written subset.

Changing a variable here takes a `docker compose up -d` to apply.

## Docker Desktop, and bridge networking

The compose file uses **host networking**, because that is what lets a phone
discover the server: the web container publishes the mDNS service
`_scorchdroid._tcp.` that the app's join screen looks for, and multicast cannot
cross a Docker bridge. Host networking is a Linux feature.

On Docker Desktop, or if you would rather map ports explicitly, add the bridge
override:

```bash
curl -O https://raw.githubusercontent.com/roge-rm/ScorchDroid/master/docker-compose.bridge.yml
docker compose -f docker-compose.yml -f docker-compose.bridge.yml up -d --build
```

That costs LAN auto-discovery and nothing else. Players then use **Enter
address manually** in the join dialog, the same way they reach a desktop
Scorched3D host.

There is no public server list. Upstream's master-server announcement is not
compiled into this port, so a server on the internet is found by its address,
not by browsing.

## Administering it

The **Dashboard** shows the server's state, who is connected, the recent log
and the chat, refreshed in place. Each player row carries kick, ban, mute,
slap, take-their-money and kill; above the table are new round, kill all tanks
and add a bot. There is one admin account, guarding the whole page.

Two things worth knowing, both inherited from upstream rather than invented
here:

- A command against a player who has already left is refused, quietly. The
  page says so rather than claiming success.
- **Add a bot** is refused while bot balancing is on, because the balancer
  would remove the new bot within a tick or two. Raise the player count
  instead.

The web container owns no game state. Restart it, or leave it out entirely,
and the game does not notice.

## Mods

A mod is a directory of its own under `globalmods`. Mount it and pick it on
the Settings page — uncomment the line already in the compose file:

```yaml
    volumes:
      - ./mods/mymod:/opt/scorchdroid/data/globalmods/mymod:ro
```

A mod change always restarts the server, because upstream loads a mod partway
through its own startup, long before anything could be applied to a running
game. Everyone connected is dropped.

## Updating

```bash
docker compose build --pull
docker compose up -d
```

The build context is a git URL, so this picks up whatever the default branch
now holds. To stay on a fixed version instead, put a tag or commit on the end
of the context in `docker-compose.yml`:

```yaml
      context: https://github.com/roge-rm/ScorchDroid.git#v1.0.1
```

Your settings and saved games are in a volume and survive both.

## Backups

Everything worth keeping is in the `config` volume: `server.xml`, the ban list
and any saved games.

```bash
docker run --rm -v scorchdroid_config:/config -v "$PWD":/out debian:trixie-slim \
    tar czf /out/scorchdroid-config.tar.gz -C /config .
```

Deleting the volume is safe — the next start writes a fresh `server.xml`
holding every option upstream defines at its default, each with upstream's own
comment above it.

## Running it without Docker

The server is an ordinary CMake project with no unusual dependencies: a C++
compiler, CMake and zlib. No SDL, no OpenAL. It takes its settings as flags or
environment variables, and `--help` lists them.

```bash
git clone --recurse-submodules https://github.com/roge-rm/ScorchDroid.git
cd ScorchDroid
cmake -S dedicated-server -B build/server -DCMAKE_BUILD_TYPE=Release
cmake --build build/server -j
./build/server/dedicated_server --config ~/scorchdroid/server.xml \
    --data-root third_party/scorched3d --control-socket /run/user/1000/sd.sock
```

The web admin is a FastAPI app in `web-admin/`; point it at the same socket
through `SCORCHDROID_CONTROL_SOCKET`.

## Troubleshooting

**The web container exits immediately.** `ADMIN_PASSWORD` is unset. It refuses
to start rather than come up unguarded. Check the `.env` file is beside
`docker-compose.yml` and that `docker compose config` shows the value.

**Phones do not see the server.** Discovery needs host networking and
`LAN_DISCOVERY=1`, and it only crosses a single subnet — a phone on a guest
network or a different VLAN will never see it. Players can always type the
address.

**The dashboard says the server is down.** The web admin reaches the game over
a Unix socket in a shared volume. `docker compose logs server` will say why the
game stopped; the admin page is only reporting it.

**The build fails compiling upstream's `Vector.hpp`.** Something has pinned the
image to Debian bookworm. Upstream calls `std::sinf`, which libstdc++ only
gained in GCC 13, so both build stages must stay on trixie or newer.

**Port already in use.** With host networking the ports are taken on the host
directly. Set `GAME_PORT` or `WEB_PORT` in `.env`, or use the bridge override.

**Still stuck?** Ask on the project Discord:
**[discord.gg/9Wun47jGC6](https://discord.gg/9Wun47jGC6)**. `docker compose logs`
from both containers is the useful thing to bring.

## Security

The admin page can kick, ban and rewrite every setting. It is guarded by one
password, a signed session cookie and a CSRF token on every form, and it
refuses to start if `ADMIN_PASSWORD` is unset. That is proportionate to a game
server with one operator; if it faces the internet, put it behind a reverse
proxy with TLS and set `HTTPS=1`, or bind it to `127.0.0.1` and use an SSH
tunnel.

The game port is what any Scorched3D server has always exposed. Set
`ServerPassword` on the Settings page if you want to keep strangers out.
