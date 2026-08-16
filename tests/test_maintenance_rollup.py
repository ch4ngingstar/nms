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
