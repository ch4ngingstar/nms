# Server Plan A — Storage and Ingest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the server's storage layer and message-ingest handlers — the nine tables, the SSE event bus, the five protocol message handlers, and the maintenance thread's timeout/rollup/prune logic.

**Architecture:** `server/ingest.py` holds handlers that take an already-validated message dict and write rows. They are deliberately free of MQTT and threading, so every semantic in the protocol is testable by calling a function. The MQTT client that feeds them arrives in Plan C.

**Tech Stack:** Python 3.12.4, Flask 3.1.3, Flask-SQLAlchemy 3.1.1, SQLAlchemy 2.0.52, pytest 9.0.2. No broker, no Docker, no network required by any task here.

**Spec:** `docs/superpowers/specs/2026-08-16-server-c2-design.md`

**Follows:** Plan B (auth, REST API, SSE endpoint, app factory). Plan C (Docker, Mosquitto, MQTT bridge, virtual probe, conformance suite).

---

### Task 1: Package scaffolding and the SQLAlchemy instance

**Files:**
- Create: `server/__init__.py`, `server/db.py`
- Test: `tests/test_db.py`

- [ ] **Step 1: Write the failing test**

`tests/test_db.py`:

```python
import sqlite3

from flask import Flask

from server.db import db


def test_sqlite_foreign_keys_are_enforced():
    """SQLite ignores foreign keys unless asked, and retention relies on them."""
    app = Flask(__name__)
    app.config["SQLALCHEMY_DATABASE_URI"] = "sqlite:///:memory:"
    db.init_app(app)
    with app.app_context():
        raw = db.session.connection().connection.driver_connection
        assert isinstance(raw, sqlite3.Connection)
        enabled = raw.execute("PRAGMA foreign_keys").fetchone()[0]
        assert enabled == 1, "foreign key enforcement is off"
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python -m pytest tests/test_db.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'server.db'`

- [ ] **Step 3: Write the implementation**

`server/__init__.py`:

```python
"""Command-and-control server for the probe fleet.

See docs/superpowers/specs/2026-08-16-server-c2-design.md
"""
```

`server/db.py`:

```python
"""The SQLAlchemy instance, kept free of model definitions to avoid cycles."""

import sqlite3

from flask_sqlalchemy import SQLAlchemy
from sqlalchemy import event
from sqlalchemy.engine import Engine

db = SQLAlchemy()


@event.listens_for(Engine, "connect")
def _enable_sqlite_foreign_keys(dbapi_connection, connection_record):
    """Turn on foreign key enforcement for SQLite connections.

    SQLite ignores foreign keys by default. Retention prunes monitor_cycles
    with a bulk DELETE, which bypasses the ORM's cascade entirely, so without
    this pragma the child monitor_results rows would be orphaned rather than
    deleted. Guarded by an isinstance check so a non-SQLite engine is untouched.
    """
    if not isinstance(dbapi_connection, sqlite3.Connection):
        return
    cursor = dbapi_connection.cursor()
    try:
        cursor.execute("PRAGMA foreign_keys=ON")
    finally:
        cursor.close()
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `python -m pytest tests/test_db.py -v`
Expected: PASS — 1 passed

- [ ] **Step 5: Commit**

```bash
git add server/ tests/test_db.py
git commit -m "feat(server): SQLAlchemy instance with SQLite foreign key enforcement"
```

---

### Task 2: The nine tables

**Files:**
- Create: `server/models.py`
- Test: `tests/conftest.py`, `tests/test_models.py`

- [ ] **Step 1: Write the shared test fixture**

`tests/conftest.py`:

```python
import pytest
from flask import Flask

from server.db import db as _db


@pytest.fixture
def app():
    application = Flask(__name__)
    application.config["SQLALCHEMY_DATABASE_URI"] = "sqlite:///:memory:"
    application.config["TESTING"] = True
    _db.init_app(application)
    with application.app_context():
        _db.create_all()
        yield application
        _db.session.remove()
        _db.drop_all()


@pytest.fixture
def db(app):
    return _db


@pytest.fixture
def node(db):
    """A registered node, since almost everything is foreign-keyed to one."""
    from server.models import Node

    record = Node(node_id="probe-a4c1f8", label="Lab North", fw="1.2.0",
                  chip="esp32s3", mac="a0:b7:65:a4:c1:f8",
                  capabilities=["port_scan", "wifi_survey"], state="online")
    db.session.add(record)
    db.session.commit()
    return record


@pytest.fixture
def device(db):
    from server.models import Device

    record = Device(name="Main Router", ip="192.168.1.1", role="router")
    db.session.add(record)
    db.session.commit()
    return record
```

- [ ] **Step 2: Write the failing test**

`tests/test_models.py`:

```python
import pytest
from sqlalchemy.exc import IntegrityError

from server.models import (
    ApObservation, Device, Job, JobChunk, MonitorCycle, MonitorResult,
    MonitorRollup, Node, Telemetry,
)


def test_all_tables_created(db):
    names = set(db.metadata.tables)
    assert names == {
        "nodes", "devices", "monitor_cycles", "monitor_results",
        "monitor_rollups", "jobs", "job_chunks", "telemetry", "ap_observations",
    }


def test_node_round_trip(db, node):
    stored = db.session.get(Node, "probe-a4c1f8")
    assert stored.capabilities == ["port_scan", "wifi_survey"]
    assert stored.label == "Lab North"


def test_device_node_id_is_nullable(db, device):
    """Null node_id means every probe monitors this device (spec 6.1)."""
    assert device.node_id is None


def test_monitor_cycle_is_unique_per_node_and_timestamp(db, node):
    from datetime import datetime, timezone

    ts = datetime(2026, 8, 16, 12, 0, tzinfo=timezone.utc)
    db.session.add(MonitorCycle(node_id=node.node_id, cycle_ts=ts))
    db.session.commit()
    db.session.add(MonitorCycle(node_id=node.node_id, cycle_ts=ts))
    with pytest.raises(IntegrityError):
        db.session.commit()
    db.session.rollback()


def test_job_chunk_is_unique_per_job_and_seq(db, node):
    db.session.add(Job(job_id="job-1", node_id=node.node_id, cmd="port_scan", args={}))
    db.session.commit()
    db.session.add(JobChunk(job_id="job-1", seq=0, payload={}))
    db.session.commit()
    db.session.add(JobChunk(job_id="job-1", seq=0, payload={}))
    with pytest.raises(IntegrityError):
        db.session.commit()
    db.session.rollback()


