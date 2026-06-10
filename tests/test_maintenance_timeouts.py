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
    """An announced promiscuous-mode absence is not a hang."""
    make_job(db, node, "job-rf", NOW - timedelta(seconds=200),
             cmd="wifi_survey", deadline_s=30 + 60)
    assert sweep_timeouts(now=NOW) == 1

    make_job(db, node, "job-rf2", NOW - timedelta(seconds=60),
             cmd="wifi_survey", deadline_s=30 + 60)
    assert sweep_timeouts(now=NOW) == 0


def test_sweep_interval_is_frequent_enough_to_be_useful():
    """An hourly sweep would leave a hung job undetected for an hour."""
    assert SWEEP_INTERVAL_S <= 30
