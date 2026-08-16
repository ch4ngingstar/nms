import json

from server.mqtt_bridge import HANDLERS, dispatch, topic_leaf
from server.models import Node

TS = 1755302400

ANNOUNCE = {
    "v": 1, "type": "announce", "node": "probe-a4c1f8",
    "msg_id": "01J8X2K9QWER", "ts": TS,
    "data": {"label": "Lab North", "fw": "1.2.0", "chip": "esp32s3",
             "mac": "a0:b7:65:a4:c1:f8", "free_heap": 214512,
             "capabilities": ["port_scan", "wifi_survey"]},
}


def _payload(message) -> bytes:
    return json.dumps(message).encode("utf-8")


def test_topic_leaf_extracts_the_last_segment():
    assert topic_leaf("nms/v1/node/probe-a4c1f8/status") == "status"
    assert topic_leaf("nms/v1/announce") == "announce"


def test_dispatch_routes_a_valid_announce(db):
    dispatch("nms/v1/announce", _payload(ANNOUNCE))
    assert db.session.get(Node, "probe-a4c1f8").fw == "1.2.0"


def test_dispatch_drops_malformed_json(db):
    dispatch("nms/v1/announce", b"{not json")
    assert db.session.query(Node).count() == 0


def test_dispatch_drops_a_schema_invalid_message(db):
    broken = json.loads(json.dumps(ANNOUNCE))
    del broken["data"]["fw"]  # fw is required by the announce schema
    dispatch("nms/v1/announce", _payload(broken))
    assert db.session.query(Node).count() == 0


def test_dispatch_ignores_the_cmd_topic(db):
    """The server publishes commands; it must not re-ingest its own echoes."""
    dispatch("nms/v1/node/probe-a4c1f8/cmd", _payload(ANNOUNCE))
    assert db.session.query(Node).count() == 0


def test_every_ingest_leaf_has_a_handler():
    assert set(HANDLERS) == {"announce", "status", "telemetry", "monitor", "result"}
