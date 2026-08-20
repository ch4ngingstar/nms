"""Static delivery of the operations console (spec §10).

The console is a single self-contained page under templates/ that talks to the
REST + SSE API on the same origin. Serving it from the app keeps everything
one process, one origin — no CORS, and the browser session cookie authenticates
the API and the SSE stream alike. The page falls back to a self-contained demo
when the API is unauthenticated, so the route is useful before login too.
"""

from pathlib import Path

from flask import Blueprint, send_from_directory

# Repo-root templates/, not server/templates/ — the console ships with the other
# top-level deliverables, beside protocol/ and firmware/.
_TEMPLATES = Path(__file__).resolve().parent.parent / "templates"

ui = Blueprint("ui", __name__)


@ui.get("/")
def index():
    return send_from_directory(_TEMPLATES, "index.html")
