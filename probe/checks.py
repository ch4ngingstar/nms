"""ICMP ping and TCP port probing.

Carried forward essentially unchanged from the working logic in the original
app.py — `build_ping_command`, `parse_latency_ms`, and `check_port` — now keyed
by IP so the virtual probe and firmware can share the same semantics.
"""

import platform
import re
import socket
import subprocess
import time

PING_TIMEOUT_S = 2
_LATENCY_RE = re.compile(r"time[=<]\s*(\d+(?:\.\d+)?)\s*ms", re.IGNORECASE)


def build_ping_command(ip_address: str, timeout_s: int = PING_TIMEOUT_S) -> list[str]:
    """A single-echo ping command for the host platform."""
    if platform.system().lower() == "windows":
        return ["ping", "-n", "1", "-w", str(timeout_s * 1000), ip_address]
    return ["ping", "-c", "1", "-W", str(timeout_s), ip_address]


def parse_latency_ms(output: str) -> float | None:
    """Pull the round-trip time out of ping output; None if not present.

    `time<1ms` (sub-millisecond, common on Windows loopback) reads as 1.0.
    """
    match = _LATENCY_RE.search(output)
    return float(match.group(1)) if match else None


def check_port(ip_address: str, port: int, timeout: float = 1.0) -> str:
    """`open` if a TCP connection is accepted, `closed` otherwise."""
    try:
        with socket.create_connection((ip_address, port), timeout=timeout):
            return "open"
    except (socket.timeout, ConnectionRefusedError, OSError):
        return "closed"


def ping(ip_address: str, timeout_s: int = PING_TIMEOUT_S) -> dict:
    """Ping once, returning `{status, latency_ms, error}`."""
    started_at = time.perf_counter()
    try:
        result = subprocess.run(
            build_ping_command(ip_address, timeout_s),
            capture_output=True, text=True, timeout=timeout_s + 1,
        )
    except subprocess.TimeoutExpired:
        return {"status": "down", "latency_ms": None, "error": "ping timed out"}

    elapsed_ms = round((time.perf_counter() - started_at) * 1000, 2)
    output = f"{result.stdout}\n{result.stderr}"
    if result.returncode == 0:
        return {"status": "up",
                "latency_ms": parse_latency_ms(output) or elapsed_ms,
                "error": None}
    return {"status": "down", "latency_ms": None,
            "error": output.strip()[:255] or "no ping response"}
