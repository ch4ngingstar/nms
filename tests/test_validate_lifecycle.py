import pytest

from protocol.errors import ProtocolError
from protocol.validate import validate_message


def envelope(msg_type, data):
    return {
        "v": 1,
        "type": msg_type,
        "node": "probe-a4c1f8",
        "msg_id": "01J8X2K9QWER",
        "ts": 1755302400,
        "data": data,
    }


ANNOUNCE_DATA = {
    "label": "Lab North",
    "fw": "1.2.0",
    "chip": "esp32s3",
    "mac": "a0:b7:65:a4:c1:f8",
    "free_heap": 214512,
    "capabilities": ["port_scan", "wifi_survey"],
}


def test_valid_announce_accepted():
    validate_message(envelope("announce", ANNOUNCE_DATA))


def test_announce_label_is_optional():
    data = {k: v for k, v in ANNOUNCE_DATA.items() if k != "label"}
    validate_message(envelope("announce", data))


def test_announce_bad_mac_rejected():
    data = dict(ANNOUNCE_DATA, mac="A0-B7-65-A4-C1-F8")
    with pytest.raises(ProtocolError):
        validate_message(envelope("announce", data))


def test_announce_missing_capabilities_rejected():
    data = {k: v for k, v in ANNOUNCE_DATA.items() if k != "capabilities"}
    with pytest.raises(ProtocolError):
        validate_message(envelope("announce", data))


def test_valid_status_accepted():
    validate_message(envelope("status", {"state": "online", "since": 1755302400, "job": None}))


def test_status_busy_carries_job_id():
    validate_message(envelope("status", {"state": "busy", "job": "job-7f3a91"}))


def test_surveying_status_requires_expect_back_in():
    """Spec §6.4: an announced absence must state its expected duration."""
    with pytest.raises(ProtocolError):
        validate_message(envelope("status", {"state": "surveying"}))
    validate_message(envelope("status", {"state": "surveying", "expect_back_in": 30}))


def test_lwt_offline_payload_accepted():
    validate_message(envelope("status", {"state": "offline", "reason": "lwt"}))


def test_unknown_state_rejected():
    with pytest.raises(ProtocolError):
        validate_message(envelope("status", {"state": "asleep"}))


def test_valid_telemetry_accepted():
    validate_message(
        envelope(
            "telemetry",
            {"free_heap": 198320, "uptime_s": 84210, "rssi": -58,
             "channel": 6, "state": "online", "jobs_done": 412},
        )
    )


def test_telemetry_positive_rssi_rejected():
    with pytest.raises(ProtocolError):
        validate_message(envelope("telemetry", {"free_heap": 1, "uptime_s": 1,
                                                "state": "online", "rssi": 20}))
