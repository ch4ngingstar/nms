"""Broker credential and ACL generation (spec §8.1, §8.2)."""

import secrets

from protocol.topics import (
    ANNOUNCE_TOPIC,
    NAMESPACE,
    cmd_topic,
    monitor_topic,
    result_topic,
    status_topic,
    telemetry_topic,
    validate_node_id,
)

PASSWORD_BYTES = 24


def generate_password() -> str:
    """Return a fresh URL-safe secret for one node."""
    return secrets.token_urlsafe(PASSWORD_BYTES)


def acl_block(node_id: str) -> str:
    """Mosquitto ACL confining one probe to its own topics."""
    validate_node_id(node_id)
    lines = [f"user {node_id}"]
    lines += [
        f"topic write {topic(node_id)}"
        for topic in (result_topic, monitor_topic, status_topic, telemetry_topic)
    ]
    lines.append(f"topic write {ANNOUNCE_TOPIC}")
    lines.append(f"topic read {cmd_topic(node_id)}")
    return "\n".join(lines) + "\n"


def server_acl_block(username: str) -> str:
    """Mosquitto ACL for the command-and-control server account."""
    if not username or not username.isascii():
        raise ValueError("username must be a non-empty ASCII string")
    return (
        f"user {username}\n"
        f"topic read {NAMESPACE}/#\n"
        f"topic write {NAMESPACE}/node/+/cmd\n"
    )
