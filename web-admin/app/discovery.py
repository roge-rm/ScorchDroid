"""Advertises the server on the LAN the way an Android host advertises itself.

LanDiscovery.kt registers and discovers `_scorchdroid._tcp.` over NSD/mDNS,
and the phone's Join Game list is built from whatever answers. A dedicated
server that publishes the same service therefore appears in that list with no
address to type - which is the one thing a desktop Scorched3D server has
never been able to do for an Android client.

Multicast cannot cross a bridge network, so this only does anything when the
container runs with host networking. When it cannot, it says so once and
stops; manual address entry still works and is what the bridge compose file
expects.
"""

import logging
import os
import socket

LOG = logging.getLogger("scorchdroid.discovery")

# Exactly the string LanDiscovery.SERVICE_TYPE uses, in python-zeroconf's
# spelling. If this drifts, phones stop seeing the server and nothing else
# breaks, which is the kind of failure worth naming in one place.
SERVICE_TYPE = "_scorchdroid._tcp.local."


class Advertiser:
    def __init__(self, enabled, name, port):
        self.enabled = enabled
        self.name = name
        self.port = port
        self._zeroconf = None
        self._info = None

    def _service_name(self):
        # "ScorchDroid-<something>" is the shape an Android host publishes
        # (ScorchDroid-<device model>), and the phone shows the name as-is.
        safe = "".join(c for c in self.name if c.isalnum() or c in " -_") or "Server"
        return f"ScorchDroid-{safe}.{SERVICE_TYPE}"

    def start(self):
        if not self.enabled or self._zeroconf is not None:
            return

        zeroconf = None
        try:
            from zeroconf import ServiceInfo, Zeroconf

            address = _lan_address()
            self._info = ServiceInfo(
                SERVICE_TYPE,
                self._service_name(),
                addresses=[socket.inet_aton(address)],
                port=self.port,
                properties={"name": self.name},
            )
            # Bound to the one address players reach this machine on, not
            # to every interface. With the default (all of them) the
            # responder hears its own announcement come back on loopback or
            # a docker bridge, decides the name is taken, and refuses to
            # register - which is what host networking on a machine with
            # more than one interface looks like, and it is intermittent,
            # so it will pass in a quick test and fail in the loop.
            zeroconf = Zeroconf(interfaces=[address])
            # And if the name really is taken - a second server on the same
            # LAN - let it rename itself rather than give up. The phone
            # shows whatever name answers.
            zeroconf.register_service(self._info, allow_name_change=True)
            self._zeroconf = zeroconf
            LOG.info("Advertising %s on %s:%s", self._info.name, address, self.port)
        except Exception as error:
            # Never fatal. A server nobody can auto-discover is still a
            # server, and the bridge compose file runs this way on purpose.
            #
            # The type matters as much as the message: zeroconf's
            # NonUniqueNameException carries no text at all, so logging
            # str(error) alone prints empty brackets and says nothing.
            LOG.warning(
                "LAN discovery unavailable (%s: %s) - players can still join by address",
                type(error).__name__, error or "no detail",
            )
            if zeroconf is not None:
                # Closing it matters: a Zeroconf left open keeps its
                # threads and its sockets, and the next attempt then
                # collides with the records this one already put out.
                try:
                    zeroconf.close()
                except Exception:
                    pass
            self._zeroconf = None
            self._info = None

    def stop(self):
        if self._zeroconf is None:
            return
        try:
            self._zeroconf.unregister_service(self._info)
            self._zeroconf.close()
        except Exception:
            pass
        self._zeroconf = None
        self._info = None

    def update(self, name, port):
        """Re-registers if the server's name or port has changed under us."""
        if not self.enabled:
            return
        if name == self.name and port == self.port and self._zeroconf is not None:
            return
        self.name, self.port = name, port
        self.stop()
        self.start()


def _lan_address():
    """This host's address on the network a player would reach it from.

    Asking a UDP socket where it would send is the usual trick: it picks the
    routable interface without sending anything, which beats gethostbyname()
    on a machine whose hostname resolves to 127.0.1.1.
    """
    override = os.environ.get("ADVERTISE_ADDRESS", "")
    if override:
        return override
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.0.2.1", 9))  # TEST-NET-1: routed, never answers
        return probe.getsockname()[0]
    finally:
        probe.close()
