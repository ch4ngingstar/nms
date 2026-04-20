"""Topic construction and node identity for Probe Protocol v1."""

import re

from protocol.errors import ProtocolError

NAMESPACE = "nms/v1"
ANNOUNCE_TOPIC = f"{NAMESPACE}/announce"

NODE_ID_RE = re.compile(r"^probe-(?:[0-9a-f]{6}|server)$")
_TOPIC_RE = re.compile(rf"^{NAMESPACE}/node/(probe-(?:[0-9a-f]{{6}}|server))/[a-z]+$")


def validate_node_id(node_id: str) -> str:
    """Return node_id unchanged, or raise if it is not a valid identity."""
    if not isinstance(node_id, str) or not NODE_ID_RE.match(node_id):
        raise ProtocolError(
            f"invalid node_id {node_id!r}: expected 'probe-<6 lowercase hex>' or 'probe-server'"
        )
    return node_id


def _node_topic(node_id: str, leaf: str) -> str:
    return f"{NAMESPACE}/node/{validate_node_id(node_id)}/{leaf}"


def cmd_topic(node_id: str) -> str:
    return _node_topic(node_id, "cmd")


def result_topic(node_id: str) -> str:
    return _node_topic(node_id, "result")


def monitor_topic(node_id: str) -> str:
    return _node_topic(node_id, "monitor")


def status_topic(node_id: str) -> str:
    return _node_topic(node_id, "status")


def telemetry_topic(node_id: str) -> str:
    return _node_topic(node_id, "telemetry")


def node_id_from_topic(topic: str) -> str:
    """Extract the node_id from a per-node topic."""
    match = _TOPIC_RE.match(topic) if isinstance(topic, str) else None
    if not match:
        raise ProtocolError(f"cannot extract node_id from topic {topic!r}")
    return match.group(1)
