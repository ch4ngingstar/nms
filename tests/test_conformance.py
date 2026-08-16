"""Conformance suite over MQTT (protocol spec §9 layer 3, server spec §13).

These are scenario tests pointed at a live broker and a live probe. They run
against the virtual probe here and would run unchanged against firmware in
subsystem 3. If the Docker daemon is unavailable they SKIP rather than fail, so
the unit suite still runs everywhere (spec §13).

Scenarios (spec §13):
  1. Clean streamed job: accepted -> chunks -> done
  2. Deliberate seq gap -> job lands `incomplete`, gaps recorded
  5. Graceful `surveying` disconnect -> no false offline
  6. Ungraceful kill -> Last Will fires -> node marked offline

Scenarios 3 (busy) and 4 (cancel) are covered at the unit level in
test_virtual_probe.py, where their timing is deterministic.
"""

import json
import socket
import subprocess
import time
import uuid

import pytest

paho = pytest.importorskip("paho.mqtt.client")

from probe.virtual_probe import VirtualProbe
from protocol.topics import cmd_topic, result_topic, status_topic
from server import commands
from server.app import create_app
from server.db import db as _db
from server.models import Job, Node
from server.mqtt_bridge import MqttBridge

NODE = "probe-server"


def _daemon_up() -> bool:
    try:
        return subprocess.run(["docker", "version"],
                              capture_output=True, timeout=15).returncode == 0
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _daemon_up(),
                                reason="docker daemon unavailable")


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _wait_port(host: str, port: int, timeout_s: float = 20.0) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=2):
                return
        except OSError:
            time.sleep(0.3)
    raise TimeoutError(f"{host}:{port} never opened")


def _wait_until(predicate, timeout_s: float = 15.0, interval: float = 0.2):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(interval)
    return predicate()


@pytest.fixture(scope="module")
def broker(tmp_path_factory):
    port = _free_port()
    cfg = tmp_path_factory.mktemp("mosq")
    (cfg / "mosquitto.conf").write_text(
        "listener 1883\nallow_anonymous true\n", encoding="utf-8")
    name = f"nms-test-mosq-{uuid.uuid4().hex[:8]}"
    started = subprocess.run(
        ["docker", "run", "-d", "--rm", "--name", name,
         "-p", f"{port}:1883", "-v", f"{cfg}:/mosquitto/config",
         "eclipse-mosquitto:2"],
        capture_output=True, text=True)
    if started.returncode != 0:
        pytest.skip(f"could not start broker: {started.stderr.strip()}")
    try:
        _wait_port("127.0.0.1", port)
        yield ("127.0.0.1", port)
    finally:
        subprocess.run(["docker", "rm", "-f", name], capture_output=True)


@pytest.fixture
def server(broker, tmp_path):
    """A server app whose bridge is connected to the live broker.

    A file-backed SQLite database is used (not :memory:) so the bridge thread,
    which ingests on the paho loop thread, shares one database with the test.
    """
    host, port = broker
    db_path = tmp_path / "conformance.db"
    app = create_app({
        "SECRET_KEY": "conformance",
        "SQLALCHEMY_DATABASE_URI": f"sqlite:///{db_path}",
        "SQLALCHEMY_ENGINE_OPTIONS": {"connect_args": {"check_same_thread": False}},
        "TESTING": True,
    })
    with app.app_context():
        _db.create_all()
    bridge = MqttBridge(app, host, port, client_id=f"nms-server-{uuid.uuid4().hex[:6]}")
    bridge.start()
    _wait_until(lambda: bridge.client.is_connected(), timeout_s=10)
    try:
        yield app, bridge
    finally:
        bridge.stop()


class LiveProbe:
    """A virtual probe wired to a real paho client for transport."""

    def __init__(self, host, port, node_id=NODE, keepalive=60):
        self.node_id = node_id
        self.client = paho.Client(paho.CallbackAPIVersion.VERSION2,
                                  client_id=node_id)
        self.probe = VirtualProbe(
            node_id, publish=lambda t, p: self.client.publish(t, p, qos=1))
        lwt = json.dumps({"v": 1, "type": "status", "node": node_id,
                          "msg_id": uuid.uuid4().hex[:20], "ts": int(time.time()),
                          "data": {"state": "offline", "reason": "lwt"}})
        self.client.will_set(status_topic(node_id), lwt, qos=1)
        self.client.on_connect = self._on_connect
        self.client.on_message = lambda c, u, m: self.probe.handle_cmd(
            json.loads(m.payload))
        self.client.connect(host, port, keepalive=keepalive)
        self.client.loop_start()
        _wait_until(lambda: self.client.is_connected(), timeout_s=10)

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        client.subscribe(cmd_topic(self.node_id), qos=1)
        self.probe.announce()
        self.probe.status("online")

    def stop_gracefully(self):
        self.probe.status("offline", reason="shutdown")
        time.sleep(0.3)
        self.client.loop_stop()
        self.client.disconnect()   # clean disconnect suppresses the Last Will

    def kill(self):
        # Drop the socket without a DISCONNECT so the broker fires the Will.
        self.client.loop_stop()
        try:
            self.client.socket().close()
        except Exception:
            pass


