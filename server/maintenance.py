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
