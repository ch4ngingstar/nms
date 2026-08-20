"""Command dispatch: turn an operator request into a published cmd.

Job creation is deliberately split from the MQTT client. This module builds
and validates the command message, records the job row, and hands the encoded
payload to a pluggable publisher. The bridge registers the real publisher at
startup; tests register a sink. Nothing here imports paho, so the whole job
lifecycle is testable without a broker.
"""

import json
import secrets
import time

from protocol.topics import cmd_topic
from protocol.validate import validate_message
from server.db import db
from server.events import bus
from server.models import Job, Node

# Control commands are a mandatory baseline every node must honour, so they are
# dispatchable regardless of the advertised capability set. Recon commands must
# appear in the node's capabilities.
CONTROL_COMMANDS = {"set_monitor", "cancel", "identify", "reboot", "get_config"}

# Every recon command the protocol defines. A node still only runs the subset it
# advertises in `announce.capabilities`; this set just separates "unknown command"
# from "known command this node did not advertise" for a clearer rejection.
# ble_scan and wifi_ids are the Phase-4 radio additions.
RECON_COMMANDS = frozenset({
    "port_scan", "banner_grab", "dns", "trace", "discover",
    "wifi_survey", "ble_scan", "wifi_ids", "wifi_deauth",
})


class CommandError(ValueError):
    """A command request that cannot be dispatched as asked."""


class UnknownNode(CommandError):
    """No such node is enrolled."""


class CapabilityError(CommandError):
    """The node has not advertised the requested recon capability."""


def _default_publisher(topic: str, payload: bytes) -> None:
    raise RuntimeError("no MQTT publisher configured; call set_publisher()")


_publisher = _default_publisher


def set_publisher(publish) -> None:
    """Register the callable the bridge uses to publish `(topic, payload)`."""
    global _publisher
    _publisher = publish


def _new_job_id() -> str:
    return "job-" + secrets.token_hex(6)


def _envelope(node_id: str, data: dict) -> dict:
    return {"v": 1, "type": "cmd", "node": node_id,
            "msg_id": secrets.token_hex(8), "ts": int(time.time()), "data": data}


def _publish_cmd(node_id: str, data: dict) -> None:
    message = _envelope(node_id, data)
    validate_message(message)
    _publisher(cmd_topic(node_id), json.dumps(message).encode("utf-8"))


def create_job(node_id: str, cmd: str, args: dict | None = None) -> Job:
    """Validate, record, and publish one command. Returns the pending job."""
    args = args or {}
    node = db.session.get(Node, node_id)
    if node is None:
        raise UnknownNode(f"no node {node_id!r} is enrolled")
    if cmd not in CONTROL_COMMANDS:
        if cmd not in RECON_COMMANDS:
            raise CapabilityError(f"unknown command {cmd!r}")
        if cmd not in (node.capabilities or []):
            raise CapabilityError(
                f"node {node_id!r} does not advertise capability {cmd!r}")

    job_id = _new_job_id()
    data = {"job_id": job_id, "cmd": cmd, "args": args}
    # Validate before writing a row: a rejected command must leave no trace.
    _publish_cmd(node_id, data)

    job = Job(job_id=job_id, node_id=node_id, cmd=cmd, args=args, state="pending")
    db.session.add(job)
    db.session.commit()

    bus.publish("job_event", {"job_id": job_id, "node": node_id,
                              "event": "created", "state": "pending"})
    return job


def cancel_job(job_id: str) -> Job | None:
    """Ask the owning node to abandon a job. Returns the job, or None if absent.

    The job is not marked terminal here; the node answers with an `error`
    event carrying code `cancelled`, which the ingest path records.
    """
    job = db.session.get(Job, job_id)
    if job is None:
        return None
    _publish_cmd(job.node_id, {"job_id": job_id, "cmd": "cancel", "args": {}})
    return job
