"""Handlers for validated protocol messages."""

from datetime import datetime, timezone

from server.db import db
from server.events import bus
from server.models import Node, Telemetry


def to_datetime(epoch_seconds: int) -> datetime:
    return datetime.fromtimestamp(epoch_seconds, tz=timezone.utc)


def get_or_create_node(node_id: str, seen_at: datetime) -> Node:
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
