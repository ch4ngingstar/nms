"""Node enrolment: register a node and mint its broker credentials.

The server generates a password and an ACL block and returns the exact commands
an operator runs to apply them. It never writes broker configuration itself —
Mosquitto's password file uses its own hashing format, and reimplementing that
in Python to save one documented command would be a poor trade. The plaintext
password is returned exactly once, at generation.
"""

from protocol.credentials import acl_block, generate_password
from protocol.topics import validate_node_id
from server.db import db
from server.models import Node


class AlreadyEnrolled(ValueError):
    """A node with this id already exists."""


def enrol_node(node_id: str, label: str | None = None) -> dict:
    """Create an unprovisioned node and return its one-time credentials."""
    validate_node_id(node_id)
    if db.session.get(Node, node_id) is not None:
        raise AlreadyEnrolled(f"node {node_id!r} is already enrolled")

    node = Node(node_id=node_id, label=label, capabilities=[],
                state="unprovisioned")
    db.session.add(node)
    db.session.commit()

    password = generate_password()
    block = acl_block(node_id)
    apply = [
        "docker compose exec mosquitto mosquitto_passwd -b "
        f"/mosquitto/config/passwd {node_id} {password}",
        "# append the returned ACL block to config/mosquitto/aclfile",
        "docker compose restart mosquitto",
    ]
    return {
        "node_id": node_id,
        "label": label,
        "state": node.state,
        "password": password,
        "acl_block": block,
        "apply": apply,
    }
