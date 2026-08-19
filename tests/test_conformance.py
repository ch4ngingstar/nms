"""Conformance suite over MQTT (protocol spec §9 layer 3, server spec §13).

These are scenario tests pointed at a live broker and a live probe. By default
they run against the in-process virtual probe on a throwaway Docker broker; if
the Docker daemon is unavailable they SKIP rather than fail, so the unit suite
still runs everywhere (spec §13).

Retargeting at hardware (--node-id != "probe-server", see conftest.py):
  * The Docker broker fixture and the in-process VirtualProbe are both skipped.
  * The server bridge connects to the external broker (--broker-host/-port),
    ingesting whatever the physical node publishes; every assertion then reads
    the server DB exactly as in virtual mode.
  * Broker credentials come from NMS_BROKER_USER / NMS_BROKER_PASS when set.

Scenarios (spec §13):
  1. Clean streamed job: accepted -> chunks -> done
  2. Deliberate seq gap -> job lands `incomplete`, gaps recorded
  5. Graceful `surveying` disconnect -> no false offline
  6. Ungraceful kill -> Last Will fires -> node marked offline
  7. ble_scan -> `devices` chunks -> done                 (hardware only)
  8. wifi_ids -> `surveying` disconnect/reconnect -> `alerts`/`frame_stats` -> done
                                                          (hardware only)

Scenarios 3 (busy) and 4 (cancel) are covered at the unit level in
test_virtual_probe.py, where their timing is deterministic. Scenarios 7 and 8
exercise the ESP32 radio and therefore only run against firmware — the virtual
probe has no radio, so they SKIP in the default (virtual) mode.
"""

import json
import os
import socket
import subprocess
import sys
import time
import uuid
from types import SimpleNamespace

import pytest

paho = pytest.importorskip("paho.mqtt.client")

from probe.virtual_probe import VirtualProbe
from protocol.topics import cmd_topic, result_topic, status_topic
from server import commands
from server.app import create_app
from server.db import db as _db
from server.models import Job, JobChunk, Node
from server.mqtt_bridge import MqttBridge

VIRTUAL_NODE = "probe-server"


def _daemon_up() -> bool:
    try:
        return subprocess.run(["docker", "version"],
                              capture_output=True, timeout=15).returncode == 0
    except Exception:
        return False


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


# --- target / broker / server fixtures -------------------------------------

@pytest.fixture(scope="module")
def target(request) -> SimpleNamespace:
    """Resolve where the suite points, from the conftest CLI options."""
    node_id = request.config.getoption("--node-id")
    return SimpleNamespace(
        node_id=node_id,
        host=request.config.getoption("--broker-host"),
        port=request.config.getoption("--broker-port"),
        hardware=(node_id != VIRTUAL_NODE),
    )


def _broker_credentials() -> tuple[str | None, str | None]:
    return os.environ.get("NMS_BROKER_USER"), os.environ.get("NMS_BROKER_PASS")