def test_deleting_a_cycle_cascades_to_results(db, node, device):
    """Bulk pruning relies on the database, not the ORM, to remove children."""
    from datetime import datetime, timezone

    cycle = MonitorCycle(node_id=node.node_id,
                         cycle_ts=datetime(2026, 8, 16, 12, 0, tzinfo=timezone.utc))
    db.session.add(cycle)
    db.session.flush()
    db.session.add(MonitorResult(cycle_id=cycle.id, device_id=device.id,
                                 status="up", latency_ms=1.8))
    db.session.commit()
    assert db.session.query(MonitorResult).count() == 1

    db.session.query(MonitorCycle).delete()   # bulk delete, bypasses ORM cascade
    db.session.commit()
    assert db.session.query(MonitorResult).count() == 0


def test_rollup_is_unique_per_node_device_hour(db, node, device):
    from datetime import datetime, timezone

    hour = datetime(2026, 8, 16, 12, 0, tzinfo=timezone.utc)
    for _ in range(2):
        db.session.add(MonitorRollup(node_id=node.node_id, device_id=device.id,
                                     hour_ts=hour, samples=1, up_count=1))
    with pytest.raises(IntegrityError):
        db.session.commit()
    db.session.rollback()


def test_job_defaults(db, node):
    job = Job(job_id="job-2", node_id=node.node_id, cmd="dns", args={})
    db.session.add(job)
    db.session.commit()
    assert job.state == "pending"
    assert job.deadline_s == 120
    assert job.created_at is not None
    assert job.last_event_at is not None


def test_ap_observation_round_trip(db, node):
    db.session.add(Job(job_id="job-3", node_id=node.node_id, cmd="wifi_survey", args={}))
    db.session.commit()
    db.session.add(ApObservation(node_id=node.node_id, job_id="job-3",
                                 bssid="a0:b7:65:11:22:33", ssid="Home",
                                 channel=6, rssi=-61, auth="wpa2", hidden=False))
    db.session.commit()
    assert db.session.query(ApObservation).one().rssi == -61


def test_telemetry_round_trip(db, node):
    from datetime import datetime, timezone

    db.session.add(Telemetry(node_id=node.node_id,
                             ts=datetime(2026, 8, 16, 12, 0, tzinfo=timezone.utc),
                             free_heap=198320, uptime_s=84210, rssi=-58,
                             channel=6, state="online", jobs_done=412))
    db.session.commit()
    assert db.session.query(Telemetry).one().free_heap == 198320
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `python -m pytest tests/test_models.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'server.models'`

- [ ] **Step 4: Write the models**

`server/models.py`:

```python
"""Table definitions for the C2 server (spec §6.1)."""

from datetime import datetime, timezone

from server.db import db

NODE_ID_LEN = 32
STATE_LEN = 16


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


class Node(db.Model):
    __tablename__ = "nodes"

    node_id = db.Column(db.String(NODE_ID_LEN), primary_key=True)
    label = db.Column(db.String(64))
    fw = db.Column(db.String(16))
    chip = db.Column(db.String(32))
    mac = db.Column(db.String(17))
    capabilities = db.Column(db.JSON, nullable=False, default=list)
    state = db.Column(db.String(STATE_LEN), nullable=False, default="offline")
    first_seen = db.Column(db.DateTime(timezone=True), nullable=False, default=_utcnow)
    last_seen = db.Column(db.DateTime(timezone=True), nullable=False, default=_utcnow)
    last_status_ts = db.Column(db.DateTime(timezone=True))


class Device(db.Model):
    __tablename__ = "devices"

    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(120), nullable=False)
    ip = db.Column(db.String(64), nullable=False, index=True)
    role = db.Column(db.String(80), nullable=False, default="unknown")
    enabled = db.Column(db.Boolean, nullable=False, default=True)
    # Null means every probe monitors this device; set restricts it to one.
    node_id = db.Column(
        db.String(NODE_ID_LEN),
        db.ForeignKey("nodes.node_id", ondelete="SET NULL"),
        nullable=True,
    )


class MonitorCycle(db.Model):
    __tablename__ = "monitor_cycles"

    id = db.Column(db.Integer, primary_key=True)
    node_id = db.Column(
        db.String(NODE_ID_LEN),
        db.ForeignKey("nodes.node_id", ondelete="CASCADE"),
        nullable=False, index=True,
    )
    cycle_ts = db.Column(db.DateTime(timezone=True), nullable=False, index=True)
    received_at = db.Column(db.DateTime(timezone=True), nullable=False, default=_utcnow)

    __table_args__ = (
        db.UniqueConstraint("node_id", "cycle_ts", name="uq_cycle_node_ts"),
    )


class MonitorResult(db.Model):
    __tablename__ = "monitor_results"

    id = db.Column(db.Integer, primary_key=True)
    cycle_id = db.Column(
        db.Integer,
        db.ForeignKey("monitor_cycles.id", ondelete="CASCADE"),
        nullable=False, index=True,
    )
    device_id = db.Column(
        db.Integer,
        db.ForeignKey("devices.id", ondelete="CASCADE"),
        nullable=False, index=True,
    )
    status = db.Column(db.String(8), nullable=False)
    latency_ms = db.Column(db.Float)
    ports = db.Column(db.JSON)


class MonitorRollup(db.Model):
    __tablename__ = "monitor_rollups"

    id = db.Column(db.Integer, primary_key=True)
    node_id = db.Column(db.String(NODE_ID_LEN), nullable=False, index=True)
    device_id = db.Column(db.Integer, nullable=False, index=True)
    hour_ts = db.Column(db.DateTime(timezone=True), nullable=False, index=True)
    samples = db.Column(db.Integer, nullable=False)
    up_count = db.Column(db.Integer, nullable=False)
    latency_min = db.Column(db.Float)
    latency_avg = db.Column(db.Float)
    latency_max = db.Column(db.Float)

    __table_args__ = (
        db.UniqueConstraint("node_id", "device_id", "hour_ts", name="uq_rollup"),
    )


class Job(db.Model):
    __tablename__ = "jobs"

    job_id = db.Column(db.String(NODE_ID_LEN), primary_key=True)
    node_id = db.Column(
        db.String(NODE_ID_LEN),
        db.ForeignKey("nodes.node_id", ondelete="CASCADE"),
        nullable=False, index=True,
    )
    cmd = db.Column(db.String(24), nullable=False)
    args = db.Column(db.JSON, nullable=False, default=dict)
    state = db.Column(db.String(STATE_LEN), nullable=False, default="pending", index=True)
    created_at = db.Column(db.DateTime(timezone=True), nullable=False, default=_utcnow)
    # The deadline slides from the last event, not from creation (spec §8).
    last_event_at = db.Column(db.DateTime(timezone=True), nullable=False, default=_utcnow)
    deadline_s = db.Column(db.Integer, nullable=False, default=120)
    accepted_at = db.Column(db.DateTime(timezone=True))
    finished_at = db.Column(db.DateTime(timezone=True))
    chunks = db.Column(db.Integer)
    results = db.Column(db.Integer)
    duration_ms = db.Column(db.Integer)
    error_code = db.Column(db.String(24))
    error_message = db.Column(db.Text)
    gaps = db.Column(db.JSON)


class JobChunk(db.Model):
    __tablename__ = "job_chunks"

    id = db.Column(db.Integer, primary_key=True)
    job_id = db.Column(
        db.String(NODE_ID_LEN),
        db.ForeignKey("jobs.job_id", ondelete="CASCADE"),
        nullable=False, index=True,
    )
    seq = db.Column(db.Integer, nullable=False)
    payload = db.Column(db.JSON, nullable=False)
    received_at = db.Column(db.DateTime(timezone=True), nullable=False, default=_utcnow)

    __table_args__ = (
        db.UniqueConstraint("job_id", "seq", name="uq_chunk_job_seq"),
    )


class Telemetry(db.Model):
    __tablename__ = "telemetry"

    id = db.Column(db.Integer, primary_key=True)
    node_id = db.Column(
        db.String(NODE_ID_LEN),
        db.ForeignKey("nodes.node_id", ondelete="CASCADE"),
        nullable=False, index=True,
    )
    ts = db.Column(db.DateTime(timezone=True), nullable=False, index=True)
    free_heap = db.Column(db.Integer, nullable=False)
    uptime_s = db.Column(db.Integer, nullable=False)
    rssi = db.Column(db.Integer)
    channel = db.Column(db.Integer)
    state = db.Column(db.String(STATE_LEN), nullable=False)
    jobs_done = db.Column(db.Integer)


class ApObservation(db.Model):
    __tablename__ = "ap_observations"

    id = db.Column(db.Integer, primary_key=True)
    node_id = db.Column(
        db.String(NODE_ID_LEN),
        db.ForeignKey("nodes.node_id", ondelete="CASCADE"),
        nullable=False, index=True,
    )
    job_id = db.Column(
        db.String(NODE_ID_LEN),
        db.ForeignKey("jobs.job_id", ondelete="CASCADE"),
        nullable=False, index=True,
    )
    bssid = db.Column(db.String(17), nullable=False)
    ssid = db.Column(db.String(64))
    channel = db.Column(db.Integer)
    rssi = db.Column(db.Integer)
    auth = db.Column(db.String(24))
    hidden = db.Column(db.Boolean, default=False)
    observed_at = db.Column(db.DateTime(timezone=True), nullable=False,
                            default=_utcnow, index=True)

    __table_args__ = (
        db.Index("ix_ap_bssid_time", "bssid", "observed_at"),
    )
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `python -m pytest tests/test_models.py -v`
Expected: PASS — 10 passed

- [ ] **Step 6: Commit**

```bash
git add server/models.py tests/conftest.py tests/test_models.py
git commit -m "feat(server): nine-table schema for nodes, monitoring, jobs and RF"
```

---

### Task 3: The SSE event bus

**Files:**
- Create: `server/events.py`
- Test: `tests/test_events.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_events.py`:

```python
import queue

