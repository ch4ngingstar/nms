"""Application factory: wiring only.

The server must run as a single process — SSE fan-out is in-memory, so multiple
workers would each hold a separate event bus. Do not deploy under Gunicorn with
more than one worker without replacing events.py.
"""

import os
from pathlib import Path

from flask import Flask

from server.api import api
from server.db import db
from server.stream import stream
from server.ui import ui

# Repo-root static/, not server/static — the console's CSS/JS ship beside
# templates/index.html at the top level (see server/ui.py).
_STATIC = Path(__file__).resolve().parent.parent / "static"


def create_app(config: dict | None = None) -> Flask:
    """Build the Flask app. Refuses to start without a SECRET_KEY."""
    app = Flask(__name__, static_folder=str(_STATIC), static_url_path="/static")
    app.config.update(
        SQLALCHEMY_DATABASE_URI=os.environ.get(
            "NMS_DATABASE_URI", "sqlite:///nms.db"),
        SQLALCHEMY_TRACK_MODIFICATIONS=False,
        SECRET_KEY=os.environ.get("SECRET_KEY"),
    )
    if config:
        app.config.update(config)

    # A default key must never reach a deployment.
    if not app.config.get("SECRET_KEY"):
        raise RuntimeError(
            "SECRET_KEY is not set; refusing to start with an insecure default")

    db.init_app(app)
    app.register_blueprint(api)
    app.register_blueprint(stream)
    app.register_blueprint(ui)
    return app
