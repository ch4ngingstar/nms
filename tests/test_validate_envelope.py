import copy

import pytest

from protocol.errors import ProtocolError
from protocol.validate import validate_envelope, validate_payload_size

VALID = {
    "v": 1,
    "type": "telemetry",
    "node": "probe-a4c1f8",
    "msg_id": "01J8X2K9QWER",
    "ts": 1755302400,
    "data": {},
}


def test_valid_envelope_accepted():
    assert validate_envelope(VALID) is VALID


@pytest.mark.parametrize("field", ["v", "type", "node", "msg_id", "ts", "data"])
def test_missing_required_field_rejected(field):
    message = copy.deepcopy(VALID)
    del message[field]
    with pytest.raises(ProtocolError):
        validate_envelope(message)


def test_unknown_type_rejected():
    message = copy.deepcopy(VALID)
    message["type"] = "nonsense"
    with pytest.raises(ProtocolError):
        validate_envelope(message)


def test_wrong_version_rejected():
    message = copy.deepcopy(VALID)
    message["v"] = 2
    with pytest.raises(ProtocolError):
        validate_envelope(message)


def test_millisecond_timestamp_rejected():
    """The classic ms-vs-s bug: 1755302400000 exceeds the year-2100 ceiling."""
    message = copy.deepcopy(VALID)
    message["ts"] = 1755302400000
    with pytest.raises(ProtocolError):
        validate_envelope(message)


def test_bad_node_id_rejected():
    message = copy.deepcopy(VALID)
    message["node"] = "probe-LAB"
    with pytest.raises(ProtocolError):
        validate_envelope(message)


def test_extra_envelope_field_rejected():
    message = copy.deepcopy(VALID)
    message["extra"] = True
    with pytest.raises(ProtocolError):
        validate_envelope(message)


def test_non_object_rejected():
    with pytest.raises(ProtocolError):
        validate_envelope("not an object")


def test_payload_size_limit():
    validate_payload_size(b"x" * 1024)
    with pytest.raises(ProtocolError):
        validate_payload_size(b"x" * 1025)
