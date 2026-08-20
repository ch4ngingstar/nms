"""Application factory: wiring only (spec §5).

The server must run as a single process — SSE fan-out is in-memory, so multiple
workers would each hold a separate event bus (spec §4). Do not deploy under
Gunicorn with more than one worker without replacing events.py.
"""

import os

from flask import Flask

from server.api import api
from server.db import db
from server.stream import stream
from server.ui import ui


def create_app(config: dict | None = None) -> Flask:
    """Build the Flask app. Refuses to start without a SECRET_KEY (spec §11)."""
    app = Flask(__name__)
    app.config.update(
        SQLALCHEMY_DATABASE_URI=os.environ.get(
            "NMS_DATABASE_URI", "sqlite:///nms.db"),
        SQLALCHEMY_TRACK_MODIFICATIONS=False,
        SECRET_KEY=os.environ.get("SECRET_KEY"),
    )
    if config:
        app.config.update(config)

    # A default key must never reach a deployment (spec §11).
    if not app.config.get("SECRET_KEY"):
        raise RuntimeError(
            "SECRET_KEY is not set; refusing to start with an insecure default")

    db.init_app(app)
    app.register_blueprint(api)
    app.register_blueprint(stream)
    app.register_blueprint(ui)
    return app
