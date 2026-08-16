# Probe Protocol v1 Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Python `protocol` package that defines and enforces Probe Protocol v1 — JSON Schemas, message validation, topic construction, port parsing, job sequence tracking, a golden fixture corpus, and broker credential generation.

**Architecture:** A standalone `protocol/` package with no Flask dependency, so both the server (subsystem 2) and the conformance suite import it. JSON Schemas are the normative artifact; `validate.py` dispatches a message to its type schema, and for commands to a per-command args schema. A golden corpus of valid and must-reject fixtures is the language-neutral contract the C++ firmware will later be tested against.

**Tech Stack:** Python 3.12, `jsonschema` (Draft 2020-12), `pytest`. No broker, no hardware, no network required for any task here.

**Spec:** `docs/superpowers/specs/2026-08-16-probe-protocol-design.md`

**Out of scope (Plan 2):** the live-MQTT conformance runner (spec §9 layer 3), which requires Mosquitto running.

---

### Task 1: Repository and package scaffolding

This project is not currently a git repository, so the commit steps in every later task would fail. This task fixes that first.

**Files:**
- Create: `.gitignore`, `protocol/__init__.py`, `protocol/errors.py`, `tests/__init__.py`
- Modify: `requirements.txt`
- Create: `requirements-dev.txt`

- [ ] **Step 1: Initialise the repository**

```bash
cd "C:/Users/alityan/OneDrive/Desktop/nms"
git init
```

Expected: `Initialized empty Git repository in .../nms/.git/`

- [ ] **Step 2: Write `.gitignore`**

```
__pycache__/
*.py[cod]
.venv/
venv/
.pytest_cache/
instance/
*.db
```

Note: `*.db` leaves your existing `network_monitor.db` on disk but untracked. It is collected data, not source; a 610 KB binary does not belong in git history.

- [ ] **Step 3: Create the package directories and files**

`protocol/__init__.py`:

```python
"""Probe Protocol v1 — see docs/superpowers/specs/2026-08-16-probe-protocol-design.md."""

PROTOCOL_VERSION = 1
MAX_PAYLOAD_BYTES = 1024
```

`protocol/errors.py`:

```python
"""Protocol violation errors."""


class ProtocolError(ValueError):
    """Raised when a message or argument violates Probe Protocol v1."""
```

`tests/__init__.py`: empty file.

- [ ] **Step 4: Add dependencies**

Append to `requirements.txt`:

```
jsonschema
```

Create `requirements-dev.txt`:

```
-r requirements.txt
pytest
```

- [ ] **Step 5: Install and verify**

Run: `python -m pip install -r requirements-dev.txt`
Then: `python -m pytest tests/ -v`
Expected: `no tests ran` — exit code 5. This confirms pytest resolves the project root.

- [ ] **Step 6: Commit**

```bash
git add .gitignore requirements.txt requirements-dev.txt protocol/ tests/ docs/
git commit -m "chore: initialise repo and protocol package scaffolding"
```

---

### Task 2: Node IDs and topic construction

**Files:**
- Create: `protocol/topics.py`
- Test: `tests/test_topics.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_topics.py`:

```python
import pytest

from protocol.errors import ProtocolError
from protocol.topics import (
    ANNOUNCE_TOPIC,
    cmd_topic,
    monitor_topic,
    node_id_from_topic,
    result_topic,
    status_topic,
    telemetry_topic,
    validate_node_id,
)


@pytest.mark.parametrize("node_id", ["probe-a4c1f8", "probe-000000", "probe-server"])
def test_valid_node_ids_accepted(node_id):
    assert validate_node_id(node_id) == node_id


@pytest.mark.parametrize(
    "node_id",
    ["probe-A4C1F8", "probe-a4c1f", "probe-a4c1f88", "a4c1f8", "probe-", "probe-lab-north"],
)
def test_invalid_node_ids_rejected(node_id):
    with pytest.raises(ProtocolError):
        validate_node_id(node_id)


def test_topic_construction():
    assert cmd_topic("probe-a4c1f8") == "nms/v1/node/probe-a4c1f8/cmd"
    assert result_topic("probe-a4c1f8") == "nms/v1/node/probe-a4c1f8/result"
    assert monitor_topic("probe-a4c1f8") == "nms/v1/node/probe-a4c1f8/monitor"
    assert status_topic("probe-a4c1f8") == "nms/v1/node/probe-a4c1f8/status"
    assert telemetry_topic("probe-a4c1f8") == "nms/v1/node/probe-a4c1f8/telemetry"
    assert ANNOUNCE_TOPIC == "nms/v1/announce"


def test_topic_construction_validates_node_id():
    with pytest.raises(ProtocolError):
        cmd_topic("probe-NOPE")


def test_node_id_extracted_from_topic():
    assert node_id_from_topic("nms/v1/node/probe-a4c1f8/result") == "probe-a4c1f8"


@pytest.mark.parametrize(
    "topic",
    ["nms/v1/announce", "nms/v2/node/probe-a4c1f8/cmd", "node/probe-a4c1f8/cmd", ""],
)
def test_node_id_from_bad_topic_rejected(topic):
    with pytest.raises(ProtocolError):
        node_id_from_topic(topic)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_topics.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protocol.topics'`

- [ ] **Step 3: Write the implementation**

`protocol/topics.py`:

```python
"""Topic construction and node identity for Probe Protocol v1 (spec §4)."""

import re

from protocol.errors import ProtocolError

NAMESPACE = "nms/v1"
ANNOUNCE_TOPIC = f"{NAMESPACE}/announce"

NODE_ID_RE = re.compile(r"^probe-(?:[0-9a-f]{6}|server)$")
_TOPIC_RE = re.compile(rf"^{NAMESPACE}/node/(probe-(?:[0-9a-f]{{6}}|server))/[a-z]+$")


def validate_node_id(node_id: str) -> str:
    """Return node_id unchanged, or raise if it is not a valid identity."""
    if not isinstance(node_id, str) or not NODE_ID_RE.match(node_id):
        raise ProtocolError(
            f"invalid node_id {node_id!r}: expected 'probe-<6 lowercase hex>' or 'probe-server'"
        )
    return node_id


def _node_topic(node_id: str, leaf: str) -> str:
    return f"{NAMESPACE}/node/{validate_node_id(node_id)}/{leaf}"


def cmd_topic(node_id: str) -> str:
    return _node_topic(node_id, "cmd")


def result_topic(node_id: str) -> str:
    return _node_topic(node_id, "result")


def monitor_topic(node_id: str) -> str:
    return _node_topic(node_id, "monitor")


def status_topic(node_id: str) -> str:
    return _node_topic(node_id, "status")


def telemetry_topic(node_id: str) -> str:
    return _node_topic(node_id, "telemetry")


def node_id_from_topic(topic: str) -> str:
    """Extract the node_id from a per-node topic."""
    match = _TOPIC_RE.match(topic) if isinstance(topic, str) else None
    if not match:
        raise ProtocolError(f"cannot extract node_id from topic {topic!r}")
    return match.group(1)
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python -m pytest tests/test_topics.py -v`
Expected: PASS — 16 passed

- [ ] **Step 5: Commit**

```bash
git add protocol/topics.py tests/test_topics.py
git commit -m "feat(protocol): node id validation and topic construction"
```

---

### Task 3: Port specification parser

Spec §7.2 defines `ports` as a comma-separated string mixing individual ports and inclusive ranges.

**Files:**
- Create: `protocol/ports.py`
- Test: `tests/test_ports.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_ports.py`:

```python
import pytest

from protocol.errors import ProtocolError
from protocol.ports import parse_ports


def test_single_port():
    assert parse_ports("22") == [22]


def test_comma_separated_list():
    assert parse_ports("22,80,443") == [22, 80, 443]


def test_inclusive_range():
    assert parse_ports("20-23") == [20, 21, 22, 23]


def test_mixed_list_and_ranges():
    assert parse_ports("22,80,443,8000-8002") == [22, 80, 443, 8000, 8001, 8002]


def test_result_is_sorted_and_deduplicated():
    assert parse_ports("80,22,80,20-22") == [20, 21, 22, 80]


def test_surrounding_whitespace_tolerated():
    assert parse_ports(" 22 , 80 - 82 ") == [22, 80, 81, 82]


def test_boundary_ports_allowed():
    assert parse_ports("1,65535") == [1, 65535]


@pytest.mark.parametrize(
    "spec",
    [
        "",            # empty
        "   ",         # whitespace only
        "22,,80",      # empty element
        "0",           # below range
        "65536",       # above range
        "100-50",      # low > high
        "http",        # not numeric
        "22-",         # missing high
        "-80",         # missing low
        "1-2-3",       # malformed range
    ],
)
def test_invalid_specs_rejected(spec):
    with pytest.raises(ProtocolError):
        parse_ports(spec)


def test_non_string_rejected():
    with pytest.raises(ProtocolError):
        parse_ports([22, 80])
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_ports.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protocol.ports'`

- [ ] **Step 3: Write the implementation**

`protocol/ports.py`:

```python
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python -m pytest tests/test_ports.py -v`
Expected: PASS — 18 passed

- [ ] **Step 5: Commit**

```bash
git add protocol/ports.py tests/test_ports.py
git commit -m "feat(protocol): port specification parser"
```

---

### Task 4: Envelope schema and the validator core

**Files:**
- Create: `protocol/schemas/envelope.schema.json`, `protocol/validate.py`
- Test: `tests/test_validate_envelope.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_validate_envelope.py`:

```python
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_validate_envelope.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protocol.validate'`

- [ ] **Step 3: Write the schema**

`protocol/schemas/envelope.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "Probe Protocol v1 envelope",
  "type": "object",
  "required": ["v", "type", "node", "msg_id", "ts", "data"],
  "additionalProperties": false,
  "properties": {
    "v": { "const": 1 },
    "type": {
      "enum": ["cmd", "result", "monitor", "status", "telemetry", "announce"]
    },
    "node": { "type": "string", "pattern": "^probe-([0-9a-f]{6}|server)$" },
    "msg_id": { "type": "string", "minLength": 8, "maxLength": 26 },
    "ts": { "type": "integer", "minimum": 1000000000, "maximum": 4102444800 },
    "data": { "type": "object" }
  }
}
```

The `ts` ceiling of `4102444800` (year 2100) is deliberate: a millisecond timestamp is roughly `1.75e12` and therefore fails, catching the units bug the spec warns about in §5.2.

- [ ] **Step 4: Write the validator**

`protocol/validate.py`:

```python
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
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `python -m pytest tests/test_validate_envelope.py -v`
Expected: PASS — 14 passed

- [ ] **Step 6: Commit**

```bash
git add protocol/schemas/envelope.schema.json protocol/validate.py tests/test_validate_envelope.py
git commit -m "feat(protocol): envelope schema and validator core"
```

---

### Task 5: Lifecycle schemas — announce, status, telemetry

**Files:**
- Create: `protocol/schemas/announce.schema.json`, `protocol/schemas/status.schema.json`, `protocol/schemas/telemetry.schema.json`
- Modify: `protocol/validate.py`
- Test: `tests/test_validate_lifecycle.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_validate_lifecycle.py`:

```python
import pytest

from protocol.errors import ProtocolError
from protocol.validate import validate_message


def envelope(msg_type, data):
    return {
        "v": 1,
        "type": msg_type,
        "node": "probe-a4c1f8",
        "msg_id": "01J8X2K9QWER",
        "ts": 1755302400,
        "data": data,
    }


