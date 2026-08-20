"""Command implementations for the virtual probe.

Each runner is a generator yielding chunk payloads. The virtual probe wraps
each yielded dict in a `result`/`chunk` envelope with an incrementing seq, so
the streaming shape here matches what firmware must produce. Capabilities cover
what a server can actually do — no wifi_survey, because it has no radio.
"""

import socket
from collections.abc import Iterator

from probe import checks
from protocol.ports import parse_ports

CAPABILITIES = ["port_scan", "banner_grab", "dns", "trace", "discover", "rf_sniff"]


def port_scan(args: dict) -> Iterator[dict]:
    """One chunk per target: the ports found open on it."""
    ports = parse_ports(args["ports"])
    timeout = args.get("timeout_ms", 1000) / 1000
    for host in args["targets"]:
        open_ports = [
            {"host": host, "port": port}
            for port in ports
            if checks.check_port(host, port, timeout=timeout) == "open"
        ]
        yield {"open": open_ports}


def banner_grab(args: dict) -> Iterator[dict]:
    """Read whatever a service announces on connect, per (host, port)."""
    timeout = args.get("timeout_ms", 1000) / 1000
    banners = []
    for host in args["targets"]:
        for port in parse_ports(args.get("ports", "22,80")):
            banners.append({"host": host, "port": port,
                            "banner": _read_banner(host, port, timeout)})
    yield {"banners": banners}


def _read_banner(host: str, port: int, timeout: float) -> str | None:
    try:
        with socket.create_connection((host, port), timeout=timeout) as conn:
            conn.settimeout(timeout)
            data = conn.recv(256)
        return data.decode("latin-1", errors="replace").strip() or None
    except OSError:
        return None


def dns(args: dict) -> Iterator[dict]:
    """Resolve each name to its addresses."""
    answers = []
    for name in args["targets"]:
        try:
            infos = socket.getaddrinfo(name, None)
            addresses = sorted({info[4][0] for info in infos})
        except socket.gaierror:
            addresses = []
        answers.append({"name": name, "addresses": addresses})
    yield {"answers": answers}


def discover(args: dict) -> Iterator[dict]:
    """Ping a list of hosts and report which answer."""
    up = []
    for host in args["targets"]:
        if checks.ping(host)["status"] == "up":
            up.append(host)
    yield {"up": up}


def trace(args: dict) -> Iterator[dict]:
    """A degenerate traceroute: the virtual probe reports reachability only."""
    for host in args["targets"]:
        yield {"host": host, "reachable": checks.ping(host)["status"] == "up"}


def rf_sniff(args: dict) -> Iterator[dict]:
    """RF-Sentinel: sub-GHz carrier telemetry.

    The one deliberately *synthetic* runner. Every other runner above does real
    host I/O; this host has no CC1101, so — exactly as ble_scan/wifi_survey are
    absent because a server has no radio — rf_sniff instead fabricates a
    deterministic carrier sweep. It exists to exercise the protocol path
    (schema -> chunk -> ingest projection -> dashboard) end-to-end and to drive
    the demo simulator before any board arrives. Firmware on a real CC1101
    produces the same chunk shape from live captures.

    Deterministic (no RNG) so golden/conformance behaviour is stable: it steps
    across the requested band at 250 kHz and reports a carrier at each step whose
    RSSI is a fixed function of its offset from 433.92 MHz (the 433 ISM centre),
    strongest at the centre and falling off with distance.
    """
    lo = float(args["freq_min_mhz"])
    hi = float(args["freq_max_mhz"])
    carriers = []
    freq = lo
    while freq <= hi + 1e-9:
        rssi = int(round(-60 - min(30.0, abs(freq - 433.92) * 40)))
        carriers.append({"freq_mhz": round(freq, 2), "rssi": rssi,
                         "bandwidth_khz": 58, "packets": 1})
        freq += 0.25
    yield {"carriers": carriers}


RUNNERS = {"port_scan": port_scan, "banner_grab": banner_grab, "dns": dns,
           "discover": discover, "trace": trace, "rf_sniff": rf_sniff}


def run_command(cmd: str, args: dict) -> Iterator[dict]:
    """Dispatch to a runner, yielding its chunk payloads."""
    runner = RUNNERS.get(cmd)
    if runner is None:
        raise ValueError(f"unsupported command {cmd!r}")
    yield from runner(args)
