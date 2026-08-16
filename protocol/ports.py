"""Parsing of the `ports` command argument (spec §7.2)."""

from protocol.errors import ProtocolError

MIN_PORT = 1
MAX_PORT = 65535


def _parse_single(text: str) -> int:
    stripped = text.strip()
    if not stripped.isdigit():
        raise ProtocolError(f"invalid port {text!r}: not a decimal number")
    value = int(stripped)
    if not MIN_PORT <= value <= MAX_PORT:
        raise ProtocolError(f"port {value} out of range {MIN_PORT}-{MAX_PORT}")
    return value


def parse_ports(spec: str) -> list[int]:
    """Parse "22,80,443,8000-8100" into a sorted list of unique port numbers."""
    if not isinstance(spec, str):
        raise ProtocolError(f"ports must be a string, got {type(spec).__name__}")
    if not spec.strip():
        raise ProtocolError("ports must not be empty")

    ports: set[int] = set()
    for element in spec.split(","):
        if not element.strip():
            raise ProtocolError(f"empty element in ports spec {spec!r}")
        if "-" in element:
            bounds = element.split("-")
            if len(bounds) != 2:
                raise ProtocolError(f"malformed range {element!r}: expected 'low-high'")
            low, high = _parse_single(bounds[0]), _parse_single(bounds[1])
            if low > high:
                raise ProtocolError(f"invalid range {element!r}: low exceeds high")
            ports.update(range(low, high + 1))
        else:
            ports.add(_parse_single(element))

    return sorted(ports)