ANNOUNCE_DATA = {
    "label": "Lab North",
    "fw": "1.2.0",
    "chip": "esp32s3",
    "mac": "a0:b7:65:a4:c1:f8",
    "free_heap": 214512,
    "capabilities": ["port_scan", "wifi_survey"],
}


def test_valid_announce_accepted():
    validate_message(envelope("announce", ANNOUNCE_DATA))


def test_announce_label_is_optional():
    data = {k: v for k, v in ANNOUNCE_DATA.items() if k != "label"}
    validate_message(envelope("announce", data))


def test_announce_bad_mac_rejected():
    data = dict(ANNOUNCE_DATA, mac="A0-B7-65-A4-C1-F8")
    with pytest.raises(ProtocolError):
        validate_message(envelope("announce", data))


def test_announce_missing_capabilities_rejected():
    data = {k: v for k, v in ANNOUNCE_DATA.items() if k != "capabilities"}
    with pytest.raises(ProtocolError):
        validate_message(envelope("announce", data))


def test_valid_status_accepted():
    validate_message(envelope("status", {"state": "online", "since": 1755302400, "job": None}))


def test_status_busy_carries_job_id():
    validate_message(envelope("status", {"state": "busy", "job": "job-7f3a91"}))


def test_surveying_status_requires_expect_back_in():
    """Spec §6.4: an announced absence must state its expected duration."""
    with pytest.raises(ProtocolError):
        validate_message(envelope("status", {"state": "surveying"}))
    validate_message(envelope("status", {"state": "surveying", "expect_back_in": 30}))


def test_lwt_offline_payload_accepted():
    validate_message(envelope("status", {"state": "offline", "reason": "lwt"}))


def test_unknown_state_rejected():
    with pytest.raises(ProtocolError):
        validate_message(envelope("status", {"state": "asleep"}))


def test_valid_telemetry_accepted():
    validate_message(
        envelope(
            "telemetry",
            {"free_heap": 198320, "uptime_s": 84210, "rssi": -58,
             "channel": 6, "state": "online", "jobs_done": 412},
        )
    )


def test_telemetry_positive_rssi_rejected():
    with pytest.raises(ProtocolError):
        validate_message(envelope("telemetry", {"free_heap": 1, "uptime_s": 1,
                                                "state": "online", "rssi": 20}))
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_validate_lifecycle.py -v`
Expected: FAIL — `ImportError: cannot import name 'validate_message'`

- [ ] **Step 3: Write the three schemas**

`protocol/schemas/announce.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "announce data (spec §6.2)",
  "type": "object",
  "required": ["fw", "chip", "mac", "free_heap", "capabilities"],
  "additionalProperties": false,
  "properties": {
    "label": { "type": "string", "maxLength": 64 },
    "fw": { "type": "string", "pattern": "^[0-9]+\\.[0-9]+\\.[0-9]+$" },
    "chip": { "type": "string", "minLength": 1 },
    "mac": { "type": "string", "pattern": "^([0-9a-f]{2}:){5}[0-9a-f]{2}$" },
    "free_heap": { "type": "integer", "minimum": 0 },
    "capabilities": {
      "type": "array",
      "items": { "type": "string" },
      "uniqueItems": true
    }
  }
}
```

`protocol/schemas/status.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "status data (spec §6.3, §6.4)",
  "type": "object",
  "required": ["state"],
  "additionalProperties": false,
  "properties": {
    "state": {
      "enum": ["unprovisioned", "connecting", "online", "busy", "surveying", "offline"]
    },
    "since": { "type": "integer", "minimum": 0 },
    "job": { "type": ["string", "null"] },
    "expect_back_in": { "type": "integer", "minimum": 1 },
    "reason": { "type": "string" }
  },
  "if": {
    "properties": { "state": { "const": "surveying" } },
    "required": ["state"]
  },
  "then": { "required": ["state", "expect_back_in"] }
}
```

`protocol/schemas/telemetry.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "telemetry data (spec §6.5)",
  "type": "object",
  "required": ["free_heap", "uptime_s", "state"],
  "additionalProperties": false,
  "properties": {
    "free_heap": { "type": "integer", "minimum": 0 },
    "uptime_s": { "type": "integer", "minimum": 0 },
    "rssi": { "type": "integer", "minimum": -100, "maximum": 0 },
    "channel": { "type": "integer", "minimum": 1, "maximum": 14 },
    "state": {
      "enum": ["unprovisioned", "connecting", "online", "busy", "surveying", "offline"]
    },
    "jobs_done": { "type": "integer", "minimum": 0 }
  }
}
```

- [ ] **Step 4: Add `validate_message` to the validator**

Append to `protocol/validate.py`:

```python
def validate_message(message) -> dict:
    """Validate a full message: envelope, then its type-specific data."""
    validate_envelope(message)
    _check(_validator(message["type"]), message["data"], f"{message['type']} data")
    return message
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `python -m pytest tests/test_validate_lifecycle.py -v`
Expected: PASS — 11 passed

- [ ] **Step 6: Commit**

```bash
git add protocol/schemas/ protocol/validate.py tests/test_validate_lifecycle.py
git commit -m "feat(protocol): announce, status and telemetry schemas"
```

---

### Task 6: Command schema and per-command argument schemas

**Files:**
- Create: `protocol/schemas/cmd.schema.json`, `protocol/schemas/args/port_scan.schema.json`, `protocol/schemas/args/wifi_survey.schema.json`, `protocol/schemas/args/set_monitor.schema.json`
- Modify: `protocol/validate.py`
- Test: `tests/test_validate_cmd.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_validate_cmd.py`:

```python
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_validate_cmd.py -v`
Expected: FAIL — `ProtocolError: no schema for 'cmd'`

- [ ] **Step 3: Write the command schema**

