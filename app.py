"""Entry point: build the app via the factory.

The models, polling thread, and TCP probing that once lived here (415 lines)
now belong to the server/ and probe/ packages. This file is wiring only.
See docs/design/2026-08-16-server-c2-design.md.
"""

from server.app import create_app
from server.db import db

app = create_app()

if __name__ == "__main__":
    with app.app_context():
        db.create_all()
    # Single process only: SSE fan-out is in-memory.
    app.run(debug=True, use_reloader=False)
