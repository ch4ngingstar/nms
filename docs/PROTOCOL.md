# Probe Protocol v1

The wire contract between probe nodes (ESP32 firmware, or the Python virtual probe) and the
command-and-control server, spoken entirely over MQTT. Two independent implementations —
`firmware/src/` in C++ and `probe/virtual_probe.py` in Python — satisfy this same contract;
`protocol/schemas/` is the normative source and `protocol/golden/` is the byte-identical fixture
corpus both are tested against. This document is the human-readable version of that contract.

## Encoding

JSON, UTF-8. Message rates are low (a chunk every few hundred milliseconds at most), so bandwidth
isn't the binding constraint — readability is, since `mosquitto_sub -t 'nms/v1/#' -v` showing
legible traffic is worth more than the bytes a binary encoding would save.

## Envelope

Every message carries the same outer fields:

```json
{
  "v": 1,
  "type": "result",
  "node": "probe-a4c1f8",
  "msg_id": "01J8X2K9QWER",
  "ts": 1755302400,
  "data": {}
}
```

| Field | Type | Rules |
| --- | --- | --- |
| `v` | integer | Protocol major version. Always `1`. |
| `type` | string | One of `cmd`, `result`, `monitor`, `status`, `telemetry`, `announce`. |
| `node` | string | The node this message concerns. Present on server→node messages too. |
| `msg_id` | string | 8–26 chars, unique per message. |
| `ts` | integer | Unix seconds, UTC — never milliseconds. |
| `data` | object | Type-specific payload. Always present, may be empty. |

A receiver must reject any message missing a required envelope field, carrying an unknown `type`,
or declaring a `v` it does not implement. A single published payload must not exceed **1024
bytes**; producers chunk to stay under it.

## Topics

Namespace: `nms/v1`. The `v1` segment is load-bearing — once boards are deployed, reflashing all
of them to change the wire format is impractical, so a future `v2` server would subscribe to both
trees during a transition.

| Topic | Direction | QoS | Retained | Purpose |
| --- | --- | --- | --- | --- |
| `nms/v1/announce` | node → server | 1 | no | Capability advertisement on connect |
| `nms/v1/node/<node_id>/cmd` | server → node | 1 | no | Command dispatch |
| `nms/v1/node/<node_id>/result` | node → server | 1 | no | Job result chunks and terminals |
| `nms/v1/node/<node_id>/monitor` | node → server | 1 | no | Scheduled monitoring cycle results |
| `nms/v1/node/<node_id>/status` | node → server | 1 | yes | Lifecycle state; carries the Last Will |
| `nms/v1/node/<node_id>/telemetry` | node → server | 0 | no | Periodic health, every 30s |

Monitoring results get their own topic rather than sharing `result`, so unsolicited periodic data
and solicited job output never need to be disambiguated by the consumer. `telemetry` is QoS 0
because a dropped sample is harmless; everything else needs to arrive, and duplicates from QoS-1
redelivery are safe because `job_id`/`msg_id` make handling idempotent.

The Last Will is registered on `status`, retained, with payload `{"state":"offline","reason":"lwt"}`.

## Node identity

`node_id` is derived from the device's factory MAC: `probe-` followed by the low three bytes in
lowercase hex (`probe-a4c1f8`), matching `^probe-(?:[0-9a-f]{6}|server)$`. It's immutable and is
the key used in every topic and every database row. The Python virtual probe uses the reserved id
`probe-server`.

A node may additionally carry a human-chosen `label`, set via the captive portal. The label is
metadata only — it appears in the UI and never in a topic, an ACL, or a foreign key, so relabeling
a probe is a zero-consequence operation.

## Node lifecycle

| State | Meaning |
| --- | --- |
| `unprovisioned` | No NVS config; the SoftAP portal is up, awaiting setup. |
| `connecting` | Config present; associating to WiFi, then dialing the broker. |
| `online` | Associated and MQTT-connected, idle, ready for commands. |
| `busy` | Executing a job. |
| `surveying` | Deliberately disconnected, radio in a non-STA mode for a job. |
| `offline` | Not connected — published by the broker via the Last Will. |