`protocol/schemas/cmd.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "cmd data (spec §7.1, §7.2)",
  "type": "object",
  "required": ["job_id", "cmd"],
  "additionalProperties": false,
  "properties": {
    "job_id": { "type": "string", "minLength": 4, "maxLength": 32 },
    "cmd": {
      "enum": ["port_scan", "banner_grab", "dns", "trace", "discover",
               "wifi_survey", "set_monitor", "cancel", "identify",
               "reboot", "get_config"]
    },
    "args": { "type": "object" }
  }
}
```

- [ ] **Step 4: Write the argument schemas**

`protocol/schemas/args/port_scan.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "required": ["targets", "ports"],
  "additionalProperties": false,
  "properties": {
    "targets": {
      "type": "array", "minItems": 1, "items": { "type": "string", "minLength": 7 }
    },
    "ports": { "type": "string", "minLength": 1 },
    "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 },
    "concurrency": { "type": "integer", "minimum": 1, "maximum": 64 }
  }
}
```

`protocol/schemas/args/wifi_survey.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "required": ["duration_s"],
  "additionalProperties": false,
  "properties": {
    "duration_s": { "type": "integer", "minimum": 1, "maximum": 300 },
    "channels": {
      "type": "array",
      "items": { "type": "integer", "minimum": 1, "maximum": 14 },
      "uniqueItems": true
    },
    "passive": { "type": "boolean" }
  }
}
```

`protocol/schemas/args/set_monitor.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "required": ["enabled", "interval_s", "devices"],
  "additionalProperties": false,
  "properties": {
    "enabled": { "type": "boolean" },
    "interval_s": { "type": "integer", "minimum": 1, "maximum": 3600 },
    "devices": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["id", "ip", "checks"],
        "additionalProperties": false,
        "properties": {
          "id": { "type": "integer", "minimum": 1 },
          "ip": { "type": "string", "minLength": 7 },
          "checks": {
            "type": "array", "minItems": 1, "uniqueItems": true,
            "items": { "enum": ["ping", "port"] }
          },
          "ports": {
            "type": "array",
            "items": { "type": "integer", "minimum": 1, "maximum": 65535 }
          }
        }
      }
    }
  }
}
```

- [ ] **Step 5: Dispatch argument validation**

Replace `validate_message` in `protocol/validate.py` with:

```python
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
```

Add to the imports at the top of `protocol/validate.py`:

```python
from protocol.ports import parse_ports
```

Commands with no argument schema (`banner_grab`, `dns`, `trace`, `discover`, `cancel`, `identify`, `reboot`, `get_config`) accept any object for now; their schemas land in Plan 2 alongside the conformance runner.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `python -m pytest tests/test_validate_cmd.py -v`
Expected: PASS — 10 passed

- [ ] **Step 7: Commit**

```bash
git add protocol/schemas/ protocol/validate.py tests/test_validate_cmd.py
git commit -m "feat(protocol): command and argument schemas"
```

---

### Task 7: Result and monitor schemas

**Files:**
- Create: `protocol/schemas/result.schema.json`, `protocol/schemas/monitor.schema.json`
- Test: `tests/test_validate_result.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_validate_result.py`:

```python
import pytest

from protocol.errors import ProtocolError
from protocol.validate import validate_message


def result(data):
    return {"v": 1, "type": "result", "node": "probe-a4c1f8",
            "msg_id": "01J8X2KB5678", "ts": 1755302400, "data": data}


def monitor(data):
    return {"v": 1, "type": "monitor", "node": "probe-a4c1f8",
            "msg_id": "01J8X2KC9012", "ts": 1755302400, "data": data}


def test_accepted_event():
    validate_message(result({"event": "accepted", "job_id": "job-7f3a91"}))


def test_chunk_event():
    validate_message(result({
        "event": "chunk", "job_id": "job-7f3a91", "seq": 3,
        "open": [{"host": "192.168.1.10", "port": 22, "state": "open", "rtt_ms": 2.4}],
    }))


def test_chunk_without_seq_rejected():
    with pytest.raises(ProtocolError):
        validate_message(result({"event": "chunk", "job_id": "job-7f3a91",
                                 "open": []}))


def test_chunk_may_declare_dropped():
    validate_message(result({"event": "chunk", "job_id": "job-7f3a91",
                             "seq": 0, "hosts": [], "dropped": 12}))


def test_done_event_with_zero_results():
    validate_message(result({"event": "done", "job_id": "job-7f3a91",
                             "chunks": 0, "results": 0, "duration_ms": 812}))


def test_done_missing_summary_rejected():
    with pytest.raises(ProtocolError):
        validate_message(result({"event": "done", "job_id": "job-7f3a91"}))


def test_error_event():
    validate_message(result({"event": "error", "job_id": "job-7f3a91",
                             "code": "busy", "message": "a job is already running"}))


def test_error_with_unknown_code_rejected():
    with pytest.raises(ProtocolError):
        validate_message(result({"event": "error", "job_id": "job-7f3a91",
                                 "code": "gremlins", "message": "?"}))


def test_result_without_event_rejected():
    """Spec §7.3: event is the discriminator; a result without it is unreadable."""
    with pytest.raises(ProtocolError):
        validate_message(result({"job_id": "job-7f3a91", "seq": 0, "open": []}))


def test_wifi_survey_chunk():
    validate_message(result({
        "event": "chunk", "job_id": "job-7f3a91", "seq": 0,
        "aps": [{"bssid": "a0:b7:65:11:22:33", "ssid": "Home", "channel": 6,
                 "rssi": -61, "auth": "wpa2", "hidden": False}],
        "clients": [{"mac": "de:ad:be:ef:00:01", "bssid": "a0:b7:65:11:22:33",
                     "rssi": -70}],
    }))


def test_valid_monitor_cycle():
    validate_message(monitor({
        "cycle_ts": 1755302400,
        "results": [{"id": 1, "status": "up", "latency_ms": 1.8,
                     "ports": {"22": "open", "53": "closed", "80": "open"}}],
    }))


def test_monitor_down_host_has_null_latency():
    validate_message(monitor({"cycle_ts": 1755302400,
                              "results": [{"id": 2, "status": "down",
                                           "latency_ms": None}]}))


def test_monitor_down_host_with_latency_rejected():
    """Spec §7.6: latency_ms must be null unless status is up."""
    with pytest.raises(ProtocolError):
        validate_message(monitor({"cycle_ts": 1755302400,
                                  "results": [{"id": 2, "status": "down",
                                               "latency_ms": 5.0}]}))


def test_monitor_unknown_port_state_rejected():
    with pytest.raises(ProtocolError):
        validate_message(monitor({"cycle_ts": 1755302400,
                                  "results": [{"id": 1, "status": "up",
                                               "ports": {"22": "ajar"}}]}))
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_validate_result.py -v`
Expected: FAIL — `ProtocolError: no schema for 'result'`

