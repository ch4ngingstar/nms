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
