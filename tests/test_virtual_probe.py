import json
import socket

import pytest

from probe import virtual_probe
from probe.virtual_probe import VirtualProbe


@pytest.fixture
def sink():
    sent = []

    def publish(topic, payload):
        sent.append((topic, json.loads(payload)))

    return sent, publish


def cmd(cmd_name, job_id="job-1234", args=None):
    return {"v": 1, "type": "cmd", "node": "probe-server",
            "msg_id": "01J8X2K9QWER", "ts": 1755302400,
            "data": {"job_id": job_id, "cmd": cmd_name, "args": args or {}}}


def events(sent):
    return [body["data"].get("event") for _, body in sent
            if body["type"] == "result"]


def test_announce_advertises_capabilities_without_wifi(sink):
    sent, publish = sink
    probe = VirtualProbe("probe-server", publish)
    probe.announce()
    topic, body = sent[0]
    assert topic == "nms/v1/announce"
    assert body["type"] == "announce"
    assert "wifi_survey" not in body["data"]["capabilities"]


def test_clean_job_streams_accepted_chunks_done(sink):
    sent, publish = sink
    probe = VirtualProbe("probe-server", publish)
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]
    try:
        probe.handle_cmd(cmd("port_scan",
                             args={"targets": ["127.0.0.1"], "ports": str(port),
                                   "timeout_ms": 500}))
    finally:
        server.close()
    assert events(sent) == ["accepted", "chunk", "done"]


def test_chunk_seqs_are_contiguous_from_zero(sink):
    sent, publish = sink
    probe = VirtualProbe("probe-server", publish)
    probe.handle_cmd(cmd("dns", args={"targets": ["localhost", "localhost"]}))
    seqs = [body["data"]["seq"] for _, body in sent
            if body["type"] == "result" and body["data"].get("event") == "chunk"]
    assert seqs == list(range(len(seqs)))


def test_second_job_while_busy_is_rejected(sink):
    sent, publish = sink
    probe = VirtualProbe("probe-server", publish)
    probe._busy = True  # simulate a job already running
    probe.handle_cmd(cmd("dns", job_id="job-9999", args={"targets": ["localhost"]}))
    error = [body["data"] for _, body in sent
             if body["type"] == "result" and body["data"]["event"] == "error"][0]
    assert error["code"] == "busy"
    assert error["job_id"] == "job-9999"


def test_cancel_mid_stream_ends_in_cancelled(sink, monkeypatch):
    sent, publish = sink
    probe = VirtualProbe("probe-server", publish)

    def fake_runner(args):
        yield {"open": []}
        probe._cancel_job = "job-1234"   # cancel arrives after the first chunk
        yield {"open": []}

    monkeypatch.setattr(virtual_probe, "run_command",
                        lambda cmd_name, args: fake_runner(args))
    probe.handle_cmd(cmd("port_scan", args={"targets": ["x"], "ports": "1"}))
    assert events(sent) == ["accepted", "chunk", "error"]
    last = sent[-1][1]["data"]
    assert last["code"] == "cancelled"


def test_unsupported_recon_command_errors(sink):
    sent, publish = sink
    probe = VirtualProbe("probe-server", probe_publish := publish)
    # wifi_survey has no runner on a server.
    probe.handle_cmd(cmd("wifi_survey", args={"duration_s": 5}))
    error = [body["data"] for _, body in sent
             if body["type"] == "result" and body["data"]["event"] == "error"][0]
    assert error["code"] == "unsupported"


def test_set_monitor_is_persisted(sink):
    sent, publish = sink
    probe = VirtualProbe("probe-server", publish)
    config = {"enabled": True, "interval_s": 5,
              "devices": [{"id": 1, "ip": "10.0.0.1", "checks": ["ping"]}]}
    probe.handle_cmd(cmd("set_monitor", args=config))
    assert probe.monitor_config == config
    assert events(sent) == ["accepted", "done"]