- [ ] **Step 3: Write the result schema**

`protocol/schemas/result.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "result data (spec §7.3, §7.4)",
  "type": "object",
  "required": ["event", "job_id"],
  "additionalProperties": false,
  "properties": {
    "event": { "enum": ["accepted", "chunk", "done", "error"] },
    "job_id": { "type": "string", "minLength": 4, "maxLength": 32 },
    "seq": { "type": "integer", "minimum": 0 },
    "dropped": { "type": "integer", "minimum": 1 },
    "chunks": { "type": "integer", "minimum": 0 },
    "results": { "type": "integer", "minimum": 0 },
    "duration_ms": { "type": "integer", "minimum": 0 },
    "code": {
      "enum": ["busy", "unsupported", "bad_args", "unreachable",
               "timeout", "oom", "cancelled", "radio_conflict"]
    },
    "message": { "type": "string" },
    "open": { "type": "array", "items": { "type": "object" } },
    "banners": { "type": "array", "items": { "type": "object" } },
    "answers": { "type": "array", "items": { "type": "object" } },
    "hops": { "type": "array", "items": { "type": "object" } },
    "hosts": { "type": "array", "items": { "type": "object" } },
    "aps": { "type": "array", "items": { "type": "object" } },
    "clients": { "type": "array", "items": { "type": "object" } }
  },
  "allOf": [
    {
      "if": { "properties": { "event": { "const": "chunk" } }, "required": ["event"] },
      "then": { "required": ["event", "job_id", "seq"] }
    },
    {
      "if": { "properties": { "event": { "const": "done" } }, "required": ["event"] },
      "then": { "required": ["event", "job_id", "chunks", "results", "duration_ms"] }
    },
    {
      "if": { "properties": { "event": { "const": "error" } }, "required": ["event"] },
      "then": { "required": ["event", "job_id", "code", "message"] }
    }
  ]
}
```

- [ ] **Step 4: Write the monitor schema**

`protocol/schemas/monitor.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "monitor data (spec §7.6)",
  "type": "object",
  "required": ["cycle_ts", "results"],
  "additionalProperties": false,
  "properties": {
    "cycle_ts": { "type": "integer", "minimum": 1000000000, "maximum": 4102444800 },
    "dropped": { "type": "integer", "minimum": 1 },
    "results": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["id", "status"],
        "additionalProperties": false,
        "properties": {
          "id": { "type": "integer", "minimum": 1 },
          "status": { "enum": ["up", "down", "unknown"] },
          "latency_ms": { "type": ["number", "null"], "minimum": 0 },
          "ports": {
            "type": "object",
            "additionalProperties": { "enum": ["open", "closed", "filtered"] }
          }
        },
        "allOf": [
          {
            "if": {
              "properties": { "status": { "enum": ["down", "unknown"] } },
              "required": ["status"]
            },
            "then": { "properties": { "latency_ms": { "type": "null" } } }
          }
        ]
      }
    }
  }
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `python -m pytest tests/test_validate_result.py -v`
Expected: PASS — 14 passed

- [ ] **Step 6: Commit**

```bash
git add protocol/schemas/result.schema.json protocol/schemas/monitor.schema.json tests/test_validate_result.py
git commit -m "feat(protocol): result and monitor schemas"
```

---

### Task 8: Job sequence tracker

Spec §7.4 requires the server to detect gaps in `seq` and mark such a job `incomplete` rather than storing a truncated result set.

**Files:**
- Create: `protocol/job.py`
- Test: `tests/test_job.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_job.py`:

```python
import pytest

from protocol.errors import ProtocolError
from protocol.job import JobState, JobTracker


def test_new_job_is_pending():
    assert JobTracker("job-1").state is JobState.PENDING


def test_accept_moves_to_accepted():
    job = JobTracker("job-1")
    job.accept()
    assert job.state is JobState.ACCEPTED


def test_contiguous_chunks_then_done():
    job = JobTracker("job-1")
    job.accept()
    for seq in range(3):
        job.chunk(seq, {"open": [{"port": 22}]})
    job.finish(chunks=3, results=3, duration_ms=100)
    assert job.state is JobState.DONE
    assert job.gaps == []


def test_sequence_gap_marks_incomplete():
    job = JobTracker("job-1")
    job.accept()
    job.chunk(0, {})
    job.chunk(2, {})
    job.finish(chunks=2, results=2, duration_ms=100)
    assert job.state is JobState.INCOMPLETE
    assert job.gaps == [1]


def test_multiple_gaps_recorded():
    job = JobTracker("job-1")
    job.accept()
    job.chunk(0, {})
    job.chunk(4, {})
    job.finish(chunks=2, results=2, duration_ms=100)
    assert job.gaps == [1, 2, 3]


def test_duplicate_seq_ignored_not_counted_twice():
    job = JobTracker("job-1")
    job.accept()
    job.chunk(0, {})
    job.chunk(0, {})
    assert job.received == 1


def test_error_is_terminal():
    job = JobTracker("job-1")
    job.accept()
    job.fail("busy", "a job is already running")
    assert job.state is JobState.ERROR
    assert job.error_code == "busy"