def _job_state(app, job_id):
    with app.app_context():
        job = _db.session.get(Job, job_id)
        return None if job is None else job.state


# --- scenario 1 ------------------------------------------------------------

def test_clean_streamed_job_reaches_done(server):
    app, bridge = server
    probe = LiveProbe(*bridge_addr(bridge))
    try:
        _wait_until(lambda: _node_exists(app, NODE))
        with app.app_context():
            # Read job_id inside the context: the ORM object detaches when the
            # context (and its session) closes, so grab the plain string now.
            job_id = commands.create_job(NODE, "dns", {"targets": ["localhost"]}).job_id
        assert _wait_until(lambda: _job_state(app, job_id) == "done") is True
    finally:
        probe.stop_gracefully()


# --- scenario 2 ------------------------------------------------------------

def test_sequence_gap_lands_incomplete(server):
    """Publish chunks 0 and 2 then done directly: the server must see the gap."""
    app, bridge = server
    host, port = bridge_addr(bridge)
    with app.app_context():
        # Commit the node before the job: with only a bare ForeignKey column and
        # no relationship() between them, SQLAlchemy's unit of work does not
        # guarantee the parent insert precedes the child within one flush, so the
        # job could be inserted first and trip the FK. Production never inserts
        # both together — a node is always enrolled in an earlier transaction.
        _db.session.add(Node(node_id=NODE, capabilities=["port_scan"],
                             state="online"))
        _db.session.commit()
        _db.session.add(Job(job_id="job-gap", node_id=NODE, cmd="port_scan",
                            args={}, state="accepted"))
        _db.session.commit()

    publisher = paho.Client(paho.CallbackAPIVersion.VERSION2,
                            client_id=f"gap-{uuid.uuid4().hex[:6]}")
    publisher.connect(host, port)
    publisher.loop_start()
    _wait_until(lambda: publisher.is_connected(), timeout_s=10)
    try:
        for seq in (0, 2):
            _publish_result(publisher, {"event": "chunk", "job_id": "job-gap",
                                        "seq": seq, "open": []})
        _publish_result(publisher, {"event": "done", "job_id": "job-gap",
                                    "chunks": 2, "results": 2, "duration_ms": 5})
        _wait_until(lambda: _job_state(app, "job-gap") == "incomplete")
    finally:
        publisher.loop_stop()
        publisher.disconnect()

    with app.app_context():
        job = _db.session.get(Job, "job-gap")
        assert job.state == "incomplete"
        assert job.gaps == [1]


# --- scenario 5 ------------------------------------------------------------

def test_graceful_surveying_disconnect_is_not_offline(server):
    app, bridge = server
    probe = LiveProbe(*bridge_addr(bridge))
    _wait_until(lambda: _node_exists(app, NODE))
    probe.probe.status("surveying", expect_back_in=30)
    _wait_until(lambda: _node_state(app, NODE) == "surveying")
    probe.client.loop_stop()
    probe.client.disconnect()          # graceful: no Last Will
    time.sleep(1.0)
    assert _node_state(app, NODE) == "surveying"


# --- scenario 6 ------------------------------------------------------------

def test_ungraceful_kill_marks_node_offline(server):
    app, bridge = server
    probe = LiveProbe(*bridge_addr(bridge), keepalive=2)
    _wait_until(lambda: _node_state(app, NODE) in {"online", "surveying"})
    probe.kill()
    # The broker publishes the Will after ~1.5x the keepalive interval.
    state = _wait_until(lambda: _node_state(app, NODE) == "offline", timeout_s=15)
    assert state is True or _node_state(app, NODE) == "offline"


# --- helpers ---------------------------------------------------------------

def bridge_addr(bridge):
    return bridge.host, bridge.port


def _publish_result(client, data):
    message = {"v": 1, "type": "result", "node": NODE,
               "msg_id": uuid.uuid4().hex[:20], "ts": int(time.time()),
               "data": data}
    client.publish(result_topic(NODE), json.dumps(message).encode("utf-8"), qos=1)


def _node_exists(app, node_id):
    with app.app_context():
        return _db.session.get(Node, node_id) is not None


def _node_state(app, node_id):
    with app.app_context():
        node = _db.session.get(Node, node_id)
        return None if node is None else node.state