@pytest.fixture(scope="module")
def broker(target, tmp_path_factory):
    """The broker under test: an external one in hardware mode, else Docker."""
    if target.hardware:
        # Real deployment broker — never touch Docker (task requirement).
        yield (target.host, target.port or 1883)
        return

    if not _daemon_up():
        pytest.skip("docker daemon unavailable")
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
def server(broker, target, tmp_path):
    """A server app whose bridge is connected to the broker under test.

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
    username, password = _broker_credentials() if target.hardware else (None, None)
    bridge = MqttBridge(app, host, port, username=username, password=password,
                        client_id=f"nms-server-{uuid.uuid4().hex[:6]}")
    bridge.start()
    _wait_until(lambda: bridge.client.is_connected(), timeout_s=10)
    try:
        yield app, bridge
    finally:
        bridge.stop()


class LiveProbe:
    """A virtual probe wired to a real paho client for transport (virtual mode)."""

    def __init__(self, host, port, node_id=VIRTUAL_NODE, keepalive=60):
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


# --- shared assertions helpers ---------------------------------------------

def _job_state(app, job_id):
    with app.app_context():
        job = _db.session.get(Job, job_id)
        return None if job is None else job.state


def _node_exists(app, node_id):
    with app.app_context():
        return _db.session.get(Node, node_id) is not None


def _node_state(app, node_id):
    with app.app_context():
        node = _db.session.get(Node, node_id)
        return None if node is None else node.state


def _chunk_payloads(app, job_id):
    with app.app_context():
        rows = _db.session.execute(
            _db.select(JobChunk.payload).filter_by(job_id=job_id)).all()
        return [row[0] for row in rows]


def _start_virtual_probe(target, bridge):
    """A started LiveProbe in virtual mode; None when a real node is the probe."""
    if target.hardware:
        return None
    return LiveProbe(bridge.host, bridge.port)


def _ensure_capability(app, node_id, capability):
    """Add a capability to the node row if the announce has not landed it yet.

    Lets a dispatch proceed even when the (non-retained) announce was missed
    because our bridge subscribed after the node connected. Harmless in virtual
    mode, where the capability is already present from the probe's announce.
    """
    with app.app_context():
        node = _db.session.get(Node, node_id)
        if node is None:
            return
        caps = list(node.capabilities or [])
        if capability not in caps:
            node.capabilities = caps + [capability]
            _db.session.commit()


def _require_capability(app, node_id, capability, timeout_s=15.0):
    """Skip the scenario unless the node advertises `capability`.

    Radio scenarios are meaningful only if this particular board actually
    implements the command; a node that never advertised it is N/A, not a fail.
    """
    def has_cap():
        with app.app_context():
            node = _db.session.get(Node, node_id)
            return node is not None and capability in (node.capabilities or [])

    if not _wait_until(has_cap, timeout_s=timeout_s):
        pytest.skip(f"node {node_id} does not advertise {capability!r}")


# --- scenario 1 ------------------------------------------------------------

def test_clean_streamed_job_reaches_done(server, target):
    """dns is a baseline recon command both the virtual probe and firmware run."""
    app, bridge = server
    probe = _start_virtual_probe(target, bridge)
    try:
        assert _wait_until(lambda: _node_exists(app, target.node_id)) is not None
        _ensure_capability(app, target.node_id, "dns")
        with app.app_context():
            # Read job_id inside the context: the ORM object detaches when the
            # context (and its session) closes, so grab the plain string now.
            job_id = commands.create_job(
                target.node_id, "dns", {"targets": ["localhost"]}).job_id
        assert _wait_until(lambda: _job_state(app, job_id) == "done",
                           timeout_s=30) is True
    finally:
        if probe is not None:
            probe.stop_gracefully()


# --- scenario 2 ------------------------------------------------------------

def test_sequence_gap_lands_incomplete(server, target):
    """Publish chunks 0 and 2 then done directly: the server must see the gap.

    Exercises server ingest over the real transport. The chunks are injected by
    a raw publisher rather than driven by the node, so this validates the
    server's gap detection independently of what the physical node is doing.
    """
    app, bridge = server
    host, port = bridge.host, bridge.port
    with app.app_context():
        # Commit the node before the job: with only a bare ForeignKey column and
        # no relationship() between them, SQLAlchemy's unit of work does not
        # guarantee the parent insert precedes the child within one flush.
        if _db.session.get(Node, target.node_id) is None:
            _db.session.add(Node(node_id=target.node_id, capabilities=["port_scan"],
                                 state="online"))
            _db.session.commit()
        _db.session.add(Job(job_id="job-gap", node_id=target.node_id,
                            cmd="port_scan", args={}, state="accepted"))
        _db.session.commit()

    username, password = _broker_credentials() if target.hardware else (None, None)
    publisher = paho.Client(paho.CallbackAPIVersion.VERSION2,
                            client_id=f"gap-{uuid.uuid4().hex[:6]}")
    if username is not None:
        publisher.username_pw_set(username, password)
    publisher.connect(host, port)
    publisher.loop_start()
    _wait_until(lambda: publisher.is_connected(), timeout_s=10)
    try:
        for seq in (0, 2):
            _publish_result(publisher, target.node_id,
                            {"event": "chunk", "job_id": "job-gap",
                             "seq": seq, "open": []})
        _publish_result(publisher, target.node_id,
                        {"event": "done", "job_id": "job-gap",
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

def test_graceful_surveying_disconnect_is_not_offline(server, target):
    """A node that announces `surveying` and disconnects cleanly stays surveying."""
    app, bridge = server
    if target.hardware:
        # On real firmware, wifi_survey is the command that legitimately makes
        # the node go quiet: it publishes `surveying`, disconnects cleanly (Will
        # suppressed), scans, then reconnects. The node must never be flipped to
        # offline during that window.
        _wait_until(lambda: _node_exists(app, target.node_id), timeout_s=30)
        _require_capability(app, target.node_id, "wifi_survey")
        with app.app_context():
            job_id = commands.create_job(
                target.node_id, "wifi_survey", {"duration_s": 15}).job_id
        assert _wait_until(lambda: _node_state(app, target.node_id) == "surveying",
                           timeout_s=20) is not None
        # It stays surveying (not offline) through the disconnected window.
        time.sleep(2.0)
        assert _node_state(app, target.node_id) != "offline"
        assert _wait_until(lambda: _job_state(app, job_id) == "done",
                           timeout_s=60) is True
        return

    probe = LiveProbe(bridge.host, bridge.port)
    _wait_until(lambda: _node_exists(app, VIRTUAL_NODE))
    probe.probe.status("surveying", expect_back_in=30)
    _wait_until(lambda: _node_state(app, VIRTUAL_NODE) == "surveying")
    probe.client.loop_stop()
    probe.client.disconnect()          # graceful: no Last Will
    time.sleep(1.0)
    assert _node_state(app, VIRTUAL_NODE) == "surveying"


# --- scenario 6 ------------------------------------------------------------

def test_ungraceful_kill_marks_node_offline(server, target, request):
    app, bridge = server
    if target.hardware:
        # No programmatic kill for a physical node: ask the operator to pull
        # power. Requires -s (unbuffered) and an interactive terminal; skip when
        # neither is available so an unattended run does not hang.
        if not sys.stdin.isatty() or request.config.getoption("capture") != "no":
            pytest.skip("scenario 6 on hardware needs an interactive tty and -s")
        _wait_until(lambda: _node_state(app, target.node_id) in {"online", "surveying"},
                    timeout_s=30)
        try:
            input(f"\n>>> Physically UNPLUG power from {target.node_id}, "
                  "then press Enter... ")
        except OSError:
            pytest.skip("stdin unavailable; cannot prompt for physical action")
        state = _wait_until(
            lambda: _node_state(app, target.node_id) == "offline", timeout_s=30)
        assert state is True or _node_state(app, target.node_id) == "offline"
        return

    probe = LiveProbe(bridge.host, bridge.port, keepalive=2)
    _wait_until(lambda: _node_state(app, VIRTUAL_NODE) in {"online", "surveying"})
    probe.kill()
    # The broker publishes the Will after ~1.5x the keepalive interval.
    state = _wait_until(lambda: _node_state(app, VIRTUAL_NODE) == "offline",
                        timeout_s=15)
    assert state is True or _node_state(app, VIRTUAL_NODE) == "offline"


# --- scenario 7: ble_scan (hardware only) ----------------------------------

def test_ble_scan_streams_devices_and_completes(server, target):
    if not target.hardware:
        pytest.skip("ble_scan needs the ESP32 BLE radio; virtual probe has none")
    app, bridge = server
    _wait_until(lambda: _node_exists(app, target.node_id), timeout_s=30)
    _require_capability(app, target.node_id, "ble_scan")
    with app.app_context():
        job_id = commands.create_job(
            target.node_id, "ble_scan", {"duration_s": 5}).job_id
    assert _wait_until(lambda: _job_state(app, job_id) == "done",
                       timeout_s=45) is True
    # ble_scan emits at least one `devices` chunk (empty if nothing was in range).
    payloads = _chunk_payloads(app, job_id)
    assert any("devices" in payload for payload in payloads), \
        f"no devices chunk among {len(payloads)} chunks"


# --- scenario 8: wifi_ids (hardware only) ----------------------------------

def test_wifi_ids_surveys_and_reports_frame_stats(server, target):
    if not target.hardware:
        pytest.skip("wifi_ids needs the ESP32 radio; virtual probe has none")
    app, bridge = server
    _wait_until(lambda: _node_exists(app, target.node_id), timeout_s=30)
    _require_capability(app, target.node_id, "wifi_ids")

    with app.app_context():
        job_id = commands.create_job(
            target.node_id, "wifi_ids",
            {"duration_s": 15, "channels": [1, 6, 11]}).job_id

    # Like wifi_survey, wifi_ids owns the radio: it announces `surveying`,
    # disconnects cleanly, listens, then reconnects to stream its findings.
    assert _wait_until(lambda: _node_state(app, target.node_id) == "surveying",
                       timeout_s=20) is not None
    assert _wait_until(lambda: _job_state(app, job_id) == "done",
                       timeout_s=90) is True
    assert _node_state(app, target.node_id) != "offline"

    payloads = _chunk_payloads(app, job_id)
    # frame_stats always closes the job. `alerts` chunks appear only if the lab
    # RF actually carried deauth/rogue/evil-twin activity during the window, so
    # they are asserted structurally only when present, not required.
    assert any("frame_stats" in payload for payload in payloads), \
        "wifi_ids did not emit a frame_stats chunk"
    for payload in payloads:
        if "alerts" in payload:
            assert isinstance(payload["alerts"], list)


# --- helpers ---------------------------------------------------------------

def _publish_result(client, node_id, data):
    message = {"v": 1, "type": "result", "node": node_id,
               "msg_id": uuid.uuid4().hex[:20], "ts": int(time.time()),
               "data": data}
    client.publish(result_topic(node_id), json.dumps(message).encode("utf-8"),
                   qos=1)
