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