### Announce

On every successful MQTT connect, the node publishes to `nms/v1/announce`:

```json
{
  "v": 1, "type": "announce", "node": "probe-a4c1f8",
  "msg_id": "01J8X2K9QWER", "ts": 1755302400,
  "data": {
    "label": "Lab North",
    "fw": "1.2.0",
    "chip": "esp32",
    "mac": "a0:b7:65:a4:c1:f8",
    "free_heap": 214512,
    "capabilities": ["port_scan", "banner_grab", "dns", "trace", "discover", "wifi_survey"]
  }
}
```

The server upserts this into its node registry. `capabilities` enumerates **recon commands only**
— when one board runs different firmware than the others, the server dispatches only commands a
node declares support for. The control commands (`set_monitor`, `cancel`, `identify`, `reboot`,
`get_config`) are a mandatory baseline every node implements and are never listed; a node may
reject a recon command with `unsupported`, but never a control command. Unknown capability
strings are ignored, not errors.

### Status

```json
{"state": "online", "since": 1755302400, "job": null}
```

`job` carries the active `job_id` when `state` is `busy`, otherwise `null`.

### The survey/Last-Will interaction

Several commands (`wifi_survey`, `wifi_ids`, `wifi_deauth`) require leaving the access point,
which drops the MQTT connection — the exact condition that fires the Last Will. MQTT's own
semantics resolve this: the broker suppresses the Will on a clean disconnect. The required
sequence is:

1. Publish retained `status` with `{"state":"surveying","expect_back_in":<seconds>}`
2. Send a proper MQTT DISCONNECT
3. Leave the AP, do the radio work into a RAM buffer
4. Reassociate, reconnect, publish buffered results
5. Publish retained `status` `{"state":"online"}`

An ungraceful drop still fires the Will normally, so genuine failures stay detectable. Skipping
the clean-disconnect sequence produces false node-death alerts. Because the node holds results in
RAM until it reconnects, firmware must bound the buffer and apply the offline-buffering semantics
below rather than exhausting the heap.

### Telemetry

Published every 30 seconds at QoS 0:

```json
{"free_heap": 198320, "uptime_s": 84210, "rssi": -58,
 "channel": 6, "state": "online", "jobs_done": 412}
```

`free_heap` over time is the primary signal for firmware memory leaks.

## Commands

```json
{
  "v": 1, "type": "cmd", "node": "probe-a4c1f8",
  "msg_id": "01J8X2KA1234", "ts": 1755302400,
  "data": { "job_id": "job-7f3a91", "cmd": "port_scan", "args": {} }
}
```

`job_id` is server-generated and unique. A node receiving a `job_id` it has already seen must
ignore the duplicate rather than re-execute — this is what makes QoS-1 redelivery safe.

| `cmd` | `args` | Streams |
| --- | --- | --- |
| `port_scan` | `targets` (IP/CIDR list), `ports` (`"22,80,443"` or `"1-1024"`, freely mixed), `timeout_ms`, `concurrency` | yes |
| `banner_grab` | `target`, `ports` (list), `read_timeout_ms`, `max_bytes` | yes |
| `dns` | `name`, `qtype` (`A`\|`AAAA`\|`PTR`\|`MX`\|`TXT`\|`NS`), `resolver`, `timeout_ms` | yes |
| `trace` | `target`, `max_hops`, `per_hop_timeout_ms`, `probes_per_hop` | yes |
| `discover` | `subnet` (CIDR), `method` (`icmp`\|`arp`\|`tcp`), `timeout_ms` | yes |
| `wifi_survey` | `duration_s`, `channels` (list), `passive` (bool) | no — store-and-forward |
| `ble_scan` | `duration_s` (max 30), `active` (bool) | yes |
| `wifi_ids` | `duration_s`, `channels` (list), `known_aps` (list of `{bssid, ssid}`, max 32) | no — store-and-forward |
| `wifi_deauth` | `target_bssid` (required), `channel`, `duration_s`, `bursts`, `confirm` (required, must be `true`) | no — store-and-forward |
| `set_monitor` | `enabled`, `interval_s`, `devices` (list) | no |
| `cancel` | `job_id` | no |
| `identify` | `duration_s` | no |
| `reboot` | — | no |
| `get_config` | — | no |

