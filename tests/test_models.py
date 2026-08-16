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
