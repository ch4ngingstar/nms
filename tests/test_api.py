import pytest
from werkzeug.security import generate_password_hash

from server.db import db as _db
from server.models import (
    ApObservation, Device, Job, JobChunk, Node, Telemetry,
)

PW = "correct horse battery staple"


@pytest.fixture
def sink(monkeypatch):
    """Capture MQTT publishes so job/monitor dispatch does not need a broker."""
    sent = []
    from server import commands
    monkeypatch.setattr(commands, "_publisher",
                        lambda topic, payload: sent.append((topic, payload)))
    return sent


@pytest.fixture
def api_app(monkeypatch, sink):
    monkeypatch.setenv("NMS_ADMIN_PASSWORD_HASH", generate_password_hash(PW))
    from server.app import create_app

    app = create_app({"SECRET_KEY": "test-secret",
                      "SQLALCHEMY_DATABASE_URI": "sqlite:///:memory:",
                      "TESTING": True})
    with app.app_context():
        _db.create_all()
        seed()
        yield app
        _db.session.remove()
        _db.drop_all()


def seed():
    _db.session.add(Node(node_id="probe-a4c1f8", label="Lab North", fw="1.2.0",
                         chip="esp32s3", mac="a0:b7:65:a4:c1:f8",
                         capabilities=["port_scan", "wifi_survey"], state="online"))
    _db.session.add(Device(id=1, name="Main Router", ip="192.168.1.1", role="router"))
    _db.session.commit()


@pytest.fixture
def client(api_app):
    return api_app.test_client()


@pytest.fixture
def auth_client(client):
    client.post("/api/login", json={"password": PW})
    return client


# --- auth gate -------------------------------------------------------------

def test_api_requires_authentication(client):
    assert client.get("/api/nodes").status_code == 401


def test_login_then_access(auth_client):
    assert auth_client.get("/api/nodes").status_code == 200


def test_logout_revokes(auth_client):
    auth_client.post("/api/logout")
    assert auth_client.get("/api/nodes").status_code == 401


# --- nodes -----------------------------------------------------------------

def test_list_nodes(auth_client):
    body = auth_client.get("/api/nodes").get_json()
    assert body[0]["node_id"] == "probe-a4c1f8"
    assert body[0]["capabilities"] == ["port_scan", "wifi_survey"]


def test_node_detail_includes_latest_telemetry(auth_client, api_app):
    from datetime import datetime, timezone
    with api_app.app_context():
        _db.session.add(Telemetry(node_id="probe-a4c1f8",
                                  ts=datetime(2026, 8, 16, tzinfo=timezone.utc),
                                  free_heap=198320, uptime_s=10, state="online"))
        _db.session.commit()
    body = auth_client.get("/api/nodes/probe-a4c1f8").get_json()
    assert body["telemetry"]["free_heap"] == 198320


def test_enrol_node_returns_credentials(auth_client):
    response = auth_client.post("/api/nodes",
                                json={"node_id": "probe-7e2b10", "label": "Lab South"})
    assert response.status_code == 201
    body = response.get_json()
    assert body["password"]
    assert "user probe-7e2b10" in body["acl_block"]


def test_enrol_rejects_bad_node_id(auth_client):
    assert auth_client.post("/api/nodes", json={"node_id": "nope"}).status_code == 400


# --- devices ---------------------------------------------------------------

def test_list_devices(auth_client):
    body = auth_client.get("/api/devices").get_json()
    assert body[0]["name"] == "Main Router"


def test_create_device_pushes_set_monitor(auth_client, sink):
    sink.clear()
    response = auth_client.post("/api/devices",
                                json={"name": "NAS", "ip": "192.168.1.20",
                                      "role": "storage", "node_id": "probe-a4c1f8"})
    assert response.status_code == 201
    assert any(t.endswith("/cmd") for t, _ in sink)


