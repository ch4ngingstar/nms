from server.ingest import handle_announce, handle_status
from server.models import Node

TS = 1755302400


def envelope(msg_type, data, node="probe-a4c1f8", ts=TS):
    return {"v": 1, "type": msg_type, "node": node,
            "msg_id": "01J8X2K9QWER", "ts": ts, "data": data}


ANNOUNCE = {"label": "Lab North", "fw": "1.2.0", "chip": "esp32s3",
            "mac": "a0:b7:65:a4:c1:f8", "free_heap": 214512,
            "capabilities": ["port_scan", "wifi_survey"]}


def test_announce_creates_an_unknown_node(db):
    handle_announce(envelope("announce", ANNOUNCE))
    stored = db.session.get(Node, "probe-a4c1f8")
    assert stored.fw == "1.2.0"
    assert stored.capabilities == ["port_scan", "wifi_survey"]
    assert stored.label == "Lab North"


def test_announce_updates_an_existing_node(db, node):
    original_first_seen = node.first_seen
    updated = dict(ANNOUNCE, fw="1.3.0", capabilities=["port_scan"])
    handle_announce(envelope("announce", updated))
    stored = db.session.get(Node, "probe-a4c1f8")
    assert stored.fw == "1.3.0"
    assert stored.capabilities == ["port_scan"]
    assert stored.first_seen == original_first_seen, "first_seen must not move"


def test_announce_without_label_keeps_the_existing_one(db, node):
    data = {k: v for k, v in ANNOUNCE.items() if k != "label"}
    handle_announce(envelope("announce", data))
    assert db.session.get(Node, "probe-a4c1f8").label == "Lab North"


def test_status_updates_state(db, node):
    handle_status(envelope("status", {"state": "busy", "job": "job-1"}))
    assert db.session.get(Node, "probe-a4c1f8").state == "busy"


def test_offline_status_does_not_advance_last_seen(db, node):
    before = node.last_seen
    handle_status(envelope("status", {"state": "offline", "reason": "lwt"},
                           ts=TS + 3600))
    stored = db.session.get(Node, "probe-a4c1f8")
    assert stored.state == "offline"
    assert stored.last_seen == before, "a death notice is not a sign of life"


def test_surveying_status_advances_last_seen(db, node):
    before = node.last_seen
    handle_status(envelope("status", {"state": "surveying", "expect_back_in": 30},
                           ts=TS + 60))
    stored = db.session.get(Node, "probe-a4c1f8")
    assert stored.state == "surveying"
    assert stored.last_seen != before, "a live node must update last_seen"


def test_status_from_unknown_node_creates_it(db):
    handle_status(envelope("status", {"state": "online"}, node="probe-7e2b10"))
    assert db.session.get(Node, "probe-7e2b10").state == "online"


def test_announce_publishes_an_event(db, monkeypatch):
    seen = []
    from server import ingest
    monkeypatch.setattr(ingest.bus, "publish",
                        lambda kind, payload: seen.append((kind, payload)))
    handle_announce(envelope("announce", ANNOUNCE))
    assert seen[0][0] == "node_status"
