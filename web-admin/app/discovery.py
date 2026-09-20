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
        try:
            from zeroconf import ServiceInfo, Zeroconf

            address = socket.inet_aton(_lan_address())
            self._info = ServiceInfo(
                SERVICE_TYPE,
                self._service_name(),
                addresses=[address],
                port=self.port,
                properties={"name": self.name},
            )
            self._zeroconf = Zeroconf()
            self._zeroconf.register_service(self._info)
            LOG.info("Advertising %s on port %s", self._service_name(), self.port)
        except Exception as error:
            # Never fatal. A server nobody can auto-discover is still a
            # server, and the bridge compose file runs this way on purpose.
            LOG.warning("LAN discovery unavailable (%s) - players can still join by address", error)
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
