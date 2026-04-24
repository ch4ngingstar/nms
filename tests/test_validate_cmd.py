import pytest

from protocol.errors import ProtocolError
from protocol.validate import validate_message


def cmd(command, args):
    return {
        "v": 1,
        "type": "cmd",
        "node": "probe-a4c1f8",
        "msg_id": "01J8X2KA1234",
        "ts": 1755302400,
        "data": {"job_id": "job-7f3a91", "cmd": command, "args": args},
    }


def test_valid_port_scan_accepted():
    validate_message(cmd("port_scan", {
        "targets": ["192.168.1.0/24", "10.0.0.5"],
        "ports": "22,80,443,8000-8100",
        "timeout_ms": 500,
        "concurrency": 8,
    }))


def test_port_scan_bad_port_spec_rejected():
    with pytest.raises(ProtocolError):
        validate_message(cmd("port_scan", {"targets": ["10.0.0.5"], "ports": "100-50"}))


def test_port_scan_missing_targets_rejected():
    with pytest.raises(ProtocolError):
        validate_message(cmd("port_scan", {"ports": "22"}))


def test_unknown_command_rejected():
    with pytest.raises(ProtocolError):
        validate_message(cmd("format_disk", {}))


def test_missing_job_id_rejected():
    message = cmd("reboot", {})
    del message["data"]["job_id"]
    with pytest.raises(ProtocolError):
        validate_message(message)


def test_control_commands_need_no_args():
    for command in ("reboot", "get_config"):
        validate_message(cmd(command, {}))


def test_valid_wifi_survey_accepted():
    validate_message(cmd("wifi_survey", {"duration_s": 30, "channels": [1, 6, 11],
                                         "passive": True}))


def test_wifi_survey_bad_channel_rejected():
    with pytest.raises(ProtocolError):
        validate_message(cmd("wifi_survey", {"duration_s": 30, "channels": [99]}))


def test_valid_set_monitor_accepted():
    validate_message(cmd("set_monitor", {
        "enabled": True,
        "interval_s": 5,
        "devices": [{"id": 1, "ip": "192.168.1.1",
                     "checks": ["ping", "port"], "ports": [22, 53, 80]}],
    }))


def test_set_monitor_unknown_check_rejected():
    with pytest.raises(ProtocolError):
        validate_message(cmd("set_monitor", {
            "enabled": True, "interval_s": 5,
            "devices": [{"id": 1, "ip": "192.168.1.1", "checks": ["telepathy"]}],
        }))
