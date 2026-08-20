"""Table definitions for the C2 server."""

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
    # The deadline slides from the last event, not from creation.
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


class BleObservation(db.Model):
    """A BLE device seen by one probe during a ble_scan job.

    The ble_scan analogue of ap_observations: the same multi-vantage question —
    GROUP BY mac comparing rssi across node_id — is what justifies a typed table
    rather than leaving the data buried in job_chunks payloads.
    """

    __tablename__ = "ble_observations"

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
    mac = db.Column(db.String(17), nullable=False)
    name = db.Column(db.String(64))
    rssi = db.Column(db.Integer)
    connectable = db.Column(db.Boolean, default=False)
    # BT company identifier as reported by the probe (4-hex, e.g. "004C"); the
    # probe emits the raw id and the server resolves the vendor name.
    manufacturer = db.Column(db.String(16))
    observed_at = db.Column(db.DateTime(timezone=True), nullable=False,
                            default=_utcnow, index=True)

    __table_args__ = (
        db.Index("ix_ble_mac_time", "mac", "observed_at"),
    )


class IdsAlert(db.Model):
    """A wireless intrusion-detection alert projected from a wifi_ids job.

    wifi_ids emits heterogeneous alert objects (deauth_flood / rogue_ap /
    evil_twin); this table flattens them to a common shape so the security
    timeline and the rogue-AP view are plain queries. `count` is meaningful only
    for deauth floods; `target_mac` only for a flood's victim — both are null for
    the AP-identity alerts, whose subject MAC lands in `source_mac`.
    """

    __tablename__ = "ids_alerts"

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
    alert_type = db.Column(db.String(24), nullable=False, index=True)
    source_mac = db.Column(db.String(17))
    target_mac = db.Column(db.String(17))
    channel = db.Column(db.Integer)
    count = db.Column(db.Integer)
    detected_at = db.Column(db.DateTime(timezone=True), nullable=False,
                            default=_utcnow, index=True)

    __table_args__ = (
        db.Index("ix_ids_type_time", "alert_type", "detected_at"),
    )
