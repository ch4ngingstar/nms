"""Static delivery of the operations console.

The page itself is served here; its CSS/JS ship under the app's static folder
(wired to the repo-root static/ in server/app.py, not Flask's server/static
default) and are served by Flask's normal static handling. Serving everything
from the app keeps it one process, one origin — no CORS, and the browser
session cookie authenticates the API and the SSE stream alike. The page falls
back to a self-contained demo when the API is unauthenticated, so the route
is useful before login too.
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
