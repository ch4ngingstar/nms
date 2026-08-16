"""Message validation against the Probe Protocol v1 schemas (spec §5, §9)."""

import json
from functools import lru_cache
from pathlib import Path

from jsonschema import Draft202012Validator

from protocol import MAX_PAYLOAD_BYTES
from protocol.errors import ProtocolError

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
    """Enforce the 1024-byte published-message ceiling (spec §5.3)."""
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
