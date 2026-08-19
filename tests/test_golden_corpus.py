import json
from pathlib import Path

import pytest

from protocol.errors import ProtocolError
from protocol.validate import validate_message

GOLDEN = Path(__file__).parent.parent / "protocol" / "golden"
VALID = sorted((GOLDEN / "valid").glob("*.json"))
INVALID = sorted((GOLDEN / "invalid").glob("*.json"))


def test_corpus_is_populated():
    assert len(VALID) >= 8, "valid corpus is too small to be meaningful"
    assert len(INVALID) >= 6, "invalid corpus is too small to be meaningful"


@pytest.mark.parametrize("path", VALID, ids=lambda p: p.stem)
def test_valid_fixtures_accepted(path):
    validate_message(json.loads(path.read_text(encoding="utf-8")))


@pytest.mark.parametrize("path", INVALID, ids=lambda p: p.stem)
def test_invalid_fixtures_rejected(path):
    with pytest.raises(ProtocolError):
        validate_message(json.loads(path.read_text(encoding="utf-8")))


# The parametrized test above already validates these, but assert their presence
# explicitly so a deleted Phase-4 fixture fails loudly rather than silently
# shrinking the corpus (spec §6.4).
PHASE4_FIXTURES = [
    "cmd_ble_scan", "cmd_wifi_ids", "result_chunk_ble_scan",
    "result_chunk_ids_alert", "result_chunk_frame_stats",
]


@pytest.mark.parametrize("stem", PHASE4_FIXTURES)
def test_phase4_fixture_present_and_valid(stem):
    path = GOLDEN / "valid" / f"{stem}.json"
    assert path.is_file(), f"missing golden fixture {stem}"
    validate_message(json.loads(path.read_text(encoding="utf-8")))