import pytest

from server.events import MAX_QUEUE, EventBus


def test_subscriber_receives_published_event():
    bus = EventBus()
    q = bus.subscribe()
    bus.publish("node_status", {"node": "probe-a4c1f8"})
    assert q.get_nowait() == {"type": "node_status",
                              "data": {"node": "probe-a4c1f8"}}


def test_every_subscriber_receives_the_event():
    bus = EventBus()
    first, second = bus.subscribe(), bus.subscribe()
    bus.publish("telemetry", {"free_heap": 1})
    assert first.get_nowait()["type"] == "telemetry"
    assert second.get_nowait()["type"] == "telemetry"


def test_unsubscribe_stops_delivery():
    bus = EventBus()
    q = bus.subscribe()
    bus.unsubscribe(q)
    bus.publish("telemetry", {})
    with pytest.raises(queue.Empty):
        q.get_nowait()


def test_publish_with_no_subscribers_is_harmless():
    EventBus().publish("telemetry", {})


def test_slow_subscriber_is_dropped_when_queue_fills():
    """A browser on a suspended laptop must not grow the server's memory."""
    bus = EventBus(maxsize=3)
    q = bus.subscribe()
    for _ in range(3):
        bus.publish("telemetry", {})
    assert bus.subscriber_count == 1
    bus.publish("telemetry", {})          # overflows
    assert bus.subscriber_count == 0


def test_default_queue_bound():
    assert MAX_QUEUE == 500
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_events.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'server.events'`

- [ ] **Step 3: Write the implementation**

`server/events.py`:

```python
"""In-memory pub/sub feeding SSE subscribers (spec §7.3).

The bus is process-local, which is why the server must run as a single
process; see spec §4.
"""

import queue
import threading

MAX_QUEUE = 500


class EventBus:
    """Fan-out to SSE clients that never blocks on a slow consumer."""

    def __init__(self, maxsize: int = MAX_QUEUE):
        self._maxsize = maxsize
        self._subscribers: set[queue.Queue] = set()
        self._lock = threading.Lock()

    def subscribe(self) -> queue.Queue:
        subscriber = queue.Queue(maxsize=self._maxsize)
        with self._lock:
            self._subscribers.add(subscriber)
        return subscriber

    def unsubscribe(self, subscriber: queue.Queue) -> None:
        with self._lock:
            self._subscribers.discard(subscriber)

    @property
    def subscriber_count(self) -> int:
        with self._lock:
            return len(self._subscribers)

    def publish(self, event_type: str, payload: dict) -> None:
        """Deliver to every subscriber, dropping any whose queue is full."""
        message = {"type": event_type, "data": payload}
        with self._lock:
            targets = list(self._subscribers)
        for subscriber in targets:
            try:
                subscriber.put_nowait(message)
            except queue.Full:
                self.unsubscribe(subscriber)


