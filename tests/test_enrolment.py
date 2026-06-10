import pytest

from protocol.errors import ProtocolError
from server.enrolment import AlreadyEnrolled, enrol_node
from server.models import Node


def test_enrol_creates_an_unprovisioned_node(db):
    result = enrol_node("probe-a4c1f8", label="Lab North")
    stored = db.session.get(Node, "probe-a4c1f8")
    assert stored.state == "unprovisioned"
    assert stored.label == "Lab North"
    assert stored.capabilities == []


def test_enrol_returns_a_password_and_acl_block(db):
    result = enrol_node("probe-a4c1f8")
    assert result["password"]
    assert "user probe-a4c1f8" in result["acl_block"]
    assert "topic read nms/v1/node/probe-a4c1f8/cmd" in result["acl_block"]


def test_enrol_returns_apply_commands_naming_the_node(db):
    result = enrol_node("probe-a4c1f8")
    joined = "\n".join(result["apply"])
    assert "mosquitto_passwd" in joined
    assert "probe-a4c1f8" in joined


def test_enrol_rejects_an_invalid_node_id(db):
    with pytest.raises(ProtocolError):
        enrol_node("not-a-probe")


def test_enrol_rejects_a_duplicate(db):
    enrol_node("probe-a4c1f8")
    with pytest.raises(AlreadyEnrolled):
        enrol_node("probe-a4c1f8")


def test_each_enrolment_password_is_distinct(db):
    first = enrol_node("probe-a4c1f8")
    second = enrol_node("probe-7e2b10")
    assert first["password"] != second["password"]
