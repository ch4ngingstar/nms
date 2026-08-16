"""Handlers for validated protocol messages (spec §7.2).

Deliberately free of MQTT and threading: each handler takes an already
validated message dict and writes rows, so protocol semantics are testable
by calling a function. The client that feeds them lives in mqtt_bridge.
"""

from datetime import datetime, timezone

from server.db import db
from server.events import bus
from server.models import Device, MonitorCycle, MonitorResult, Node, Telemetry


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
