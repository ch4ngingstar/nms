import socket

import pytest

from probe.jobs import CAPABILITIES, run_command


def test_capabilities_exclude_wifi_survey():
    """A server has no radio; capabilities exist precisely for this."""
    assert "wifi_survey" not in CAPABILITIES
    assert "port_scan" in CAPABILITIES


def test_port_scan_finds_a_listening_port():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]
    try:
        chunks = list(run_command("port_scan",
                                  {"targets": ["127.0.0.1"], "ports": str(port),
                                   "timeout_ms": 500}))
    finally:
        server.close()
    opened = [entry for chunk in chunks for entry in chunk["open"]]
    assert {"host": "127.0.0.1", "port": port} in opened


def test_port_scan_reports_no_open_closed_port():
    chunks = list(run_command("port_scan",
                              {"targets": ["127.0.0.1"], "ports": "1",
                               "timeout_ms": 300}))
    assert all(chunk["open"] == [] for chunk in chunks)


def test_dns_resolves_localhost():
    chunks = list(run_command("dns", {"targets": ["localhost"]}))
    answers = chunks[0]["answers"]
    assert answers[0]["name"] == "localhost"
    assert any(addr.startswith("127.") or addr == "::1"
               for addr in answers[0]["addresses"])


def test_unsupported_command_raises():
    with pytest.raises(ValueError):
        list(run_command("wifi_survey", {"duration_s": 5}))


def test_rf_sniff_is_advertised():
    """Unlike the radio commands, rf_sniff IS advertised: the virtual probe
    fabricates its carriers, so it can serve the demo/fixture path with no CC1101."""
    assert "rf_sniff" in CAPABILITIES


def test_rf_sniff_emits_deterministic_carriers_across_the_band():
    args = {"freq_min_mhz": 433.0, "freq_max_mhz": 434.0, "duration_s": 5, "gain": 32}
    first = list(run_command("rf_sniff", args))
    second = list(run_command("rf_sniff", args))
    assert first == second                       # deterministic: no RNG
    carriers = first[0]["carriers"]
    assert len(carriers) == 5                    # 433.0..434.0 stepped by 0.25
    assert all(433.0 <= c["freq_mhz"] <= 434.0 for c in carriers)
    # RSSI falls off with distance from the 433.92 MHz ISM centre.
    nearest = min(carriers, key=lambda c: abs(c["freq_mhz"] - 433.92))
    assert nearest["rssi"] == max(c["rssi"] for c in carriers)
