import json

import pytest

from protocol.errors import ProtocolError
from server import commands
from server.commands import (
    CapabilityError, UnknownNode, cancel_job, create_job,
)
from server.models import Job

PORT_SCAN_ARGS = {"targets": ["10.0.0.5"], "ports": "22,80"}


@pytest.fixture
def captured(monkeypatch):
    """Replace the MQTT publisher with a sink that records every publish."""
    sent = []
    monkeypatch.setattr(commands, "_publisher",
                        lambda topic, payload: sent.append((topic, payload)))
    return sent


def test_create_job_inserts_a_pending_row(db, node, captured):
    job = create_job("probe-a4c1f8", "port_scan", PORT_SCAN_ARGS)
    stored = db.session.get(Job, job.job_id)
    assert stored.state == "pending"
    assert stored.cmd == "port_scan"
    assert stored.args == PORT_SCAN_ARGS


def test_create_job_publishes_to_the_cmd_topic(db, node, captured):
    job = create_job("probe-a4c1f8", "port_scan", PORT_SCAN_ARGS)
    topic, payload = captured[0]
    assert topic == "nms/v1/node/probe-a4c1f8/cmd"
    message = json.loads(payload)
    assert message["type"] == "cmd"
    assert message["data"]["job_id"] == job.job_id
    assert message["data"]["cmd"] == "port_scan"


def test_recon_command_not_in_capabilities_is_rejected(db, node, captured):
    """wifi_survey is a capability this node happens to have; dns is not."""
    with pytest.raises(CapabilityError):
        create_job("probe-a4c1f8", "dns", {"targets": ["example.com"]})
    assert captured == []


def test_control_command_needs_no_capability(db, node, captured):
    """identify/reboot/get_config are a mandatory baseline (spec §8)."""
    create_job("probe-a4c1f8", "identify", {})
    assert captured[0][0] == "nms/v1/node/probe-a4c1f8/cmd"


def test_unknown_node_is_rejected(db, captured):
    with pytest.raises(UnknownNode):
        create_job("probe-000000", "identify", {})


def test_invalid_args_are_rejected(db, node, captured):
    with pytest.raises(ProtocolError):
        create_job("probe-a4c1f8", "port_scan", {"ports": ""})  # missing targets
    assert db.session.query(Job).count() == 0
    assert captured == []


def test_job_ids_are_unique(db, node, captured):
    first = create_job("probe-a4c1f8", "identify", {})
    second = create_job("probe-a4c1f8", "identify", {})
    assert first.job_id != second.job_id


def test_cancel_publishes_a_cancel_command(db, node, captured):
    job = create_job("probe-a4c1f8", "port_scan", PORT_SCAN_ARGS)
    captured.clear()
    returned = cancel_job(job.job_id)
    assert returned.job_id == job.job_id
    topic, payload = captured[0]
    assert topic == "nms/v1/node/probe-a4c1f8/cmd"
    assert json.loads(payload)["data"]["cmd"] == "cancel"


def test_cancel_of_unknown_job_returns_none(db, node, captured):
    assert cancel_job("job-nope") is None
    assert captured == []
