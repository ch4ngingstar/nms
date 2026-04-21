import pytest

from protocol.errors import ProtocolError
from protocol.topics import (
    ANNOUNCE_TOPIC,
    cmd_topic,
    monitor_topic,
    node_id_from_topic,
    result_topic,
    status_topic,
    telemetry_topic,
    validate_node_id,
)


@pytest.mark.parametrize("node_id", ["probe-a4c1f8", "probe-000000", "probe-server"])
def test_valid_node_ids_accepted(node_id):
    assert validate_node_id(node_id) == node_id


@pytest.mark.parametrize(
    "node_id",
    ["probe-A4C1F8", "probe-a4c1f", "probe-a4c1f88", "a4c1f8", "probe-", "probe-lab-north"],
)
def test_invalid_node_ids_rejected(node_id):
    with pytest.raises(ProtocolError):
        validate_node_id(node_id)


def test_topic_construction():
    assert cmd_topic("probe-a4c1f8") == "nms/v1/node/probe-a4c1f8/cmd"
    assert result_topic("probe-a4c1f8") == "nms/v1/node/probe-a4c1f8/result"
    assert monitor_topic("probe-a4c1f8") == "nms/v1/node/probe-a4c1f8/monitor"
    assert status_topic("probe-a4c1f8") == "nms/v1/node/probe-a4c1f8/status"
    assert telemetry_topic("probe-a4c1f8") == "nms/v1/node/probe-a4c1f8/telemetry"
    assert ANNOUNCE_TOPIC == "nms/v1/announce"


def test_topic_construction_validates_node_id():
    with pytest.raises(ProtocolError):
        cmd_topic("probe-NOPE")


def test_node_id_extracted_from_topic():
    assert node_id_from_topic("nms/v1/node/probe-a4c1f8/result") == "probe-a4c1f8"


@pytest.mark.parametrize(
    "topic",
    ["nms/v1/announce", "nms/v2/node/probe-a4c1f8/cmd", "node/probe-a4c1f8/cmd", ""],
)
def test_node_id_from_bad_topic_rejected(topic):
    with pytest.raises(ProtocolError):
        node_id_from_topic(topic)
