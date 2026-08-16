"""Background maintenance: job timeouts, rollups, pruning (spec §6.3, §8).

One thread on a 10-second tick. Timeouts are swept every tick because an
hourly sweep would leave a hung job undetected for up to an hour; rollup and
prune run at most once per hour.
"""

from datetime import datetime, timedelta, timezone

from protocol.job import JobState
from server.db import db
from server.models import Job, MonitorCycle, MonitorResult, MonitorRollup

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
