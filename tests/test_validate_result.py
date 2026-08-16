import pytest

from protocol.errors import ProtocolError
from protocol.validate import validate_message


def result(data):
    return {"v": 1, "type": "result", "node": "probe-a4c1f8",
            "msg_id": "01J8X2KB5678", "ts": 1755302400, "data": data}


def monitor(data):
    return {"v": 1, "type": "monitor", "node": "probe-a4c1f8",
            "msg_id": "01J8X2KC9012", "ts": 1755302400, "data": data}


def test_accepted_event():
    validate_message(result({"event": "accepted", "job_id": "job-7f3a91"}))


def test_chunk_event():
    validate_message(result({
        "event": "chunk", "job_id": "job-7f3a91", "seq": 3,
        "open": [{"host": "192.168.1.10", "port": 22, "state": "open", "rtt_ms": 2.4}],
    }))


def test_chunk_without_seq_rejected():
    with pytest.raises(ProtocolError):
        validate_message(result({"event": "chunk", "job_id": "job-7f3a91",
                                 "open": []}))


def test_chunk_may_declare_dropped():
    validate_message(result({"event": "chunk", "job_id": "job-7f3a91",
                             "seq": 0, "hosts": [], "dropped": 12}))


def test_done_event_with_zero_results():
    validate_message(result({"event": "done", "job_id": "job-7f3a91",
                             "chunks": 0, "results": 0, "duration_ms": 812}))


def test_done_missing_summary_rejected():
    with pytest.raises(ProtocolError):
        validate_message(result({"event": "done", "job_id": "job-7f3a91"}))


def test_error_event():
    validate_message(result({"event": "error", "job_id": "job-7f3a91",
                             "code": "busy", "message": "a job is already running"}))


def test_error_with_unknown_code_rejected():
    with pytest.raises(ProtocolError):
        validate_message(result({"event": "error", "job_id": "job-7f3a91",
                                 "code": "gremlins", "message": "?"}))


def test_result_without_event_rejected():
    """Spec §7.3: event is the discriminator; a result without it is unreadable."""
    with pytest.raises(ProtocolError):
        validate_message(result({"job_id": "job-7f3a91", "seq": 0, "open": []}))


def test_wifi_survey_chunk():
    validate_message(result({
        "event": "chunk", "job_id": "job-7f3a91", "seq": 0,
        "aps": [{"bssid": "a0:b7:65:11:22:33", "ssid": "Home", "channel": 6,
                 "rssi": -61, "auth": "wpa2", "hidden": False}],
        "clients": [{"mac": "de:ad:be:ef:00:01", "bssid": "a0:b7:65:11:22:33",
                     "rssi": -70}],
    }))


def test_valid_monitor_cycle():
    validate_message(monitor({
        "cycle_ts": 1755302400,
        "results": [{"id": 1, "status": "up", "latency_ms": 1.8,
                     "ports": {"22": "open", "53": "closed", "80": "open"}}],
    }))


def test_monitor_down_host_has_null_latency():
    validate_message(monitor({"cycle_ts": 1755302400,
                              "results": [{"id": 2, "status": "down",
                                           "latency_ms": None}]}))


def test_monitor_down_host_with_latency_rejected():
    """Spec §7.6: latency_ms must be null unless status is up."""
    with pytest.raises(ProtocolError):
        validate_message(monitor({"cycle_ts": 1755302400,
                                  "results": [{"id": 2, "status": "down",
                                               "latency_ms": 5.0}]}))


def test_monitor_unknown_port_state_rejected():
    with pytest.raises(ProtocolError):
        validate_message(monitor({"cycle_ts": 1755302400,
                                  "results": [{"id": 1, "status": "up",
                                               "ports": {"22": "ajar"}}]}))