def test_patch_device(auth_client):
    response = auth_client.patch("/api/devices/1", json={"role": "gateway"})
    assert response.status_code == 200
    assert response.get_json()["role"] == "gateway"


def test_delete_device(auth_client, api_app):
    assert auth_client.delete("/api/devices/1").status_code == 204
    with api_app.app_context():
        assert _db.session.get(Device, 1) is None


# --- jobs ------------------------------------------------------------------

def test_issue_job_returns_202_and_job_id(auth_client):
    response = auth_client.post("/api/nodes/probe-a4c1f8/jobs",
                                json={"cmd": "port_scan",
                                      "args": {"targets": ["10.0.0.5"], "ports": "22"}})
    assert response.status_code == 202
    assert response.get_json()["job_id"].startswith("job-")


def test_issue_job_uncapable_command_is_400(auth_client):
    response = auth_client.post("/api/nodes/probe-a4c1f8/jobs",
                                json={"cmd": "dns", "args": {"targets": ["x.com"]}})
    assert response.status_code == 400


def test_issue_job_unknown_node_is_404(auth_client):
    response = auth_client.post("/api/nodes/probe-000000/jobs",
                                json={"cmd": "identify", "args": {}})
    assert response.status_code == 404


def test_issue_job_bad_args_is_400(auth_client):
    response = auth_client.post("/api/nodes/probe-a4c1f8/jobs",
                                json={"cmd": "port_scan", "args": {"ports": ""}})
    assert response.status_code == 400


def test_get_job_state(auth_client, api_app):
    with api_app.app_context():
        _db.session.add(Job(job_id="job-abc", node_id="probe-a4c1f8",
                            cmd="port_scan", args={}, state="done"))
        _db.session.commit()
    body = auth_client.get("/api/jobs/job-abc").get_json()
    assert body["state"] == "done"


def test_get_job_chunks(auth_client, api_app):
    with api_app.app_context():
        _db.session.add(Job(job_id="job-abc", node_id="probe-a4c1f8",
                            cmd="port_scan", args={}, state="done"))
        _db.session.add(JobChunk(job_id="job-abc", seq=0, payload={"open": []}))
        _db.session.commit()
    body = auth_client.get("/api/jobs/job-abc/chunks").get_json()
    assert body[0]["seq"] == 0


def test_get_unknown_job_is_404(auth_client):
    assert auth_client.get("/api/jobs/job-nope").status_code == 404


def test_cancel_job(auth_client, api_app, sink):
    with api_app.app_context():
        _db.session.add(Job(job_id="job-abc", node_id="probe-a4c1f8",
                            cmd="port_scan", args={}, state="accepted"))
        _db.session.commit()
    sink.clear()
    assert auth_client.post("/api/jobs/job-abc/cancel").status_code == 202
    assert any(t.endswith("/cmd") for t, _ in sink)


def test_cancel_unknown_job_is_404(auth_client):
    assert auth_client.post("/api/jobs/job-nope/cancel").status_code == 404


# --- rf --------------------------------------------------------------------

def test_rf_aps_group_by_bssid(auth_client, api_app):
    from datetime import datetime, timezone
    with api_app.app_context():
        _db.session.add(Job(job_id="job-rf", node_id="probe-a4c1f8",
                            cmd="wifi_survey", args={}, state="done"))
        _db.session.commit()
        _db.session.add(ApObservation(
            node_id="probe-a4c1f8", job_id="job-rf", bssid="a0:b7:65:11:22:33",
            ssid="Home", channel=6, rssi=-61, auth="wpa2", hidden=False,
            observed_at=datetime(2026, 8, 16, tzinfo=timezone.utc)))
        _db.session.commit()
    body = auth_client.get("/api/rf/aps").get_json()
    assert body[0]["bssid"] == "a0:b7:65:11:22:33"
    assert body[0]["observations"][0]["node"] == "probe-a4c1f8"
    assert body[0]["observations"][0]["rssi"] == -61
