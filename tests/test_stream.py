import json

import pytest
from werkzeug.security import generate_password_hash

from server.db import db as _db
from server.stream import format_sse

PW = "correct horse battery staple"


def test_format_sse_frames_an_event():
    frame = format_sse("node_status", {"node": "probe-a4c1f8", "state": "online"})
    lines = frame.split("\n")
    assert lines[0] == "event: node_status"
    assert json.loads(lines[1].removeprefix("data: "))["state"] == "online"
    assert frame.endswith("\n\n")


@pytest.fixture
def client(monkeypatch):
    monkeypatch.setenv("NMS_ADMIN_PASSWORD_HASH", generate_password_hash(PW))
    from server.app import create_app
    app = create_app({"SECRET_KEY": "t",
                      "SQLALCHEMY_DATABASE_URI": "sqlite:///:memory:",
                      "TESTING": True})
    with app.app_context():
        _db.create_all()
        yield app.test_client()
        _db.session.remove()
        _db.drop_all()


def test_stream_requires_auth(client):
    """The gate must return before the endpoint blocks on the event queue."""
    assert client.get("/api/stream").status_code == 401