bus = EventBus()
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python -m pytest tests/test_events.py -v`
Expected: PASS — 6 passed

- [ ] **Step 5: Commit**

```bash
git add server/events.py tests/test_events.py
git commit -m "feat(server): bounded in-memory event bus for SSE fan-out"
```

---

### Task 4: Ingest — announce and status

**Files:**
- Create: `server/ingest.py`
- Test: `tests/test_ingest_lifecycle.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_ingest_lifecycle.py`:

```python
from server.ingest import handle_announce, handle_status
from server.models import Node

TS = 1755302400


def envelope(msg_type, data, node="probe-a4c1f8", ts=TS):
    return {"v": 1, "type": msg_type, "node": node,
            "msg_id": "01J8X2K9QWER", "ts": ts, "data": data}


ANNOUNCE = {"label": "Lab North", "fw": "1.2.0", "chip": "esp32s3",
            "mac": "a0:b7:65:a4:c1:f8", "free_heap": 214512,
            "capabilities": ["port_scan", "wifi_survey"]}


def test_announce_creates_an_unknown_node(db):
    handle_announce(envelope("announce", ANNOUNCE))
    stored = db.session.get(Node, "probe-a4c1f8")
    assert stored.fw == "1.2.0"
    assert stored.capabilities == ["port_scan", "wifi_survey"]
    assert stored.label == "Lab North"


def test_announce_updates_an_existing_node(db, node):
    original_first_seen = node.first_seen
    updated = dict(ANNOUNCE, fw="1.3.0", capabilities=["port_scan"])
    handle_announce(envelope("announce", updated))
    stored = db.session.get(Node, "probe-a4c1f8")
    assert stored.fw == "1.3.0"
    assert stored.capabilities == ["port_scan"]
    assert stored.first_seen == original_first_seen, "first_seen must not move"


def test_announce_without_label_keeps_the_existing_one(db, node):
    data = {k: v for k, v in ANNOUNCE.items() if k != "label"}
    handle_announce(envelope("announce", data))
    assert db.session.get(Node, "probe-a4c1f8").label == "Lab North"


def test_status_updates_state(db, node):
    handle_status(envelope("status", {"state": "busy", "job": "job-1"}))
    assert db.session.get(Node, "probe-a4c1f8").state == "busy"


def test_offline_status_does_not_advance_last_seen(db, node):
    before = node.last_seen
    handle_status(envelope("status", {"state": "offline", "reason": "lwt"},
                           ts=TS + 3600))
    stored = db.session.get(Node, "probe-a4c1f8")
    assert stored.state == "offline"
    assert stored.last_seen == before, "a death notice is not a sign of life"


def test_surveying_status_advances_last_seen(db, node):
    handle_status(envelope("status", {"state": "surveying", "expect_back_in": 30},
                           ts=TS + 60))
    stored = db.session.get(Node, "probe-a4c1f8")
    assert stored.state == "surveying"
    assert stored.last_seen > node.first_seen


def test_status_from_unknown_node_creates_it(db):
    handle_status(envelope("status", {"state": "online"}, node="probe-7e2b10"))
    assert db.session.get(Node, "probe-7e2b10").state == "online"


def test_announce_publishes_an_event(db, monkeypatch):
    seen = []
    from server import ingest
    monkeypatch.setattr(ingest.bus, "publish",
                        lambda kind, payload: seen.append((kind, payload)))
    handle_announce(envelope("announce", ANNOUNCE))
    assert seen[0][0] == "node_status"
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_ingest_lifecycle.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'server.ingest'`

- [ ] **Step 3: Write the implementation**

`server/ingest.py`:

```python
"""Handlers for validated protocol messages (spec §7.2).

Deliberately free of MQTT and threading: each handler takes an already
validated message dict and writes rows, so protocol semantics are testable
by calling a function. The client that feeds them lives in mqtt_bridge.
"""

from datetime import datetime, timezone

from server.db import db
from server.events import bus
from server.models import Node


def to_datetime(epoch_seconds: int) -> datetime:
    """Protocol timestamps are Unix seconds, UTC (protocol spec §5.2)."""
    return datetime.fromtimestamp(epoch_seconds, tz=timezone.utc)


def get_or_create_node(node_id: str, seen_at: datetime) -> Node:
    """Return the node, creating a minimal row if this is the first sighting.

    The broker ACL already established that this is an authorised node, so
    refusing to record it here would add nothing but lost data.
    """
    node = db.session.get(Node, node_id)
    if node is None:
        node = Node(node_id=node_id, capabilities=[],
                    first_seen=seen_at, last_seen=seen_at)
        db.session.add(node)
    return node


def handle_announce(message: dict) -> Node:
    data = message["data"]
    seen_at = to_datetime(message["ts"])
    node = get_or_create_node(message["node"], seen_at)

    if "label" in data:
        node.label = data["label"]
    node.fw = data["fw"]
    node.chip = data["chip"]
    node.mac = data["mac"]
    node.capabilities = data["capabilities"]
    node.last_seen = seen_at
    db.session.commit()

    bus.publish("node_status", {"node": node.node_id, "state": node.state,
                                "label": node.label,
                                "capabilities": node.capabilities})
    return node


def handle_status(message: dict) -> Node:
    data = message["data"]
    seen_at = to_datetime(message["ts"])
    node = get_or_create_node(message["node"], seen_at)

    node.state = data["state"]
    node.last_status_ts = seen_at
    if data["state"] != "offline":
        # An offline notice is usually the broker's Last Will, published on
        # the node's behalf after it stopped talking. It is not a sign of life.
        node.last_seen = seen_at
    db.session.commit()

    bus.publish("node_status", {"node": node.node_id, "state": node.state,
                                "expect_back_in": data.get("expect_back_in")})
    return node
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python -m pytest tests/test_ingest_lifecycle.py -v`
Expected: PASS — 8 passed

- [ ] **Step 5: Commit**

```bash
git add server/ingest.py tests/test_ingest_lifecycle.py
git commit -m "feat(server): ingest handlers for announce and status"
```

---

### Task 5: Ingest — telemetry and monitor cycles

**Files:**
- Modify: `server/ingest.py`
- Test: `tests/test_ingest_monitor.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_ingest_monitor.py`:

```python
from server.ingest import handle_monitor, handle_telemetry
from server.models import MonitorCycle, MonitorResult, Node, Telemetry

TS = 1755302400


def envelope(msg_type, data, node="probe-a4c1f8"):
    return {"v": 1, "type": msg_type, "node": node,
            "msg_id": "01J8X2KC9012", "ts": TS, "data": data}


def test_telemetry_is_stored(db, node):
    handle_telemetry(envelope("telemetry", {
        "free_heap": 198320, "uptime_s": 84210, "rssi": -58,
        "channel": 6, "state": "online", "jobs_done": 412}))
    sample = db.session.query(Telemetry).one()
    assert sample.free_heap == 198320
    assert sample.rssi == -58


