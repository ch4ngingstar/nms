"""Message validation against the Probe Protocol v1 schemas."""

import json
from functools import lru_cache
from pathlib import Path

from jsonschema import Draft202012Validator

from protocol import MAX_PAYLOAD_BYTES
from protocol.errors import ProtocolError
from protocol.ports import parse_ports

SCHEMA_DIR = Path(__file__).parent / "schemas"


@lru_cache(maxsize=None)
def _validator(relative_name: str) -> Draft202012Validator:
    path = SCHEMA_DIR / f"{relative_name}.schema.json"
    if not path.is_file():
        raise ProtocolError(f"no schema for {relative_name!r}")
    return Draft202012Validator(json.loads(path.read_text(encoding="utf-8")))


def _check(validator: Draft202012Validator, instance, label: str) -> None:
    errors = sorted(validator.iter_errors(instance), key=lambda e: list(e.path))
    if errors:
        first = errors[0]
        location = "/".join(str(part) for part in first.path) or "(root)"
        raise ProtocolError(f"{label} invalid at {location}: {first.message}")


def validate_payload_size(payload: bytes) -> bytes:
    """Enforce the 1024-byte published-message ceiling."""
    if len(payload) > MAX_PAYLOAD_BYTES:
        raise ProtocolError(
            f"payload is {len(payload)} bytes, exceeds {MAX_PAYLOAD_BYTES}"
        )
    return payload


def validate_envelope(message) -> dict:
    """Validate only the outer envelope fields."""
    if not isinstance(message, dict):
        raise ProtocolError(f"message must be an object, got {type(message).__name__}")
    _check(_validator("envelope"), message, "envelope")
    return message


def validate_message(message) -> dict:
    """Validate a full message: envelope, type-specific data, and command args."""
    validate_envelope(message)
    data = message["data"]
    _check(_validator(message["type"]), data, f"{message['type']} data")

    if message["type"] == "cmd":
        _validate_command_args(data["cmd"], data.get("args", {}))
    return message


def _validate_command_args(command: str, args: dict) -> None:
    """Validate args against a per-command schema, if one exists."""
    path = SCHEMA_DIR / "args" / f"{command}.schema.json"
    if path.is_file():
        _check(_validator(f"args/{command}"), args, f"{command} args")
    if command == "port_scan":
        parse_ports(args["ports"])
