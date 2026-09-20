"""ScorchDroid dedicated server - web admin.

One of the two containers in docker-compose.yml. It owns no game state at
all: every page is a rendering of what the server said over the control
socket, and every button is one control command. If this container is not
running, the game is entirely unaffected.
"""

import asyncio
import logging
import os
from contextlib import asynccontextmanager

from fastapi import FastAPI, Form, Request
from fastapi.responses import HTMLResponse, PlainTextResponse, RedirectResponse, Response
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates

from . import auth
from .control import Control, ControlError, ServerDown
from .discovery import Advertiser

logging.basicConfig(level=os.environ.get("LOG_LEVEL", "INFO"))
LOG = logging.getLogger("scorchdroid.web")

HERE = os.path.dirname(os.path.abspath(__file__))
control = Control()

# Groups in the order the phone's setup screen shows them, so the two
# surfaces read the same way round. Anything upstream defines that the
# curated list never placed lands under "Server".
GROUP_ORDER = ["Game", "Players", "Arms", "World"]
SERVER_GROUP = "Server"


@asynccontextmanager
async def lifespan(app):
    # Fails the container immediately rather than serving an open panel.
    auth._password()

    advertiser = Advertiser(
        enabled=os.environ.get("LAN_DISCOVERY", "1") not in ("0", "false", "no"),
        name=os.environ.get("SERVER_NAME", "ScorchDroid"),
        port=int(os.environ.get("GAME_PORT", "27270")),
    )
    app.state.advertiser = advertiser

    async def keep_advertising():
        # The server names itself, and an operator can rename it from the
        # settings page, so the advertised name follows the server rather
        # than the environment once there is one to ask.
        while True:
            try:
                status = await asyncio.to_thread(control.status)
                if status.get("running"):
                    advertiser.update(status.get("serverName") or advertiser.name,
                                      int(status.get("port") or advertiser.port))
                    advertiser.start()
                else:
                    advertiser.stop()
            except ServerDown:
                advertiser.stop()
            except Exception as error:
                LOG.warning("discovery refresh failed: %s", error)
            await asyncio.sleep(15)

    task = asyncio.create_task(keep_advertising())
    try:
        yield
    finally:
        task.cancel()
        advertiser.stop()


app = FastAPI(title="ScorchDroid server admin", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=os.path.join(HERE, "static")), name="static")
templates = Jinja2Templates(directory=os.path.join(HERE, "templates"))


# --- session plumbing -------------------------------------------------

def session_of(request):
    return auth.read_session(request.cookies.get(auth.COOKIE_NAME))


def needs_login(request):
    """A redirect for a page, a 401 for an htmx fragment."""
    if request.headers.get("HX-Request"):
        return HTMLResponse("<p class='error'>Signed out. Reload the page.</p>", status_code=401)
    return RedirectResponse("/login", status_code=303)


def render(request, template, **context):
    session = session_of(request)
    context.setdefault("csrf", session.get("csrf") if session else "")
    context.setdefault("notice", request.query_params.get("notice"))
    context.setdefault("error", request.query_params.get("error"))
    context.setdefault("server_name", os.environ.get("SERVER_NAME", "ScorchDroid"))
    return templates.TemplateResponse(request, template, context)


def back(path, notice=None, error=None):
    from urllib.parse import urlencode

    query = urlencode({k: v for k, v in (("notice", notice), ("error", error)) if v})
    return RedirectResponse(f"{path}?{query}" if query else path, status_code=303)


# --- login ------------------------------------------------------------

@app.get("/login", response_class=HTMLResponse)
def login_form(request: Request):
    if session_of(request):
        return RedirectResponse("/", status_code=303)
    return render(request, "login.html")


@app.post("/login")
def login(request: Request, password: str = Form("")):
    if not auth.check_password(password):
        return back("/login", error="That is not the admin password.")
    response = RedirectResponse("/", status_code=303)
    response.set_cookie(
        auth.COOKIE_NAME,
        auth.issue_session(),
        max_age=auth.MAX_AGE,
        httponly=True,
        samesite="lax",
        # Only when the operator has told us the panel is behind TLS -
        # setting it unconditionally would break every plain-HTTP LAN
        # install, which is most of them.
        secure=os.environ.get("HTTPS", "0") in ("1", "true", "yes"),
    )
    return response


@app.post("/logout")
def logout():
    response = RedirectResponse("/login", status_code=303)
    response.delete_cookie(auth.COOKIE_NAME)
    return response


def guard(request, csrf_token, back_to="/"):
    """Returns a response to send instead, or None to carry on.

    [back_to] is a page, never the route being posted to: these handlers are
    POST-only, so redirecting a browser back to its own path would answer
    with 405.
    """
    session = session_of(request)
    if not session:
        return needs_login(request)
    if not auth.csrf_ok(session, csrf_token):
        return back(back_to, error="That form had gone stale. Reload the page and try again.")
    return None


# --- dashboard --------------------------------------------------------