def test_telemetry_advances_last_seen(db, node):
    handle_telemetry(envelope("telemetry", {"free_heap": 1, "uptime_s": 1,
                                            "state": "online"}))
    assert db.session.get(Node, "probe-a4c1f8").last_seen is not None


def test_monitor_cycle_and_results_are_stored(db, node, device):
    handle_monitor(envelope("monitor", {
        "cycle_ts": TS,
        "results": [{"id": device.id, "status": "up", "latency_ms": 1.8,
                     "ports": {"22": "open", "80": "closed"}}]}))
    cycle = db.session.query(MonitorCycle).one()
    assert cycle.node_id == "probe-a4c1f8"
    result = db.session.query(MonitorResult).one()
    assert result.status == "up"
    assert result.latency_ms == 1.8
    assert result.ports == {"22": "open", "80": "closed"}


def test_redelivered_cycle_is_ignored(db, node, device):
    """QoS 1 permits redelivery; the unique constraint must not blow up."""
    message = envelope("monitor", {
        "cycle_ts": TS,
        "results": [{"id": device.id, "status": "up", "latency_ms": 1.8}]})
    handle_monitor(message)
    handle_monitor(message)
    assert db.session.query(MonitorCycle).count() == 1
    assert db.session.query(MonitorResult).count() == 1


def test_results_for_unknown_devices_are_skipped(db, node, device):
    """A probe may still hold a device the operator has since deleted."""
    handle_monitor(envelope("monitor", {
        "cycle_ts": TS,
        "results": [{"id": device.id, "status": "up", "latency_ms": 1.0},
                    {"id": 9999, "status": "down", "latency_ms": None}]}))
    assert db.session.query(MonitorResult).count() == 1


def test_down_host_stores_null_latency(db, node, device):
    handle_monitor(envelope("monitor", {
        "cycle_ts": TS,
        "results": [{"id": device.id, "status": "down", "latency_ms": None}]}))
    assert db.session.query(MonitorResult).one().latency_ms is None


def test_monitor_publishes_an_event(db, node, device, monkeypatch):
    seen = []
    from server import ingest
    monkeypatch.setattr(ingest.bus, "publish",
                        lambda kind, payload: seen.append((kind, payload)))
    handle_monitor(envelope("monitor", {
        "cycle_ts": TS,
        "results": [{"id": device.id, "status": "up", "latency_ms": 1.0}]}))
    assert seen[0][0] == "monitor_cycle"
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_ingest_monitor.py -v`
Expected: FAIL — `ImportError: cannot import name 'handle_monitor'`

- [ ] **Step 3: Extend the implementation**

Add to the imports at the top of `server/ingest.py`:

```python
from server.models import Device, MonitorCycle, MonitorResult, Node, Telemetry
```

(replacing the existing `from server.models import Node` line)

Append to `server/ingest.py`:

```python
def handle_telemetry(message: dict) -> Telemetry:
    data = message["data"]
    seen_at = to_datetime(message["ts"])
    node = get_or_create_node(message["node"], seen_at)
    node.last_seen = seen_at

    sample = Telemetry(
        node_id=node.node_id, ts=seen_at,
        free_heap=data["free_heap"], uptime_s=data["uptime_s"],
        rssi=data.get("rssi"), channel=data.get("channel"),
        state=data["state"], jobs_done=data.get("jobs_done"),
    )
    db.session.add(sample)
    db.session.commit()

    bus.publish("telemetry", {"node": node.node_id, "free_heap": data["free_heap"],
                              "uptime_s": data["uptime_s"], "rssi": data.get("rssi")})
    return sample


def handle_monitor(message: dict) -> MonitorCycle:
    data = message["data"]
    cycle_ts = to_datetime(data["cycle_ts"])
    node = get_or_create_node(message["node"], cycle_ts)

    existing = db.session.execute(
        db.select(MonitorCycle).filter_by(node_id=node.node_id, cycle_ts=cycle_ts)
    ).scalar_one_or_none()
    if existing is not None:
        return existing          # QoS 1 redelivery; already recorded

    cycle = MonitorCycle(node_id=node.node_id, cycle_ts=cycle_ts)
    db.session.add(cycle)
    db.session.flush()

    known_device_ids = {
        row[0] for row in db.session.execute(db.select(Device.id)).all()
    }
    stored = 0
    for row in data["results"]:
        # A probe can still be monitoring a device the operator deleted.
        if row["id"] not in known_device_ids:
            continue
        db.session.add(MonitorResult(
            cycle_id=cycle.id, device_id=row["id"], status=row["status"],
            latency_ms=row.get("latency_ms"), ports=row.get("ports"),
        ))
        stored += 1

    node.last_seen = cycle_ts
    db.session.commit()

    bus.publish("monitor_cycle", {"node": node.node_id,
                                  "cycle_ts": data["cycle_ts"],
                                  "results": stored})
    return cycle
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python -m pytest tests/test_ingest_monitor.py -v`
Expected: PASS — 7 passed

- [ ] **Step 5: Commit**

```bash
git add server/ingest.py tests/test_ingest_monitor.py
git commit -m "feat(server): ingest handlers for telemetry and monitor cycles"
```

---

### Task 6: Ingest — the job result state machine

**Files:**
- Modify: `server/ingest.py`
- Test: `tests/test_ingest_result.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_ingest_result.py`:

```python
import pytest

from server.ingest import handle_result
from server.models import ApObservation, Job, JobChunk

TS = 1755302400


def result(data, ts=TS):
    return {"v": 1, "type": "result", "node": "probe-a4c1f8",
            "msg_id": "01J8X2KB0001", "ts": ts, "data": data}


@pytest.fixture
def job(db, node):
    record = Job(job_id="job-7f3a91", node_id=node.node_id,
                 cmd="port_scan", args={})
    db.session.add(record)
    db.session.commit()
    return record


def test_accepted_sets_state(db, job):
    handle_result(result({"event": "accepted", "job_id": "job-7f3a91"}))
    assert db.session.get(Job, "job-7f3a91").state == "accepted"
    assert db.session.get(Job, "job-7f3a91").accepted_at is not None


def test_chunk_is_stored(db, job):
    handle_result(result({"event": "chunk", "job_id": "job-7f3a91", "seq": 0,
                          "open": [{"host": "10.0.0.5", "port": 22}]}))
    chunk = db.session.query(JobChunk).one()
    assert chunk.seq == 0
    assert chunk.payload["open"][0]["port"] == 22


