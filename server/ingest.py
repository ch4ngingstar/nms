"""Handlers for validated protocol messages (spec §7.2).

Deliberately free of MQTT and threading: each handler takes an already
validated message dict and writes rows, so protocol semantics are testable
by calling a function. The client that feeds them lives in mqtt_bridge.
"""

from datetime import datetime, timezone

from server.db import db
from server.events import bus
from server.models import Node


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
