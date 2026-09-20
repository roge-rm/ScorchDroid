"""The client half of the dedicated server's control channel.

Requests are one tab-separated line, replies one line of JSON - see
dedicated-server/ControlServer.cpp, which is the other end and the only
document of record for the command list.

A new connection per call. The server accepts as many as it is asked for and
serves them from its own tick, and a short-lived connection means a page that
hangs cannot wedge a socket the next page needs.
"""

import json
import os
import socket
import threading

DEFAULT_SOCKET = os.environ.get("SCORCHDROID_CONTROL_SOCKET", "/run/scorchdroid/control.sock")
DEFAULT_TIMEOUT = float(os.environ.get("CONTROL_TIMEOUT", "10"))


class ServerDown(Exception):
    """The server is not listening.

    Not an error condition in itself: the container restarts, and a restart
    from the web UI deliberately takes the socket away for a second or two.
    Every page is expected to render this as a state rather than a failure.
    """


class ControlError(Exception):
    """The server answered, and the answer was no."""


def _escape(value):
    return str(value).replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")


class Control:
    def __init__(self, path=DEFAULT_SOCKET, timeout=DEFAULT_TIMEOUT):
        self.path = path
        self.timeout = timeout
        # Requests are serialised because the server handles them on its own
        # game loop; several browsers polling at once otherwise queue up
        # behind each other in the kernel anyway, and this keeps the failure
        # mode a timeout rather than a pile of half-open sockets.
        self._lock = threading.Lock()

    def call(self, *args):
        line = "\t".join(_escape(a) for a in args) + "\n"
        with self._lock:
            try:
                connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                connection.settimeout(self.timeout)
                connection.connect(self.path)
            except FileNotFoundError as error:
                # The socket only exists while the server is up, so this is
                # what a restart, a crash and "never started" all look like.
                raise ServerDown("It is not running, or is still starting up.") from error
            except ConnectionRefusedError as error:
                raise ServerDown("Its control socket is there but nothing is listening.") from error
            except PermissionError as error:
                raise ServerDown(
                    f"This container is not allowed to open {self.path} - check that both "
                    "services share the control volume and run as the same user."
                ) from error
            except OSError as error:
                raise ServerDown(str(error)) from error

            try:
                connection.sendall(line.encode("utf-8"))
                chunks = []
                while True:
                    chunk = connection.recv(65536)
                    if not chunk:
                        break
                    chunks.append(chunk)
                    if b"\n" in chunk:
                        break
            except socket.timeout as error:
                raise ServerDown("the server did not answer in time") from error
            finally:
                connection.close()

        raw = b"".join(chunks).split(b"\n", 1)[0]
        if not raw:
            raise ServerDown("the server closed the connection without answering")
        # The server escapes and validates its own UTF-8, so this cannot
        # fail on a hostile player name - see Json.hpp.
        return json.loads(raw.decode("utf-8"))

    def demand(self, *args):
        """call(), but an `ok: false` reply is raised rather than returned."""
        reply = self.call(*args)
        if not reply.get("ok"):
            raise ControlError(reply.get("error", "the server refused the command"))
        return reply

    # --- the commands, named as the pages use them --------------------

    def status(self):
        return self.call("status")

    def log(self, after=0, limit=500):
        return self.call("log", after, limit)

    def chat(self, after=0):
        return self.call("chat", after)

    def say(self, text, channel="general"):
        return self.demand("say", channel, text)

    def admin(self, verb, *args):
        return self.demand("admin", verb, *args)

    def options(self):
        return self.demand("options")["options"]

    def set_option(self, name, value):
        return self.call("options.set", name, value)

    def save_options(self):
        return self.demand("options.save")

    def reset_options(self):
        return self.demand("options.reset")

    def apply(self):
        return self.demand("apply")

    def restart(self):
        return self.demand("restart")

    def shutdown(self):
        return self.demand("shutdown")

    def mods(self):
        return self.demand("mods")

    def set_mod(self, name):
        return self.demand("setmod", name)

    def landscapes(self):
        return self.demand("landscapes")

    def set_landscapes(self, names):
        return self.demand("setlandscapes", *names)

    def bots(self):
        return self.demand("bots")

    def set_bots(self, names):
        return self.demand("setbots", *names)

    def presets(self):
        return self.demand("presets")

    def load_preset(self, path):
        return self.demand("loadpreset", path)