def test_duplicate_seq_is_ignored(db, job):
    payload = {"event": "chunk", "job_id": "job-7f3a91", "seq": 0, "open": []}
    handle_result(result(payload))
    handle_result(result(payload))
    assert db.session.query(JobChunk).count() == 1


def test_chunk_advances_last_event_at(db, job):
    before = job.last_event_at
    handle_result(result({"event": "chunk", "job_id": "job-7f3a91", "seq": 0,
                          "open": []}, ts=TS + 60))
    assert db.session.get(Job, "job-7f3a91").last_event_at > before


def test_contiguous_chunks_finish_done(db, job):
    for seq in range(3):
        handle_result(result({"event": "chunk", "job_id": "job-7f3a91",
                              "seq": seq, "open": []}))
    handle_result(result({"event": "done", "job_id": "job-7f3a91",
                          "chunks": 3, "results": 3, "duration_ms": 812}))
    stored = db.session.get(Job, "job-7f3a91")
    assert stored.state == "done"
    assert stored.gaps == []
    assert stored.duration_ms == 812
    assert stored.finished_at is not None


def test_sequence_gap_finishes_incomplete(db, job):
    """Spec §7.4: a truncated scan must announce itself, not look complete."""
    for seq in (0, 2):
        handle_result(result({"event": "chunk", "job_id": "job-7f3a91",
                              "seq": seq, "open": []}))
    handle_result(result({"event": "done", "job_id": "job-7f3a91",
                          "chunks": 2, "results": 2, "duration_ms": 500}))
    stored = db.session.get(Job, "job-7f3a91")
    assert stored.state == "incomplete"
    assert stored.gaps == [1]


def test_error_is_terminal(db, job):
    handle_result(result({"event": "error", "job_id": "job-7f3a91",
                          "code": "busy", "message": "a job is already running"}))
    stored = db.session.get(Job, "job-7f3a91")
    assert stored.state == "error"
    assert stored.error_code == "busy"
    assert stored.finished_at is not None


def test_result_for_unknown_job_is_dropped(db, node):
    assert handle_result(result({"event": "accepted", "job_id": "job-nope"})) is None


def test_wifi_survey_chunks_project_into_observations(db, node):
    """The multi-vantage correlation dataset (spec §6.2)."""
    db.session.add(Job(job_id="job-rf", node_id=node.node_id,
                       cmd="wifi_survey", args={}))
    db.session.commit()
    handle_result({"v": 1, "type": "result", "node": "probe-a4c1f8",
                   "msg_id": "01J8X2KB0006", "ts": TS,
                   "data": {"event": "chunk", "job_id": "job-rf", "seq": 0,
                            "aps": [{"bssid": "a0:b7:65:11:22:33", "ssid": "Home",
                                     "channel": 6, "rssi": -61, "auth": "wpa2",
                                     "hidden": False}]}})
    observation = db.session.query(ApObservation).one()
    assert observation.bssid == "a0:b7:65:11:22:33"
    assert observation.rssi == -61
    assert observation.node_id == "probe-a4c1f8"


def test_port_scan_chunks_do_not_project(db, job):
    handle_result(result({"event": "chunk", "job_id": "job-7f3a91", "seq": 0,
                          "open": [{"host": "10.0.0.5", "port": 22}]}))
    assert db.session.query(ApObservation).count() == 0
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_ingest_result.py -v`
Expected: FAIL — `ImportError: cannot import name 'handle_result'`

- [ ] **Step 3: Extend the implementation**

Add to the imports at the top of `server/ingest.py`:

```python
from protocol.job import JobState, JobTracker
from server.models import (
    ApObservation, Device, Job, JobChunk, MonitorCycle, MonitorResult,
    Node, Telemetry,
)
```

(replacing the existing `from server.models import ...` line)

Append to `server/ingest.py`:

```python
def _received_seqs(job_id: str) -> list[int]:
    return [
        row[0] for row in
        db.session.execute(db.select(JobChunk.seq).filter_by(job_id=job_id)).all()
    ]


def _gaps_for(job_id: str) -> list[int]:
    """Reuse the tested tracker rather than reimplementing gap detection."""
    tracker = JobTracker(job_id)
    tracker.accept()
    for seq in _received_seqs(job_id):
        tracker.chunk(seq, {})
    return tracker.gaps


def _project_ap_observations(job: Job, data: dict, observed_at) -> None:
    """Explode a wifi_survey chunk into the queryable observations table.

    Only RF survey gets a projection: GROUP BY bssid comparing rssi across
    node_id is the multi-vantage query that justifies a typed table (spec §6.2).
    """
    for access_point in data.get("aps", []):
        db.session.add(ApObservation(
            node_id=job.node_id, job_id=job.job_id,
            bssid=access_point["bssid"], ssid=access_point.get("ssid"),
            channel=access_point.get("channel"), rssi=access_point.get("rssi"),
            auth=access_point.get("auth"), hidden=access_point.get("hidden", False),
            observed_at=observed_at,
        ))


def _store_chunk(job: Job, data: dict, received_at) -> None:
    seq = data["seq"]
    already = db.session.execute(
        db.select(JobChunk.id).filter_by(job_id=job.job_id, seq=seq)
    ).scalar_one_or_none()
    if already is not None:
        return                     # QoS 1 redelivery
    db.session.add(JobChunk(job_id=job.job_id, seq=seq, payload=data,
                            received_at=received_at))
    if job.cmd == "wifi_survey":
        _project_ap_observations(job, data, received_at)


def handle_result(message: dict):
    """Drive one job's state machine from a result event (spec §8)."""
    data = message["data"]
    job = db.session.get(Job, data["job_id"])
    if job is None:
        return None                # result for a job this server never issued

    event = data["event"]
    at = to_datetime(message["ts"])
    job.last_event_at = at

    if event == "accepted":
        job.state = JobState.ACCEPTED.value
        job.accepted_at = at
    elif event == "chunk":
        _store_chunk(job, data, at)
    elif event == "done":
        db.session.flush()
        gaps = _gaps_for(job.job_id)
        job.gaps = gaps
        job.state = (JobState.INCOMPLETE if gaps else JobState.DONE).value
        job.chunks = data["chunks"]
        job.results = data["results"]
        job.duration_ms = data["duration_ms"]
        job.finished_at = at
    elif event == "error":
        job.state = JobState.ERROR.value
        job.error_code = data["code"]
        job.error_message = data["message"]
        job.finished_at = at

    db.session.commit()
    bus.publish("job_event", {"job_id": job.job_id, "node": job.node_id,
                              "event": event, "state": job.state})
    return job
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python -m pytest tests/test_ingest_result.py -v`
Expected: PASS — 10 passed

- [ ] **Step 5: Commit**

```bash
git add server/ingest.py tests/test_ingest_result.py
git commit -m "feat(server): job result state machine with gap detection and RF projection"
```

---

### Task 7: Maintenance — job timeout sweeper

**Files:**
- Create: `server/maintenance.py`
- Test: `tests/test_maintenance_timeouts.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_maintenance_timeouts.py`:

```python
from datetime import datetime, timedelta, timezone