`wifi_survey`, `wifi_ids`, and `wifi_deauth` can't stream because their radio work requires
leaving the AP (see the survey/Last-Will sequence above), but their results are still chunked
exactly like any other job: `accepted` is published before the disconnect, then the buffered
`chunk` messages and the terminal `done` are published after reassociating. The only difference is
*when* the chunks are published, not *how*. `wifi_deauth` additionally rejects the job unless
`confirm` is `true` — see [`docs/SECURITY.md`](SECURITY.md) for what that flag does and doesn't
guarantee.

`identify` blinks the onboard LED — with several boards placed around a building it's the only
practical way to determine which physical box is `probe-a4c1f8`.

**Argument conventions.** `ports` (on `port_scan`) is a single string holding a comma-separated
list whose elements are either individual ports or inclusive `low-high` ranges, freely mixed.
Ports are 1–65535; a range with `low > high` is `bad_args`. All `*_timeout_ms` values are integer
milliseconds. `targets` accepts both bare addresses and CIDR notation in the same list.

## Results

Every message on the `result` topic carries `type: "result"` in the envelope; `data.event`
discriminates which kind it is. Receivers must switch on `event` and reject a result message that
lacks it.

| `data.event` | Additional `data` fields |
| --- | --- |
| `accepted` | `job_id` |
| `chunk` | `job_id`, `seq`, one command-specific key (below) |
| `done` | `job_id`, `chunks`, `results`, `duration_ms` |
| `error` | `job_id`, `code`, `message` |

Each chunk carries `job_id`, `seq`, and a command-specific key:

| Command | Chunk key | Element |
| --- | --- | --- |
| `port_scan` | `open` | `{host, port, state, rtt_ms}` |
| `banner_grab` | `banners` | `{host, port, text, bytes, truncated}` |
| `dns` | `answers` | `{name, type, ttl, value}` |
| `trace` | `hops` | `{ttl, addr, rtt_ms, timeout}` |
| `discover` | `hosts` | `{ip, mac, rtt_ms, method}` |
| `wifi_survey` | `aps`, `clients` | `{bssid, ssid, channel, rssi, auth, hidden}` / `{mac, bssid, rssi}` |
| `ble_scan` | `devices` | `{mac, name, rssi, connectable, manufacturer}` |
| `wifi_ids` | `alerts`, then a final `frame_stats` | `{alert_type, source_mac, ...}` / `{total, management, data, control, deauth, probe_request}` |
| `wifi_deauth` | `deauth` (single object, not an array) | `{target_bssid, channel, clients_found, frames_sent, duration_ms}` |

Example:

```json
{"event": "chunk", "job_id": "job-7f3a91", "seq": 3,
 "open": [{"host":"192.168.1.10","port":22,"state":"open","rtt_ms":2.4}]}
```

## Job lifecycle

```
cmd ──► accepted ──► chunk(seq 0) ──► chunk(seq 1) ──► … ──► done
                └──────────────────────────────────────────► error
```

- **`accepted`** — sent before any work begins, as a distinct message from the first chunk, so a
  long-running or store-and-forward job is distinguishable from a node that never received the
  command.
- **Chunks** — `seq` starts at `0` and increments by exactly 1.
- **`done`** — terminal, carrying `{"job_id":…, "chunks": n, "results": n, "duration_ms": n}`.
- **`error`** — terminal, carrying `{"job_id":…, "code":…, "message":…}`.

