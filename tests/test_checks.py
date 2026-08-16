import socket

from probe.checks import (
    build_ping_command, check_port, parse_latency_ms,
)


def test_build_ping_command_sends_one_probe():
    cmd = build_ping_command("192.168.1.1")
    assert cmd[0] == "ping"
    assert "192.168.1.1" in cmd
    # Exactly one echo request, whichever platform flag names it.
    assert ("-n" in cmd and cmd[cmd.index("-n") + 1] == "1") or \
           ("-c" in cmd and cmd[cmd.index("-c") + 1] == "1")


def test_parse_latency_reads_milliseconds():
    assert parse_latency_ms("64 bytes from x: time=1.8 ms") == 1.8


def test_parse_latency_handles_sub_millisecond():
    assert parse_latency_ms("Reply from x: time<1ms TTL=64") == 1.0


def test_parse_latency_returns_none_when_absent():
    assert parse_latency_ms("Request timed out.") is None


def test_check_port_reports_closed_for_a_dead_port():
    # 1 is almost never listening on loopback.
    assert check_port("127.0.0.1", 1, timeout=0.5) == "closed"


def test_check_port_reports_open_for_a_listening_socket():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]
    try:
        assert check_port("127.0.0.1", port, timeout=0.5) == "open"
    finally:
        server.close()