from server.maintenance import DEFAULT_DEADLINE_S, SWEEP_INTERVAL_S, sweep_timeouts
from server.models import Job

NOW = datetime(2026, 8, 16, 12, 0, tzinfo=timezone.utc)


def make_job(db, node, job_id, last_event_at, state="accepted",
             cmd="port_scan", deadline_s=DEFAULT_DEADLINE_S):
    record = Job(job_id=job_id, node_id=node.node_id, cmd=cmd, args={},
                 state=state, created_at=last_event_at,
                 last_event_at=last_event_at, deadline_s=deadline_s)
    db.session.add(record)
    db.session.commit()
    return record


def test_silent_job_past_its_deadline_times_out(db, node):
    make_job(db, node, "job-1", NOW - timedelta(seconds=DEFAULT_DEADLINE_S + 1))
    assert sweep_timeouts(now=NOW) == 1
    assert db.session.get(Job, "job-1").state == "timed_out"


def test_recently_active_job_survives(db, node):
    make_job(db, node, "job-2", NOW - timedelta(seconds=10))
    assert sweep_timeouts(now=NOW) == 0
    assert db.session.get(Job, "job-2").state == "accepted"


def test_deadline_slides_from_the_last_event(db, node):
    """A long healthy stream must not be killed for taking a while."""
    job = make_job(db, node, "job-3", NOW - timedelta(hours=2))
    job.last_event_at = NOW - timedelta(seconds=5)
    db.session.commit()
    assert sweep_timeouts(now=NOW) == 0


def test_terminal_jobs_are_left_alone(db, node):
    for index, state in enumerate(("done", "incomplete", "error", "timed_out")):
        make_job(db, node, f"job-t{index}",
                 NOW - timedelta(hours=1), state=state)
    assert sweep_timeouts(now=NOW) == 0


def test_pending_job_can_time_out(db, node):
    make_job(db, node, "job-4", NOW - timedelta(seconds=DEFAULT_DEADLINE_S + 1),
             state="pending")
    assert sweep_timeouts(now=NOW) == 1


def test_wifi_survey_gets_its_extended_deadline(db, node):
    """An announced promiscuous-mode absence is not a hang (spec §8)."""
    make_job(db, node, "job-rf", NOW - timedelta(seconds=200),
             cmd="wifi_survey", deadline_s=30 + 60)
    assert sweep_timeouts(now=NOW) == 1

    make_job(db, node, "job-rf2", NOW - timedelta(seconds=60),
             cmd="wifi_survey", deadline_s=30 + 60)
    assert sweep_timeouts(now=NOW) == 0


def test_sweep_interval_is_frequent_enough_to_be_useful():
    """An hourly sweep would leave a hung job undetected for an hour."""
    assert SWEEP_INTERVAL_S <= 30
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_maintenance_timeouts.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'server.maintenance'`

- [ ] **Step 3: Write the implementation**

`server/maintenance.py`:

```python
"""Background maintenance: job timeouts, rollups, pruning (spec §6.3, §8).

One thread on a 10-second tick. Timeouts are swept every tick because an
hourly sweep would leave a hung job undetected for up to an hour; rollup and
prune run at most once per hour.
"""

from datetime import datetime, timedelta, timezone

from protocol.job import JobState
from server.db import db
from server.models import Job

SWEEP_INTERVAL_S = 10
DEFAULT_DEADLINE_S = 120
SURVEY_DEADLINE_MARGIN_S = 60

TERMINAL_STATES = {
    JobState.DONE.value,
    JobState.INCOMPLETE.value,
    JobState.ERROR.value,
    JobState.TIMED_OUT.value,
}


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def sweep_timeouts(now: datetime | None = None) -> int:
    """Mark silent non-terminal jobs as timed out. Returns how many."""
    now = now or _utcnow()
    candidates = db.session.execute(
        db.select(Job).where(Job.state.notin_(TERMINAL_STATES))
    ).scalars().all()

    timed_out = 0
    for job in candidates:
        last_event = job.last_event_at
        if last_event.tzinfo is None:
            last_event = last_event.replace(tzinfo=timezone.utc)
        if now - last_event > timedelta(seconds=job.deadline_s):
            job.state = JobState.TIMED_OUT.value
            job.finished_at = now
            timed_out += 1

    if timed_out:
        db.session.commit()
    return timed_out
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python -m pytest tests/test_maintenance_timeouts.py -v`
Expected: PASS — 7 passed

- [ ] **Step 5: Commit**

```bash
git add server/maintenance.py tests/test_maintenance_timeouts.py
git commit -m "feat(server): job timeout sweeper with sliding deadline"
```

---

### Task 8: Maintenance — hourly rollup and prune

**Files:**
- Modify: `server/maintenance.py`
- Test: `tests/test_maintenance_rollup.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_maintenance_rollup.py`:

```python
from datetime import datetime, timedelta, timezone

from server.maintenance import RAW_RETENTION_DAYS, prune_old_cycles, roll_up_hour
from server.models import MonitorCycle, MonitorResult, MonitorRollup

NOW = datetime(2026, 8, 16, 12, 0, tzinfo=timezone.utc)


def add_cycle(db, node, device, at, status="up", latency=None):
    cycle = MonitorCycle(node_id=node.node_id, cycle_ts=at)
    db.session.add(cycle)
    db.session.flush()
    db.session.add(MonitorResult(cycle_id=cycle.id, device_id=device.id,
                                 status=status, latency_ms=latency))
    db.session.commit()
    return cycle


def test_rollup_aggregates_an_hour(db, node, device):
    hour = NOW - timedelta(days=10)
    hour = hour.replace(minute=0, second=0, microsecond=0)
    for minute, latency in enumerate([10.0, 20.0, 30.0]):
        add_cycle(db, node, device, hour + timedelta(minutes=minute),
                  latency=latency)
    add_cycle(db, node, device, hour + timedelta(minutes=4), status="down")

    assert roll_up_hour(hour) == 1
    rollup = db.session.query(MonitorRollup).one()
    assert rollup.samples == 4
    assert rollup.up_count == 3
    assert rollup.latency_min == 10.0
    assert rollup.latency_max == 30.0
    assert rollup.latency_avg == 20.0


