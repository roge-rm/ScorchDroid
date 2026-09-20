"""One password, a signed cookie, and a CSRF token.

Deliberately small. This guards kick, ban and "rewrite every server setting",
so it has to exist; it is not trying to be an identity system for a game
server that has exactly one operator.
"""

import hashlib
import hmac
import os
import secrets

from itsdangerous import BadSignature, URLSafeTimedSerializer

COOKIE_NAME = "scorchdroid_admin"
MAX_AGE = int(os.environ.get("SESSION_MAX_AGE", str(7 * 24 * 3600)))


class MissingPassword(RuntimeError):
    pass


def _password():
    password = os.environ.get("ADMIN_PASSWORD", "")
    if not password:
        # Refusing to start is the whole point: an admin panel that came up
        # open because a variable was missing from an .env file is exactly
        # the accident worth making impossible.
        raise MissingPassword(
            "ADMIN_PASSWORD is not set. The web admin can kick, ban and rewrite "
            "every server setting, so it will not start without one. Put it in "
            "your .env file (see .env.example)."
        )
    return password


def _secret():
    # Derived from the password by default so that restarting the container
    # does not sign everyone out, while still changing the moment the
    # password does. An explicit SESSION_SECRET wins if one is set.
    explicit = os.environ.get("SESSION_SECRET", "")
    if explicit:
        return explicit
    return hashlib.sha256(("scorchdroid-session:" + _password()).encode("utf-8")).hexdigest()


def check_password(candidate):
    # Constant time, because the password is the only thing standing in
    # front of the admin surface.
    return hmac.compare_digest(candidate or "", _password())


def issue_session():
    serializer = URLSafeTimedSerializer(_secret())
    return serializer.dumps({"csrf": secrets.token_urlsafe(24)})


def read_session(token):
    if not token:
        return None
    try:
        return URLSafeTimedSerializer(_secret()).loads(token, max_age=MAX_AGE)
    except BadSignature:
        return None
    except Exception:
        return None


def csrf_ok(session, submitted):
    if not session:
        return False
    return hmac.compare_digest(session.get("csrf", ""), submitted or "")