@app.get("/", response_class=HTMLResponse)
def dashboard(request: Request):
    if not session_of(request):
        return needs_login(request)
    return render(request, "dashboard.html", status=safe_status())


def safe_status():
    try:
        return control.status()
    except ServerDown as error:
        return {"ok": False, "running": False, "down": str(error), "tanks": []}


@app.get("/fragments/status", response_class=HTMLResponse)
def fragment_status(request: Request):
    if not session_of(request):
        return needs_login(request)
    return render(request, "fragments/status.html", status=safe_status())


@app.get("/fragments/players", response_class=HTMLResponse)
def fragment_players(request: Request):
    if not session_of(request):
        return needs_login(request)
    return render(request, "fragments/players.html", status=safe_status())


@app.get("/fragments/log", response_class=HTMLResponse)
def fragment_log(request: Request, limit: int = 40):
    if not session_of(request):
        return needs_login(request)
    try:
        lines = control.log(0, limit).get("lines", [])
    except ServerDown:
        lines = []
    return render(request, "fragments/log.html", lines=lines)


@app.get("/fragments/chat", response_class=HTMLResponse)
def fragment_chat(request: Request):
    if not session_of(request):
        return needs_login(request)
    try:
        messages = control.chat(0).get("messages", [])
    except ServerDown:
        messages = []
    return render(request, "fragments/chat.html", messages=messages[-40:])


# --- admin actions ----------------------------------------------------

@app.post("/admin/{verb}")
def admin(request: Request, verb: str, csrf: str = Form(""), player: str = Form(""),
          reason: str = Form(""), bot: str = Form("Random")):
    stop = guard(request, csrf)
    if stop:
        return stop
    try:
        if verb == "addbot":
            reply = control.admin("addbot", bot)
        elif verb in ("newgame", "killall", "stopwhenempty"):
            reply = control.admin(verb)
        else:
            reply = control.admin(verb, player, reason)
    except ControlError as error:
        return back("/", error=str(error))
    except ServerDown:
        return back("/", error="The server is not running.")
    # ServerAdminCommon refuses a command against a player who has already
    # gone rather than failing loudly, so say which happened.
    if reply.get("accepted"):
        return back("/", notice=f"{verb} done.")
    return back("/", error=f"The server refused {verb} - the player may already have gone.")


@app.post("/say")
def say(request: Request, csrf: str = Form(""), text: str = Form("")):
    stop = guard(request, csrf)
    if stop:
        return stop
    if not text.strip():
        return back("/")
    try:
        control.say(text.strip())
    except (ControlError, ServerDown) as error:
        return back("/", error=str(error))
    return back("/")


@app.post("/lifecycle/{action}")
def lifecycle(request: Request, action: str, csrf: str = Form("")):
    stop = guard(request, csrf)
    if stop:
        return stop
    try:
        if action == "restart":
            control.restart()
            return back("/", notice="Restarted. Anyone who was connected has been dropped.")
        if action == "shutdown":
            control.shutdown()
            return back("/", notice="Shutting down. The container's restart policy decides what happens next.")
    except (ControlError, ServerDown) as error:
        return back("/", error=str(error))
    return back("/", error=f"Unknown action {action}.")


# --- settings ---------------------------------------------------------

def grouped_options(options):
    groups = {name: [] for name in GROUP_ORDER}
    groups[SERVER_GROUP] = []
    for option in options:
        groups.setdefault(option["group"] or SERVER_GROUP, []).append(option)
    return [(name, groups[name]) for name in GROUP_ORDER + [SERVER_GROUP] if groups.get(name)]


@app.get("/settings", response_class=HTMLResponse)
def settings(request: Request):
    if not session_of(request):
        return needs_login(request)
    try:
        options = control.options()
        mods = control.mods()
        landscapes = control.landscapes()
        bots = control.bots()
        presets = control.presets()["presets"]
    except ServerDown as error:
        return render(request, "settings.html", down=str(error))
    return render(
        request,
        "settings.html",
        groups=grouped_options(options),
        all_options=options,
        mods=mods,
        landscapes=landscapes,
        bots=bots,
        presets=presets,
    )