def test_chunk_after_terminal_rejected():
    job = JobTracker("job-1")
    job.accept()
    job.finish(chunks=0, results=0, duration_ms=10)
    with pytest.raises(ProtocolError):
        job.chunk(0, {})


def test_timeout_is_terminal():
    job = JobTracker("job-1")
    job.accept()
    job.time_out()
    assert job.state is JobState.TIMED_OUT


def test_negative_seq_rejected():
    job = JobTracker("job-1")
    job.accept()
    with pytest.raises(ProtocolError):
        job.chunk(-1, {})
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_job.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protocol.job'`

- [ ] **Step 3: Write the implementation**

`protocol/job.py`:

```python
"""Server-side tracking of a single job's result stream (spec §7.4)."""

from enum import Enum

from protocol.errors import ProtocolError


class JobState(Enum):
    PENDING = "pending"
    ACCEPTED = "accepted"
    DONE = "done"
    INCOMPLETE = "incomplete"
    ERROR = "error"
    TIMED_OUT = "timed_out"


_TERMINAL = {JobState.DONE, JobState.INCOMPLETE, JobState.ERROR, JobState.TIMED_OUT}


class JobTracker:
    """Accumulates result events for one job and detects sequence gaps."""

    def __init__(self, job_id: str):
        self.job_id = job_id
        self.state = JobState.PENDING
        self.error_code: str | None = None
        self.summary: dict | None = None
        self._seen: set[int] = set()

    @property
    def received(self) -> int:
        return len(self._seen)

    @property
    def gaps(self) -> list[int]:
        """Sequence numbers missing below the highest one received."""
        if not self._seen:
            return []
        return [n for n in range(max(self._seen)) if n not in self._seen]

    def _guard_open(self) -> None:
        if self.state in _TERMINAL:
            raise ProtocolError(
                f"job {self.job_id} is terminal ({self.state.value}); no further events"
            )

    def accept(self) -> None:
        self._guard_open()
        self.state = JobState.ACCEPTED

    def chunk(self, seq: int, payload: dict) -> None:
        self._guard_open()
        if not isinstance(seq, int) or seq < 0:
            raise ProtocolError(f"invalid seq {seq!r}: expected a non-negative integer")
        self._seen.add(seq)

    def finish(self, chunks: int, results: int, duration_ms: int) -> None:
        self._guard_open()
        self.summary = {"chunks": chunks, "results": results, "duration_ms": duration_ms}
        self.state = JobState.INCOMPLETE if self.gaps else JobState.DONE

    def fail(self, code: str, message: str) -> None:
        self._guard_open()
        self.error_code = code
        self.summary = {"code": code, "message": message}
        self.state = JobState.ERROR

    def time_out(self) -> None:
        self._guard_open()
        self.state = JobState.TIMED_OUT
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python -m pytest tests/test_job.py -v`
Expected: PASS — 10 passed

- [ ] **Step 5: Commit**

```bash
git add protocol/job.py tests/test_job.py
git commit -m "feat(protocol): job sequence tracker with gap detection"
```

---

### Task 9: Golden fixture corpus

The language-neutral contract the C++ firmware will later be tested against (spec §9).

**Files:**
- Create: `protocol/golden/valid/*.json`, `protocol/golden/invalid/*.json`
- Test: `tests/test_golden_corpus.py`

- [ ] **Step 1: Write the failing test**

`tests/test_golden_corpus.py`:

```python
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
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python -m pytest tests/test_golden_corpus.py -v`
Expected: FAIL — `test_corpus_is_populated` asserts on an empty corpus

- [ ] **Step 3: Write the valid fixtures**

Each file below goes in `protocol/golden/valid/`.

`announce_full.json`:

```json
{"v":1,"type":"announce","node":"probe-a4c1f8","msg_id":"01J8X2K9QWER","ts":1755302400,
 "data":{"label":"Lab North","fw":"1.2.0","chip":"esp32s3","mac":"a0:b7:65:a4:c1:f8",
 "free_heap":214512,"capabilities":["port_scan","banner_grab","dns","trace","discover","wifi_survey"]}}
```

`status_online.json`:

```json
{"v":1,"type":"status","node":"probe-a4c1f8","msg_id":"01J8X2K9QWES","ts":1755302400,
 "data":{"state":"online","since":1755302400,"job":null}}
```

`status_surveying.json`:

```json
{"v":1,"type":"status","node":"probe-a4c1f8","msg_id":"01J8X2K9QWET","ts":1755302400,
 "data":{"state":"surveying","expect_back_in":30}}
```

`status_offline_lwt.json`:

```json
{"v":1,"type":"status","node":"probe-a4c1f8","msg_id":"01J8X2K9QWEU","ts":1755302400,
 "data":{"state":"offline","reason":"lwt"}}
```

`telemetry_basic.json`:

```json
{"v":1,"type":"telemetry","node":"probe-a4c1f8","msg_id":"01J8X2K9QWEV","ts":1755302400,
 "data":{"free_heap":198320,"uptime_s":84210,"rssi":-58,"channel":6,"state":"online","jobs_done":412}}
```

`cmd_port_scan.json`:

```json
{"v":1,"type":"cmd","node":"probe-a4c1f8","msg_id":"01J8X2KA1234","ts":1755302400,
 "data":{"job_id":"job-7f3a91","cmd":"port_scan",
 "args":{"targets":["192.168.1.0/24"],"ports":"22,80,443,8000-8100","timeout_ms":500,"concurrency":8}}}
```

`result_accepted.json`:

```json
{"v":1,"type":"result","node":"probe-a4c1f8","msg_id":"01J8X2KB0001","ts":1755302400,
 "data":{"event":"accepted","job_id":"job-7f3a91"}}
```

`result_chunk_port_scan.json`:

```json
{"v":1,"type":"result","node":"probe-a4c1f8","msg_id":"01J8X2KB0002","ts":1755302401,
 "data":{"event":"chunk","job_id":"job-7f3a91","seq":3,
 "open":[{"host":"192.168.1.10","port":22,"state":"open","rtt_ms":2.4}]}}
```

`result_chunk_dropped.json`:

```json
{"v":1,"type":"result","node":"probe-a4c1f8","msg_id":"01J8X2KB0003","ts":1755302402,
 "data":{"event":"chunk","job_id":"job-7f3a91","seq":4,"hosts":[],"dropped":12}}
```

`result_done_zero_results.json`:

```json
{"v":1,"type":"result","node":"probe-a4c1f8","msg_id":"01J8X2KB0004","ts":1755302403,
 "data":{"event":"done","job_id":"job-7f3a91","chunks":0,"results":0,"duration_ms":812}}
```

`result_error_busy.json`:

```json
{"v":1,"type":"result","node":"probe-a4c1f8","msg_id":"01J8X2KB0005","ts":1755302404,
 "data":{"event":"error","job_id":"job-7f3a92","code":"busy","message":"a job is already running"}}
```

`result_chunk_wifi_survey.json`:

```json
{"v":1,"type":"result","node":"probe-a4c1f8","msg_id":"01J8X2KB0006","ts":1755302460,
 "data":{"event":"chunk","job_id":"job-7f3a93","seq":0,
 "aps":[{"bssid":"a0:b7:65:11:22:33","ssid":"Home","channel":6,"rssi":-61,"auth":"wpa2","hidden":false}],
 "clients":[{"mac":"de:ad:be:ef:00:01","bssid":"a0:b7:65:11:22:33","rssi":-70}]}}
```

`monitor_cycle.json`:

```json
{"v":1,"type":"monitor","node":"probe-server","msg_id":"01J8X2KC9012","ts":1755302400,
 "data":{"cycle_ts":1755302400,
 "results":[{"id":1,"status":"up","latency_ms":1.8,"ports":{"22":"open","53":"closed","80":"open"}},
            {"id":2,"status":"down","latency_ms":null}]}}
```

- [ ] **Step 4: Write the must-reject fixtures**

Each file below goes in `protocol/golden/invalid/`. The filename states the violation.

`missing_envelope_ts.json`:

```json
{"v":1,"type":"telemetry","node":"probe-a4c1f8","msg_id":"01J8X2K9QWER",
 "data":{"free_heap":1,"uptime_s":1,"state":"online"}}
```

`ts_in_milliseconds.json`:

```json
{"v":1,"type":"telemetry","node":"probe-a4c1f8","msg_id":"01J8X2K9QWER","ts":1755302400000,
 "data":{"free_heap":1,"uptime_s":1,"state":"online"}}
```

`unknown_message_type.json`:

```json
{"v":1,"type":"gossip","node":"probe-a4c1f8","msg_id":"01J8X2K9QWER","ts":1755302400,"data":{}}
```

`wrong_protocol_version.json`:

```json
{"v":2,"type":"telemetry","node":"probe-a4c1f8","msg_id":"01J8X2K9QWER","ts":1755302400,
 "data":{"free_heap":1,"uptime_s":1,"state":"online"}}
```

`bad_node_id_uppercase.json`:

```json
{"v":1,"type":"telemetry","node":"probe-A4C1F8","msg_id":"01J8X2K9QWER","ts":1755302400,
 "data":{"free_heap":1,"uptime_s":1,"state":"online"}}
```

`result_missing_event.json`:

```json
{"v":1,"type":"result","node":"probe-a4c1f8","msg_id":"01J8X2KB0002","ts":1755302401,
 "data":{"job_id":"job-7f3a91","seq":3,"open":[]}}
```

`result_chunk_missing_seq.json`:

```json
{"v":1,"type":"result","node":"probe-a4c1f8","msg_id":"01J8X2KB0002","ts":1755302401,
 "data":{"event":"chunk","job_id":"job-7f3a91","open":[]}}
```

`surveying_without_expect_back_in.json`:

```json
{"v":1,"type":"status","node":"probe-a4c1f8","msg_id":"01J8X2K9QWET","ts":1755302400,
 "data":{"state":"surveying"}}
```

`port_scan_inverted_range.json`:

```json
{"v":1,"type":"cmd","node":"probe-a4c1f8","msg_id":"01J8X2KA1234","ts":1755302400,
 "data":{"job_id":"job-7f3a91","cmd":"port_scan","args":{"targets":["10.0.0.5"],"ports":"100-50"}}}
```

`monitor_down_host_with_latency.json`:

```json
{"v":1,"type":"monitor","node":"probe-server","msg_id":"01J8X2KC9012","ts":1755302400,
 "data":{"cycle_ts":1755302400,"results":[{"id":2,"status":"down","latency_ms":5.0}]}}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `python -m pytest tests/test_golden_corpus.py -v`
Expected: PASS — 24 passed (1 population check, 13 valid fixtures, 10 must-reject fixtures)

- [ ] **Step 6: Run the whole suite**

Run: `python -m pytest tests/ -v`
Expected: PASS — all tests green

- [ ] **Step 7: Commit**

```bash
git add protocol/golden/ tests/test_golden_corpus.py
git commit -m "feat(protocol): golden fixture corpus"
```

---

### Task 10: Broker credential and ACL generation

Spec §8.1 and §8.2: adding a node produces a username/password pair and the matching Mosquitto ACL block.

**Files:**
- Create: `protocol/credentials.py`, `scripts/gen_node_credentials.py`
- Test: `tests/test_credentials.py`

- [ ] **Step 1: Write the failing tests**

`tests/test_credentials.py`:

```python
import pytest

from protocol.credentials import acl_block, generate_password, server_acl_block
from protocol.errors import ProtocolError


def test_password_is_long_enough():
    assert len(generate_password()) >= 24


def test_passwords_are_unique():
    assert len({generate_password() for _ in range(100)}) == 100


