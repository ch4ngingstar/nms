"""Admin authentication for the web console.

A single admin password, hashed with werkzeug and read from
`NMS_ADMIN_PASSWORD_HASH` in the environment. Login establishes a Flask
session; `@require_auth` guards every `/api/*` route except `/api/login`.
The hash is read at call time so it is never baked into the process.
"""

import os
from functools import wraps

from flask import jsonify, session
from werkzeug.security import check_password_hash

SESSION_KEY = "authenticated"


def _admin_hash() -> str | None:
    return os.environ.get("NMS_ADMIN_PASSWORD_HASH")


def check_password(password: str) -> bool:
    """True only if a hash is configured and the password matches it."""
    stored = _admin_hash()
    return bool(stored) and check_password_hash(stored, password)


def login(password: str) -> bool:
    """Establish an authenticated session for a correct password."""
    if check_password(password):
        session[SESSION_KEY] = True
        return True
    return False


def logout() -> None:
    session.pop(SESSION_KEY, None)


def is_authenticated() -> bool:
    return bool(session.get(SESSION_KEY))


def require_auth(view):
    """Reject unauthenticated callers with 401 before the view runs."""

    @wraps(view)
    def wrapper(*args, **kwargs):
        if not is_authenticated():
            return jsonify({"error": "authentication required"}), 401
        return view(*args, **kwargs)

    return wrapper