Server-side, a job's `state` is one of `pending → accepted → {done | incomplete | error |
timed_out}`. The server detects gaps in `seq` and lands the job `incomplete` (with the missing
`seq` values recorded) rather than storing a truncated result set as if it succeeded. A job with
no chunk, `done`, or `error` before its deadline is marked `timed_out`; the deadline slides from
`last_event_at`, not job creation, and for store-and-forward commands it accounts for the
announced `expect_back_in`. `cancel` doesn't mark a job terminal by itself — the node answers with
an `error` event carrying code `cancelled`.

**Concurrency:** one job at a time per node, queue depth one. A command arriving while the node is
busy and the queue is full is rejected with `error` code `busy` — this is a hardware constraint
(limited heap for concurrent socket sets and JSON buffers), not a simplification. `cancel` and
`identify` are exempt and are handled immediately regardless of job state.

## Scheduled monitoring

`set_monitor` pushes a configuration the node persists to NVS and executes on its own timer,
independent of the job machinery — this means monitoring continues across server restarts:

```json
{"enabled": true, "interval_s": 5,
 "devices": [{"id": 1, "ip": "192.168.1.1", "checks": ["ping","port"], "ports": [22,53,80]}]}
```

Results publish to the `monitor` topic:

```json
{"cycle_ts": 1755302400,
 "results": [{"id":1,"status":"up","latency_ms":1.8,"ports":{"22":"open","53":"closed","80":"open"}}]}
```

`status` is one of `up`, `down`, or `unknown`; per-port values are `open`, `closed`, or
`filtered`; `latency_ms` is `null` when `status` is not `up`. The server derives alert state from
consecutive `down` cycles — the node reports observations only and holds no alerting logic.

## Error codes

| Code | Meaning |
| --- | --- |
| `busy` | A job is running and the queue is full. |
| `unsupported` | Command not in this node's `capabilities`. |
| `bad_args` | Arguments malformed or out of range. |
| `unreachable` | Target did not respond. |
| `timeout` | Operation exceeded its deadline. |
| `oom` | Insufficient heap to execute. |
| `cancelled` | Terminated by a `cancel` command. |
| `radio_conflict` | Requested operation conflicts with the current radio state. |

## Security

Each probe authenticates to the broker as itself, with credentials generated server-side and
entered via the captive portal during setup — never committed, never crossing the network in
plaintext during enrollment (TLS is deferred, so a token-based scheme would have the same
plaintext-crossing problem it's meant to solve). Broker ACLs scope each node strictly to its own
topics:

```
user probe-a4c1f8
topic write nms/v1/node/probe-a4c1f8/result
topic write nms/v1/node/probe-a4c1f8/monitor
topic write nms/v1/node/probe-a4c1f8/status
topic write nms/v1/node/probe-a4c1f8/telemetry
topic write nms/v1/announce
topic read  nms/v1/node/probe-a4c1f8/cmd
```

The server account holds read access across `nms/v1/#` and write access to
`nms/v1/node/+/cmd`. The resulting containment property: a compromised probe can neither issue
commands to the fleet nor read another probe's results. See
[`ARCHITECTURE.md`](../ARCHITECTURE.md#security-considerations) and
[`docs/SECURITY.md`](SECURITY.md) for the dual-use considerations around `wifi_deauth`
specifically — ACL scoping controls *which topics a node can touch*, not what a valid command
sent to it is allowed to do.

## Reliability

WiFi and MQTT reconnects use exponential backoff with jitter, 1s to a 60s ceiling — jitter is
required, not optional, because multiple probes returning after a broker restart would otherwise
retry in lockstep and stampede it.

Results produced while disconnected go into a bounded queue. On overflow the oldest entries are
dropped and the next published message sets `"dropped": <n>` — admitting loss is required,
silently truncating is not.

## Conformance

Three layers:

- **Schemas** (`protocol/schemas/`) — one JSON Schema per message type, normative. Prose is where
  ambiguity hides; a schema stating `ts` is an integer of Unix seconds leaves nothing to argue
  about.
- **Golden corpus** (`protocol/golden/`) — canonical `.json` fixtures both implementations test
  against byte-identically, covering the edge cases that actually bite: a `done` with zero
  results, a chunk carrying `dropped`, a sequence with a hole, and malformed messages that must be
  rejected.
- **Conformance suite** (`tests/test_conformance.py`) — scenario tests driven over a live MQTT
  broker. Because both the virtual probe and the firmware implement this protocol, one suite
  validates both: firmware that passes the suite the Python probe already passes is
  protocol-correct by construction.

Local Mosquitto (`docker compose up -d mosquitto`) is the only dependency; no hardware is required
to develop or run any of it.
