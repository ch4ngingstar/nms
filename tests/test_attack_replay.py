"""End-to-end attack replay: a deauthentication flood observed by a probe's
wifi_ids runner must reach the operator through the REST API.

Exercises the whole detection pipeline in one test — the ingest projection
(server.ingest._project_ids_alerts) plus the operator-facing timeline
(GET /api/security/alerts) and the rogue-AP filter — which the ingest-only
tests in test_ingest_extended.py do not cover.
"""

import pytest
from werkzeug.security import generate_password_hash

from server.db import db as _db
from server.ingest import handle_result, to_datetime
from server.models import IdsAlert, Job, Node

PW = "correct horse battery staple"
TS = 1755302400


@pytest.fixture
def app(monkeypatch):
    monkeypatch.setenv("NMS_ADMIN_PASSWORD_HASH", generate_password_hash(PW))
    from server.app import create_app

    application = create_app({"SECRET_KEY": "test-secret",
                              "SQLALCHEMY_DATABASE_URI": "sqlite:///:memory:",
                              "TESTING": True})
    with application.app_context():
        _db.create_all()
        _db.session.add(Node(node_id="probe-a4c1f8", label="Lab North", fw="1.2.0",
                             chip="esp32s3", mac="a0:b7:65:a4:c1:f8",
                             capabilities=["wifi_ids"], state="online"))
        _db.session.commit()
        _db.session.add(Job(job_id="job-ids01", node_id="probe-a4c1f8",
                            cmd="wifi_ids", args={},
                            created_at=to_datetime(TS), last_event_at=to_datetime(TS)))
        _db.session.commit()
        yield application
        _db.session.remove()
        _db.drop_all()


@pytest.fixture
def auth_client(app):
    client = app.test_client()
    client.post("/api/login", json={"password": PW})
    return client


def deauth_flood_chunk():
    return {"v": 1, "type": "result", "node": "probe-a4c1f8",
            "msg_id": "01J8X2KB0007", "ts": TS,
            "data": {"event": "chunk", "job_id": "job-ids01", "seq": 0,
                     "alerts": [{"type": "deauth_flood",
                                 "source_mac": "de:ad:be:ef:00:11",
                                 "target_mac": "a0:b7:65:11:22:33",
                                 "channel": 6, "count": 240,
                                 "first_seen": TS - 5, "last_seen": TS}]}}


def test_deauth_flood_reaches_operator_timeline(app, auth_client):
    assert auth_client.get("/api/security/alerts").get_json() == []

    handle_result(deauth_flood_chunk())

    assert _db.session.query(IdsAlert).count() == 1

    body = auth_client.get("/api/security/alerts").get_json()
    assert len(body) == 1
    assert body[0]["alert_type"] == "deauth_flood"
    assert body[0]["source_mac"] == "de:ad:be:ef:00:11"
    assert body[0]["count"] == 240
    assert body[0]["channel"] == 6
    assert body[0]["node_id"] == "probe-a4c1f8"


def test_deauth_flood_excluded_from_rogue_ap_filter(app, auth_client):
    handle_result(deauth_flood_chunk())
    assert auth_client.get("/api/security/rogue-aps").get_json() == []