def test_password_is_url_safe():
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
    assert set(generate_password()) <= allowed


def test_acl_block_confines_node_to_own_topics():
    block = acl_block("probe-a4c1f8")
    assert "user probe-a4c1f8" in block
    assert "topic write nms/v1/node/probe-a4c1f8/result" in block
    assert "topic write nms/v1/node/probe-a4c1f8/monitor" in block
    assert "topic write nms/v1/node/probe-a4c1f8/status" in block
    assert "topic write nms/v1/node/probe-a4c1f8/telemetry" in block
    assert "topic write nms/v1/announce" in block
    assert "topic read nms/v1/node/probe-a4c1f8/cmd" in block


def test_acl_block_grants_no_access_to_other_nodes():
    block = acl_block("probe-a4c1f8")
    assert "probe-7e2b10" not in block
    assert "nms/v1/#" not in block
    assert "node/+/" not in block


def test_acl_block_validates_node_id():
    with pytest.raises(ProtocolError):
        acl_block("probe-NOPE")


def test_server_acl_block_reads_all_and_writes_commands():
    block = server_acl_block("nms-server")
    assert "user nms-server" in block
    assert "topic read nms/v1/#" in block
    assert "topic write nms/v1/node/+/cmd" in block
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python -m pytest tests/test_credentials.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protocol.credentials'`

- [ ] **Step 3: Write the implementation**

`protocol/credentials.py`:

```python
"""Broker credential and ACL generation (spec §8.1, §8.2)."""

import secrets

from protocol.topics import (
    ANNOUNCE_TOPIC,
    NAMESPACE,
    cmd_topic,
    monitor_topic,
    result_topic,
    status_topic,
    telemetry_topic,
    validate_node_id,
)

PASSWORD_BYTES = 24


def generate_password() -> str:
    """Return a fresh URL-safe secret for one node."""
    return secrets.token_urlsafe(PASSWORD_BYTES)


def acl_block(node_id: str) -> str:
    """Mosquitto ACL confining one probe to its own topics."""
    validate_node_id(node_id)
    lines = [f"user {node_id}"]
    lines += [
        f"topic write {topic(node_id)}"
        for topic in (result_topic, monitor_topic, status_topic, telemetry_topic)
    ]
    lines.append(f"topic write {ANNOUNCE_TOPIC}")
    lines.append(f"topic read {cmd_topic(node_id)}")
    return "\n".join(lines) + "\n"


def server_acl_block(username: str) -> str:
    """Mosquitto ACL for the command-and-control server account."""
    if not username or not username.isascii():
        raise ValueError("username must be a non-empty ASCII string")
    return (
        f"user {username}\n"
        f"topic read {NAMESPACE}/#\n"
        f"topic write {NAMESPACE}/node/+/cmd\n"
    )
```

- [ ] **Step 4: Write the CLI script**

`scripts/gen_node_credentials.py`:

```python
"""Print broker credentials and an ACL block for one probe node.

Usage: python scripts/gen_node_credentials.py probe-a4c1f8
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from protocol.credentials import acl_block, generate_password  # noqa: E402
from protocol.errors import ProtocolError  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    node_id = argv[1]
    try:
        block = acl_block(node_id)
    except ProtocolError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    password = generate_password()
    print(f"# credentials for {node_id} — enter these in the captive portal")
    print(f"username: {node_id}")
    print(f"password: {password}")
    print()
    print(f"# add to the Mosquitto password file:")
    print(f"mosquitto_passwd -b /etc/mosquitto/passwd {node_id} {password}")
    print()
    print("# append to the Mosquitto ACL file:")
    print(block, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `python -m pytest tests/test_credentials.py -v`
Expected: PASS — 7 passed

- [ ] **Step 6: Verify the script end to end**

Run: `python scripts/gen_node_credentials.py probe-a4c1f8`
Expected: username, a fresh password, a `mosquitto_passwd` line, and the ACL block.

Run: `python scripts/gen_node_credentials.py probe-NOPE`
Expected: `error: invalid node_id 'probe-NOPE'...` and exit code 1.

- [ ] **Step 7: Run the full suite and commit**

```bash
python -m pytest tests/ -v
git add protocol/credentials.py scripts/gen_node_credentials.py tests/test_credentials.py
git commit -m "feat(protocol): broker credential and ACL generation"
```

---

## Definition of done

- [ ] `python -m pytest tests/ -v` passes with every test green
- [ ] `protocol/` imports cleanly with no dependency on Flask
- [ ] Every schema in `protocol/schemas/` has at least one valid and one must-reject golden fixture
- [ ] `python scripts/gen_node_credentials.py probe-a4c1f8` produces usable output

## Spec coverage

| Spec section | Covered by |
|---|---|
| §4.1–4.2 node identity | Task 2 |
| §4.3 topics | Task 2 |
| §4.4 QoS and retention | Plan 2 (a broker-side concern) |
| §5.1–5.2 encoding and envelope | Task 4 |
| §5.3 size limit | Task 4 (`validate_payload_size`) |
| §6.1–6.3 states, announce, status | Task 5 |
| §6.4 survey/LWT interaction | Task 5 (`expect_back_in` conditional) |
| §6.5 telemetry | Task 5 |
| §7.1–7.2 command message and reference | Task 6 |
| §7.3 result events and chunk shapes | Task 7 |
| §7.4 job lifecycle and gap detection | Tasks 7 and 8 |
| §7.5 concurrency (`busy`) | Task 7 (error code) |
| §7.6 scheduled monitoring | Tasks 6 and 7 |
| §8.1–8.2 credentials and ACLs | Task 10 |
| §8.3 error codes | Task 7 |
| §8.4–8.5 backoff and buffering | Task 7 (`dropped`); behaviour lands in Plan 2 |
| §9 schemas and golden corpus | Tasks 4–7, 9 |
| §9 conformance suite | Plan 2 |