@app.post("/settings")
async def save_settings(request: Request):
    form = await request.form()
    stop = guard(request, form.get("csrf", ""), "/settings")
    if stop:
        return stop

    try:
        current = {option["name"]: option for option in control.options()}
    except ServerDown as error:
        return back("/settings", error=str(error))

    refused = []
    changed = 0
    for key, value in form.multi_items():
        if not key.startswith("opt."):
            continue
        name = key[4:]
        option = current.get(name)
        if option is None:
            continue
        if option["kind"] == "bool":
            # An unticked checkbox sends nothing at all, so the off case is
            # handled below rather than here.
            value = "on"
        if str(value) == option["value"]:
            continue
        reply = control.set_option(name, str(value))
        if reply.get("accepted"):
            changed += 1
        else:
            refused.append(reply.get("error") or f"{name}: the server refused \"{value}\"")

    # Now the unticked ones: upstream's bools take only "on" and "off".
    submitted = {key[4:] for key in form.keys() if key.startswith("opt.")}
    for name, option in current.items():
        if option["kind"] != "bool" or name in submitted:
            continue
        if form.get("shown." + name) is None:
            continue  # not on the tab that was submitted
        if option["value"].lower() == "off":
            continue
        reply = control.set_option(name, "off")
        if reply.get("accepted"):
            changed += 1
        else:
            refused.append(f"{name}: the server refused \"off\"")

    try:
        control.save_options()
    except (ControlError, ServerDown) as error:
        return back("/settings", error=f"Could not write the config: {error}")

    action = form.get("action", "apply")
    try:
        if action == "restart":
            control.restart()
            note = f"{changed} setting(s) saved and the server restarted."
        else:
            control.apply()
            note = f"{changed} setting(s) saved; they take effect at the next round."
    except (ControlError, ServerDown) as error:
        return back("/settings", error=f"Saved, but could not apply: {error}")

    if refused:
        return back("/settings", notice=note, error="; ".join(refused[:4]))
    return back("/settings", notice=note)


@app.post("/settings/mod")
def set_mod(request: Request, csrf: str = Form(""), mod: str = Form("none")):
    stop = guard(request, csrf, "/settings")
    if stop:
        return stop
    try:
        reply = control.set_mod(mod)
        control.save_options()
    except (ControlError, ServerDown) as error:
        return back("/settings", error=str(error))
    if not reply.get("accepted"):
        return back("/settings", error=f"The server would not switch to \"{mod}\".")
    # A mod is loaded partway through the server's own startup, long before
    # anything could be applied to a running one, so this is the one
    # setting that cannot take effect without a restart.
    substituted = reply.get("botsSubstituted", 0)
    extra = f" {substituted} bot(s) the mod does not define were substituted." if substituted else ""
    try:
        control.restart()
    except (ControlError, ServerDown) as error:
        return back("/settings", error=f"Mod set, but the restart failed: {error}")
    return back("/settings", notice=f"Now running the \"{mod}\" mod.{extra}")


@app.post("/settings/landscapes")
async def set_landscapes(request: Request):
    form = await request.form()
    stop = guard(request, form.get("csrf", ""), "/settings")
    if stop:
        return stop
    chosen = form.getlist("landscape")
    try:
        # An empty list is upstream's own "all of them", not an error.
        control.set_landscapes(chosen)
        control.save_options()
        control.apply()
    except (ControlError, ServerDown) as error:
        return back("/settings", error=str(error))
    what = f"{len(chosen)} map(s)" if chosen else "every map"
    return back("/settings", notice=f"Games will now use {what}, from the next round.")


@app.post("/settings/bots")
async def set_bots(request: Request):
    form = await request.form()
    stop = guard(request, form.get("csrf", ""), "/settings")
    if stop:
        return stop
    chosen = form.getlist("bot")
    if not chosen:
        return back("/settings", error="A game needs at least one kind of bot.")
    try:
        control.set_bots(chosen)
        control.save_options()
        control.apply()
    except (ControlError, ServerDown) as error:
        return back("/settings", error=str(error))
    return back("/settings", notice=f"Bot slots will be filled from: {', '.join(chosen)}.")


@app.post("/settings/preset")
def load_preset(request: Request, csrf: str = Form(""), gamefile: str = Form("")):
    stop = guard(request, csrf, "/settings")
    if stop:
        return stop
    try:
        reply = control.load_preset(gamefile)
        if not reply.get("accepted"):
            return back("/settings", error="That preset could not be read.")
        control.save_options()
        control.restart()
    except (ControlError, ServerDown) as error:
        return back("/settings", error=str(error))
    return back("/settings", notice="Preset loaded and the server restarted.")


@app.post("/settings/reset")
def reset_settings(request: Request, csrf: str = Form("")):
    stop = guard(request, csrf, "/settings")
    if stop:
        return stop
    try:
        control.reset_options()
    except (ControlError, ServerDown) as error:
        return back("/settings", error=str(error))
    return back("/settings", notice="Back to what the config file on disk says. Nothing saved yet.")


# --- logs -------------------------------------------------------------

@app.get("/logs", response_class=HTMLResponse)
def logs(request: Request):
    if not session_of(request):
        return needs_login(request)
    return render(request, "logs.html")


@app.get("/logs/download")
def download_logs(request: Request):
    if not session_of(request):
        return needs_login(request)
    try:
        lines = control.log(0, 2000).get("lines", [])
    except ServerDown:
        lines = []
    body = "\n".join(f"[{line['time']}] {line['message']}" for line in lines)
    return Response(
        body,
        media_type="text/plain",
        headers={"Content-Disposition": "attachment; filename=scorchdroid-server.log"},
    )


@app.get("/healthz", response_class=PlainTextResponse)
def healthz():
    try:
        control.call("ping")
        return "ok"
    except ServerDown:
        return PlainTextResponse("server down", status_code=503)
