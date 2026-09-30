"""The game in a browser, served by the web container.

Two things, neither behind the admin login, since players aren't admins:

- /play/     the page itself (web/ built into the image at PLAY_DIR).
- /play/ws   a WebSocket that joins the page to the game server.

A page can't open a TCP connection, so the socket is relayed here. Each
WebSocket message is one of the game's own messages, and upstream's TCP
framing (NetServerTCP3Send/Recv) is a 4-byte big-endian length and then the
buffer, so the relay adds the length on the way in and takes it off on the
way out. The server gets byte-for-byte what a phone would have sent it and
can't tell a browser from a phone, apart from every browser arriving from
this container's address.
"""

import asyncio
import logging
import os
import struct

from fastapi import APIRouter, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, RedirectResponse

from .control import Control

LOG = logging.getLogger("scorchdroid.play")

router = APIRouter()
control = Control()

PLAY_DIR = os.environ.get("PLAY_DIR", "/app/play")
GAME_HOST = os.environ.get("GAME_HOST", "127.0.0.1")
# The biggest message upstream's receiver accepts (NetServerTCP3Recv).
MAX_MESSAGE = 15_000_000
MAX_PLAYERS = int(os.environ.get("PLAY_MAX_CONNECTIONS", "32"))

_open = 0


def _game_port():
    return int(os.environ.get("GAME_PORT", "27270"))


@router.websocket("/play/ws")
async def relay(ws: WebSocket):
    global _open
    if _open >= MAX_PLAYERS:
        await ws.close(code=1013)
        return
    await ws.accept()
    try:
        reader, writer = await asyncio.open_connection(GAME_HOST, _game_port())
    except OSError as error:
        LOG.warning("relay: can't reach the game on %s:%d: %s", GAME_HOST, _game_port(), error)
        await ws.close(code=1011)
        return

    _open += 1
    peer = ws.client.host if ws.client else "?"
    LOG.info("relay: %s joined (%d open)", peer, _open)

    async def up():
        while True:
            data = await ws.receive_bytes()
            if not data or len(data) > MAX_MESSAGE:
                return
            writer.write(struct.pack(">I", len(data)) + data)
            await writer.drain()

    async def down():
        while True:
            length = struct.unpack(">I", await reader.readexactly(4))[0]
            if length == 0 or length > MAX_MESSAGE:
                return
            await ws.send_bytes(await reader.readexactly(length))

    tasks = [asyncio.create_task(up()), asyncio.create_task(down())]
    try:
        # Whichever side ends first ends both.
        done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in pending:
            task.cancel()
        for task in done:
            error = task.exception()
            if error and not isinstance(error, (WebSocketDisconnect, asyncio.IncompleteReadError, ConnectionError)):
                LOG.warning("relay: %s: %s: %s", peer, type(error).__name__, error)
    finally:
        _open -= 1
        LOG.info("relay: %s left (%d open)", peer, _open)
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass
        try:
            await ws.close()
        except Exception:
            pass


@router.get("/play")
def play_root():
    return RedirectResponse("/play/", status_code=301)


@router.get("/play/server.json")
def play_server():
    """Tells the page it came from a server, and where the relay is, so
    Join Game can offer "This server" first."""
    info = {"relay": "/play/ws"}
    # The mod, so a browser can fetch its files before joining. Pages served
    # from elsewhere (the hosted one) ask too, hence the open CORS header.
    try:
        mod = control.status().get("mod")
        if mod:
            info["mod"] = mod
    except Exception:
        pass
    return JSONResponse(info, headers={"Cache-Control": "no-cache", "Access-Control-Allow-Origin": "*"})


@router.get("/play/{path:path}")
def play_file(request: Request, path: str):
    """The page's files, gzipped ahead of time where the build did (the
    game data is 87MB as it is and 39MB gzipped)."""
    if not os.path.isdir(PLAY_DIR):
        return HTMLResponse("<p>This server was built without the web game.</p>", status_code=404)
    path = path or "index.html"
    # Normalised rather than resolved: this refuses a path that climbs out
    # with "..", and a link the image's own build put here is trusted.
    root = os.path.abspath(PLAY_DIR)
    full = os.path.normpath(os.path.join(root, path))
    if not full.startswith(root + os.sep) or not os.path.isfile(full):
        return HTMLResponse("<p>Not found.</p>", status_code=404)

    headers = {"Cache-Control": "no-cache"}
    if "gzip" in request.headers.get("accept-encoding", "") and os.path.isfile(full + ".gz"):
        media = _media_type(full)
        headers["Content-Encoding"] = "gzip"
        headers["Vary"] = "Accept-Encoding"
        return FileResponse(full + ".gz", media_type=media, headers=headers)
    return FileResponse(full, media_type=_media_type(full), headers=headers)


def _media_type(path):
    for ext, media in (
        (".html", "text/html; charset=utf-8"),
        (".js", "text/javascript"),
        (".mjs", "text/javascript"),
        (".wasm", "application/wasm"),
        (".json", "application/json"),
        (".css", "text/css"),
        (".png", "image/png"),
        (".svg", "image/svg+xml"),
        (".webmanifest", "application/manifest+json"),
        (".data", "application/octet-stream"),
    ):
        if path.endswith(ext):
            return media
    return "application/octet-stream"