def test_rollup_is_idempotent(db, node, device):
    hour = (NOW - timedelta(days=10)).replace(minute=0, second=0, microsecond=0)
    add_cycle(db, node, device, hour, latency=10.0)
    roll_up_hour(hour)
    roll_up_hour(hour)
    assert db.session.query(MonitorRollup).count() == 1
    assert db.session.query(MonitorRollup).one().samples == 1


def test_rollup_of_an_empty_hour_writes_nothing(db, node, device):
    hour = (NOW - timedelta(days=10)).replace(minute=0, second=0, microsecond=0)
    assert roll_up_hour(hour) == 0
    assert db.session.query(MonitorRollup).count() == 0


def test_prune_removes_cycles_past_retention(db, node, device):
    old = NOW - timedelta(days=RAW_RETENTION_DAYS + 1)
    recent = NOW - timedelta(days=1)
    add_cycle(db, node, device, old, latency=1.0)
    add_cycle(db, node, device, recent, latency=2.0)

    assert prune_old_cycles(now=NOW) == 1
    remaining = db.session.query(MonitorCycle).one()
    assert remaining.cycle_ts.replace(tzinfo=timezone.utc) == recent


def test_prune_cascades_to_results(db, node, device):
    """Bulk delete relies on the SQLite foreign key pragma from Task 1."""
    add_cycle(db, node, device, NOW - timedelta(days=RAW_RETENTION_DAYS + 1),
              latency=1.0)
    prune_old_cycles(now=NOW)
    assert db.session.query(MonitorResult).count() == 0


def test_prune_keeps_rollups(db, node, device):
    hour = (NOW - timedelta(days=RAW_RETENTION_DAYS + 1)).replace(
        minute=0, second=0, microsecond=0)
    add_cycle(db, node, device, hour, latency=5.0)
    roll_up_hour(hour)
    prune_old_cycles(now=NOW)
    assert db.session.query(MonitorCycle).count() == 0
    assert db.session.query(MonitorRollup).count() == 1
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_maintenance_rollup.py -v`
Expected: FAIL — `ImportError: cannot import name 'roll_up_hour'`

- [ ] **Step 3: Extend the implementation**

Add to the imports at the top of `server/maintenance.py`:

```python
from server.models import Job, MonitorCycle, MonitorResult, MonitorRollup
```

(replacing the existing `from server.models import Job` line)

Append to `server/maintenance.py`:

```python
RAW_RETENTION_DAYS = 7


def roll_up_hour(hour_start: datetime) -> int:
    """Summarise one hour of monitor results per (node, device). Returns rows written.

    Idempotent: re-running the same hour overwrites rather than duplicating,
    so a restart mid-maintenance cannot double-count.
    """
    hour_end = hour_start + timedelta(hours=1)
    rows = db.session.execute(
        db.select(
            MonitorCycle.node_id,
            MonitorResult.device_id,
            db.func.count(MonitorResult.id),
            db.func.sum(db.case((MonitorResult.status == "up", 1), else_=0)),
            db.func.min(MonitorResult.latency_ms),
            db.func.avg(MonitorResult.latency_ms),
            db.func.max(MonitorResult.latency_ms),
        )
        .join(MonitorResult, MonitorResult.cycle_id == MonitorCycle.id)
        .where(MonitorCycle.cycle_ts >= hour_start)
        .where(MonitorCycle.cycle_ts < hour_end)
        .group_by(MonitorCycle.node_id, MonitorResult.device_id)
    ).all()

    for node_id, device_id, samples, up_count, low, avg, high in rows:
        existing = db.session.execute(
            db.select(MonitorRollup).filter_by(
                node_id=node_id, device_id=device_id, hour_ts=hour_start)
        ).scalar_one_or_none()
        target = existing or MonitorRollup(
            node_id=node_id, device_id=device_id, hour_ts=hour_start)
        target.samples = samples
        target.up_count = int(up_count or 0)
        target.latency_min = low
        target.latency_avg = avg
        target.latency_max = high
        if existing is None:
            db.session.add(target)

    db.session.commit()
    return len(rows)


def prune_old_cycles(now: datetime | None = None) -> int:
    """Delete monitor cycles past the raw retention window. Returns how many.

    A bulk delete bypasses the ORM's cascade, so the child monitor_results
    rows are removed by the database itself — which is why server/db.py turns
    on SQLite's foreign key pragma.
    """
    now = now or _utcnow()
    cutoff = now - timedelta(days=RAW_RETENTION_DAYS)
    deleted = db.session.execute(
        db.delete(MonitorCycle).where(MonitorCycle.cycle_ts < cutoff)
    ).rowcount
    db.session.commit()
    return deleted
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python -m pytest tests/test_maintenance_rollup.py -v`
Expected: PASS — 6 passed

- [ ] **Step 5: Run the whole suite**

Run: `python -m pytest tests/ -v`
Expected: PASS — all green, including the 131 protocol tests from Plan 1

- [ ] **Step 6: Commit**

```bash
git add server/maintenance.py tests/test_maintenance_rollup.py
git commit -m "feat(server): hourly rollups and raw cycle pruning"
```

---

## Definition of done

- [ ] `python -m pytest tests/ -v` passes with every test green
- [ ] `server/` imports without any MQTT dependency — Plan A touches no broker
- [ ] Every spec §7.2 handler exists and has tests
- [ ] A bulk `DELETE` on `monitor_cycles` removes child `monitor_results`

## Spec coverage

| Spec section | Covered by |
|---|---|
| §5 module structure (`db`, `models`, `events`, `ingest`, `maintenance`) | Tasks 1–8 |
| §6.1 nine tables | Task 2 |
| §6.2 RF-only projection | Task 6 |
| §6.3 retention, rollups | Task 8 |
| §7.2 all five handlers | Tasks 4, 5, 6 |
| §7.3 bounded event bus | Task 3 |
| §8 job state machine, gap detection | Task 6 |
| §8 sliding deadline, timeout sweep | Task 7 |
| §4 MQTT bridge, §9 virtual probe | Plan C |
| §10 API, §11 auth | Plan B |
| §12 Docker, Mosquitto config | Plan C |
| §13 conformance suite | Plan C |
