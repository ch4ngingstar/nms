# Probe Protocol v1 — Design Spec

**Subsystem 1 of 4** · Status: **approved, ready for implementation planning** · Date: 2026-08-16

## 1. Scope

This document specifies the wire protocol between **probe nodes** and the **command-and-control server** in the NMS distributed network-recon platform. It is the contract that two independent implementations must satisfy:

- **Firmware probe** — ESP32-S3, C++/PlatformIO (subsystem 3)
- **Virtual probe** — Python, running in-process with the Flask server (subsystem 2)

This spec defines message formats, topics, lifecycle, and error semantics. It does **not** specify server storage, UI, or firmware internals; those belong to subsystems 2, 3, and 4.

### Why this is specified first

Both implementations depend on it. Defining the contract before either side exists prevents the C++ and Python from drifting into mutually incompatible dialects, which would turn every integration bug into a wire-format archaeology session.

## 2. Context

The platform deploys three or more ESP32-S3 probe nodes around a network. Each probe performs reconnaissance — TCP/IP scanning plus 802.11 RF survey — and reports to a central Flask server that dispatches commands, aggregates results, and serves a console and dashboard.

The existing `app.py` ping poller and `check_port`/`check_service_ports` functions are **not discarded**. They are wrapped as the virtual probe, which speaks this same protocol and registers like any hardware node. This yields one `Probe` interface with two implementations and keeps the system demonstrable when no hardware is powered on.

### 2.1 Relationship to prior art

**ESP32-Bit-Pirate** (github.com/geo-tp/ESP32-Bit-Pirate, MIT) is the acknowledged design inspiration. It is ESP32-S3 firmware implementing ~24 protocol modes (I2C, SPI, UART, JTAG, CAN, Sub-GHz, RFID, LoRa, and others) behind a unified console available as a web CLI, a serial CLI, and a standalone on-device mode.

Its Wi-Fi mode already performs scanning, sniffing, `nmap`-style port scanning, and `netcat`. **An ESP32 performing network reconnaissance from a web console is therefore not novel**, and this project does not claim it as a contribution.

What this project contributes is the distributed system built around that capability:

| ESP32-Bit-Pirate | This project |
|---|---|
| One handheld device, one operator, one location | Fleet of 3+ permanently deployed probes |
| No server, no persistence | Central C2 server, node registry, time-series history |
| Interactive one-shot tool use | Node-side scheduled monitoring surviving server restarts |
| No inter-device protocol | This specification, with schemas, golden corpus, conformance suite |
| Single implementation | Two implementations of one contract, validated by one shared suite |
| Single vantage point | Multi-vantage RF correlation (same AP, differing RSSI per probe) |

**Borrowed:** the console-plus-modes interaction pattern, web CLI over Wi-Fi, and the ESP32-S3 / PlatformIO / C++ platform choice.

**Deliberately not adopted:** the 24 hardware protocol modes (out of scope).

**Extended capabilities (spec revision 2026-08-17):** three additional capabilities — `wifi_deauth`, `ble_scan`, and `wifi_ids` — extend the recon-only baseline. `wifi_deauth` uses `esp_wifi_80211_tx()` for controlled deauthentication testing, gated by a mandatory `confirm: true` argument. `wifi_ids` uses promiscuous-mode frame classification for intrusion detection (deauth flood detection, rogue AP detection, evil twin detection) — passive and read-only. `ble_scan` uses the ESP32 BLE radio for device enumeration. All three are dispatched exclusively through the C2 server's command pipeline and follow the same radio disconnect sequence as `wifi_survey`.

**Attribution obligation.** Bit-Pirate's source may be read as a reference for ESP-IDF radio patterns (`esp_wifi_scan`, promiscuous-mode callbacks), but all firmware in this project is written independently. The project must be cited as inspiration and as a consulted reference in the final report, and in a source-tree `NOTICE` or `CREDITS` file.

## 3. Locked decisions and rationale

| Decision | Choice | Rationale |
|---|---|---|
| ESP32 role | The star — embedded-heavy | Probes do the real recon work; server is C2 + aggregation. Reads as computer engineering, not a web project. |
| Radio | Station **and** monitor mode | `esp_wifi_set_promiscuous` needs no privileges. RF survey collects data no host-side script can. Requires a radio state scheduler. |
| Fleet size | Multi-node, 3+ | One board on hand today; design for a fleet, sequence the build so one board is fully functional. |
| Transport | MQTT via Mosquitto | Pub/sub natural for N probes; Last Will gives node-death detection free; QoS handles flaky Wi-Fi. |
| Server role | Virtual probe node | Uniform node abstraction; demo survives dead hardware; reuses working code. |
| Provisioning | SoftAP captive portal | Avoids reflashing 3+ boards per network change; credentials stay out of the source tree. |
| Result delivery | Streamed chunks with job ID | Long scans, limited ESP32 RAM, live console UX. |
| C2 security | Per-node credentials + topic ACLs | Contains a compromised probe. TLS deferred — mbedTLS RAM contends with promiscuous-mode buffers. |
| Monitoring schedule | Node-side | Probe keeps working across server restarts; drastically less MQTT chatter. |
| Node naming | MAC-derived id + optional label | Identity separated from description; relabeling never breaks topics, ACLs, or history. |

## 4. Identity and addressing

### 4.1 Node identity

`node_id` is derived from the ESP32 factory MAC address: the literal `probe-` followed by the low three bytes in lowercase hex.

```
probe-a4c1f8
```

Properties: stable across reflashes, unique without central allocation, short enough to write on a physical label. It is **immutable** and is the key used in every topic and every database row.

The Python virtual probe uses the reserved id **`probe-server`**.

### 4.2 Friendly label

A node may additionally carry a human-chosen `label` (e.g. `"Lab North"`), set via the captive portal and stored in NVS. The label is **metadata only** — it appears in the UI and never in a topic, an ACL, or a foreign key. Relabeling a probe is therefore a zero-consequence operation.

### 4.3 Topics

All topics are namespaced and versioned:

| Topic | Direction | Purpose |
|---|---|---|
| `nms/v1/node/<node_id>/cmd` | server → node | Commands |
| `nms/v1/node/<node_id>/result` | node → server | Job result chunks and terminals |
| `nms/v1/node/<node_id>/monitor` | node → server | Scheduled monitoring results |
| `nms/v1/node/<node_id>/status` | node → server | Lifecycle state (**retained**) |
| `nms/v1/node/<node_id>/telemetry` | node → server | Periodic health |
| `nms/v1/announce` | node → server | Registration on connect |

The `v1` segment is load-bearing. Once boards are deployed around a building, reflashing all of them to change the wire format is impractical; a future `v2` server can subscribe to both trees and support deployed `v1` nodes through a transition.

Monitoring results occupy their own topic rather than sharing `result`, so that unsolicited periodic data and solicited job output never have to be disambiguated by the consumer.

### 4.4 QoS and retention

| Topic | QoS | Retained | Rationale |
|---|---|---|---|
| `cmd` | 1 | no | Must arrive. Duplicates are safe — `job_id` makes execution idempotent. |
| `result` | 1 | no | A lost chunk silently corrupts a scan. |
| `monitor` | 1 | no | Feeds stored history; gaps are visible as data loss. |
| `status` | 1 | **yes** | A restarting server learns every node's state on subscribe, with no timeout bookkeeping. |
| `telemetry` | 0 | no | Periodic health; a dropped sample is harmless and QoS 0 is free. |

The **Last Will** is registered on `status`, retained, with payload `{"state":"offline","reason":"lwt"}`.

## 5. Message format

### 5.1 Encoding

**JSON**, UTF-8. Binary encodings would be smaller, but message rates here are low (a chunk every few hundred milliseconds), so bandwidth is not the binding constraint. Readability is: two implementations in two languages are being developed in parallel, and `mosquitto_sub -t 'nms/v1/#' -v` showing legible traffic is worth more than the bytes saved. ArduinoJson is mature on ESP32.

### 5.2 Envelope

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
|---|---|---|
| `v` | integer | Protocol major version. Always `1` for this spec. |
| `type` | string | One of `cmd`, `result`, `monitor`, `status`, `telemetry`, `announce`. |
| `node` | string | The node this message concerns. Present on server→node messages too. |
| `msg_id` | string | 8–26 chars, unique per message. ULID recommended. |
| `ts` | integer | **Unix seconds, UTC.** Not milliseconds. |
| `data` | object | Type-specific payload. Always present, may be empty. |

A receiver **must reject** any message missing a required envelope field, carrying an unknown `type`, or declaring a `v` it does not implement.

### 5.3 Size limit

A single published message payload **must not exceed 1024 bytes**. Producers chunk to stay within it. This keeps messages comfortably inside both Mosquitto's defaults and the ESP32's TCP buffers.

## 6. Node lifecycle

### 6.1 States

| State | Meaning |
|---|---|
| `unprovisioned` | No NVS config. SoftAP portal is up, awaiting setup. |
| `connecting` | Config present; associating to Wi-Fi, then dialing the broker. |
| `online` | Associated and MQTT-connected. Idle, ready for commands. |
| `busy` | Executing a job. |
| `surveying` | Deliberately disconnected, in promiscuous mode. |
| `offline` | Not connected. Published by the broker via Last Will. |

### 6.2 Registration

On every successful MQTT connect, the node publishes to `nms/v1/announce`:

```json
{
  "v": 1, "type": "announce", "node": "probe-a4c1f8",
  "msg_id": "01J8X2K9QWER", "ts": 1755302400,
  "data": {
    "label": "Lab North",
    "fw": "1.2.0",
    "chip": "esp32s3",
    "mac": "a0:b7:65:a4:c1:f8",
    "free_heap": 214512,
    "capabilities": ["port_scan","banner_grab","dns","trace","discover","wifi_survey"]
  }
}
```

The server upserts this into its node registry. The `capabilities` array is required: when one board runs newer firmware than the others, the server dispatches only commands a node declares support for, and the UI disables the rest per node. Unknown capability strings are ignored, not errors.

`capabilities` enumerates **recon commands only**. The control commands — `set_monitor`, `cancel`, `identify`, `reboot`, `get_config` — are a mandatory baseline that every conforming v1 node implements, and are never listed. A node may therefore reject a recon command with `unsupported`, but never a control command.

### 6.3 Status

```json
{"state": "online", "since": 1755302400, "job": null}
```

`job` carries the active `job_id` when `state` is `busy`, otherwise `null`.

### 6.4 The survey/Last-Will interaction

Entering promiscuous mode requires leaving the access point, which drops the MQTT connection — the exact condition that fires the Last Will. Without care, a node doing precisely what it was told would appear to die on every survey.

MQTT's own semantics resolve this: **the broker suppresses the Will on a clean disconnect.** The required sequence is:

1. Publish retained `status` with `{"state":"surveying","expect_back_in":<seconds>}`
2. Send a proper MQTT DISCONNECT
3. Leave the AP, enter promiscuous mode, survey into a RAM buffer
4. Reassociate, reconnect, publish buffered results
5. Publish retained `status` `{"state":"online"}`

The server therefore observes an *announced, bounded* absence. An ungraceful drop still fires the Will normally, so genuine failures remain detectable. Implementations **must** perform the clean disconnect; skipping it produces false node-death alerts.

### 6.5 Telemetry

Published every 30 seconds at QoS 0:

```json
{"free_heap": 198320, "uptime_s": 84210, "rssi": -58,
 "channel": 6, "state": "online", "jobs_done": 412}
```

`free_heap` over time is the primary signal for firmware memory leaks.

## 7. Commands and jobs

### 7.1 Command message

```json
{
  "v": 1, "type": "cmd", "node": "probe-a4c1f8",
  "msg_id": "01J8X2KA1234", "ts": 1755302400,
  "data": { "job_id": "job-7f3a91", "cmd": "port_scan", "args": {} }
}
```

`job_id` is **server-generated** and unique. A node receiving a `job_id` it has already seen **must** ignore the duplicate rather than re-execute — this is what makes QoS 1 redelivery safe.

### 7.2 Command reference

| `cmd` | `args` | Streams |
|---|---|---|
| `port_scan` | `targets` (list of IP/CIDR), `ports` (`"22,80,443"` or `"1-1024"`), `timeout_ms`, `concurrency` | yes |
| `banner_grab` | `target`, `ports` (list), `read_timeout_ms`, `max_bytes` | yes |
| `dns` | `name`, `qtype` (`A`\|`AAAA`\|`PTR`\|`MX`\|`TXT`\|`NS`), `resolver`, `timeout_ms` | yes |
| `trace` | `target`, `max_hops`, `per_hop_timeout_ms`, `probes_per_hop` | yes |
| `discover` | `subnet` (CIDR), `method` (`icmp`\|`arp`\|`tcp`), `timeout_ms` | yes |
| `wifi_survey` | `duration_s`, `channels` (list), `passive` (bool) | **no — burst** |
| `set_monitor` | `enabled`, `interval_s`, `devices` (list) | no |
| `cancel` | `job_id` | no |
| `identify` | `duration_s` | no |
| `reboot` | — | no |
| `get_config` | — | no |

`identify` blinks the onboard LED. With three boards placed around a building it is the only practical way to determine which physical box is `probe-a4c1f8`.

**`wifi_survey` is store-and-forward, not single-message.** It cannot stream because promiscuous mode requires leaving the AP (§6.4), but its results are still chunked exactly like any other job — `accepted` is published *before* the disconnect, then the buffered `chunk` messages and the terminal `done` are published after reassociating. A dense RF environment produces far more than 1024 bytes of access points and clients, so the `seq` sequence and the size cap apply unchanged. The only difference is *when* the chunks are published, not *how*.

Because the node holds the entire survey in RAM until it reconnects, firmware **must** bound the buffer and apply §8.5 overflow semantics rather than exhausting the heap.

**Argument conventions.** `ports` is a single string holding a comma-separated list whose elements are either individual ports or inclusive `low-high` ranges, freely mixed — `"22,80,443,8000-8100"` is valid. Ports are 1–65535; a range with `low > high` is `bad_args`. All `*_timeout_ms` values are integer milliseconds. `targets` accepts both bare addresses and CIDR notation in the same list.

### 7.3 Result messages

Every message on the `result` topic carries `type: "result"` in the envelope, so `data.event` discriminates which kind it is. Receivers **must** switch on `event` and reject a result message that lacks it.

| `data.event` | Additional `data` fields |
|---|---|
| `accepted` | `job_id` |
| `chunk` | `job_id`, `seq`, one command-specific array (below) |
| `done` | `job_id`, `chunks`, `results`, `duration_ms` |
| `error` | `job_id`, `code`, `message` |

Each chunk carries `job_id`, `seq`, and a command-specific array:

| Command | Array key | Element |
|---|---|---|
| `port_scan` | `open` | `{host, port, state, rtt_ms}` |
| `banner_grab` | `banners` | `{host, port, text, bytes, truncated}` |
| `dns` | `answers` | `{name, type, ttl, value}` |
| `trace` | `hops` | `{ttl, addr, rtt_ms, timeout}` |
| `discover` | `hosts` | `{ip, mac, rtt_ms, method}` |
| `wifi_survey` | `aps`, `clients` | `{bssid, ssid, channel, rssi, auth, hidden}` / `{mac, bssid, rssi}` |

Example:

```json
{"event": "chunk", "job_id": "job-7f3a91", "seq": 3,
 "open": [{"host":"192.168.1.10","port":22,"state":"open","rtt_ms":2.4}]}
```

### 7.4 Job lifecycle

```
cmd ──► accepted ──► chunk(seq 0) ──► chunk(seq 1) ──► … ──► done
                └──────────────────────────────────────────► error
```

- **`accepted`** — sent before any work begins. It is a distinct message from the first chunk specifically so that a long-running or store-and-forward job is distinguishable from a node that never received the command.
- **Chunks** — `seq` starts at `0` and increments by exactly 1.
- **`done`** — terminal, carrying a summary: `{"job_id":…, "chunks": n, "results": n, "duration_ms": n}`.
- **`error`** — terminal, carrying `{"job_id":…, "code":…, "message":…}`.

The server **must** detect gaps in `seq` and mark such a job **`incomplete`** rather than storing a truncated result set. A scan that silently lost forty hosts is a worse outcome than one that reports itself as broken.

The server applies a job timeout: if no chunk, `done`, or `error` arrives within the job's deadline, the job is marked `timed_out`. For `wifi_survey` the deadline accounts for the announced `expect_back_in`.

### 7.5 Concurrency

**One job at a time per node**, queue depth one. A command arriving while the node is busy and the queue is full is rejected with `error` code `busy`.

This is a hardware constraint, not a simplification. An ESP32-S3 running a scan with an open socket set plus JSON serialization has limited heap headroom; permitting two concurrent scans to allocate buffers is a direct route to heap exhaustion and a watchdog reboot.

`cancel` and `identify` are exempt — they are handled immediately regardless of job state.

### 7.6 Scheduled monitoring

`set_monitor` pushes a monitoring configuration that the node **persists to NVS** and executes on its own timer:

```json
{"enabled": true, "interval_s": 5,
 "devices": [{"id": 1, "ip": "192.168.1.1", "checks": ["ping","port"], "ports": [22,53,80]}]}
```

Results publish to the `monitor` topic, independent of the job machinery:

```json
{"cycle_ts": 1755302400,
 "results": [{"id":1,"status":"up","latency_ms":1.8,"ports":{"22":"open","53":"closed","80":"open"}}]}
```

`status` is one of `up`, `down`, or `unknown`. Per-port values are `open`, `closed`, or `filtered`. `latency_ms` is `null` when `status` is not `up`. The server derives alert state from consecutive `down` cycles; the node reports observations only and holds no alerting logic.

Because the schedule lives on the node, monitoring continues across server restarts. Results produced while the server or broker is unreachable are buffered per §8.3 and flushed on reconnect, so restarting Flask no longer punches holes in the history.

## 8. Security and error handling

### 8.1 Credentials

Each probe authenticates to the broker as itself. Credentials are **generated server-side**: adding a node in the Flask UI produces a username/password pair and the matching Mosquitto ACL entry, which is then entered into that board's captive portal during setup.

A token-based auto-enrollment scheme was considered and rejected. Since TLS is deferred, an enrollment token would cross the network in plaintext, so the added machinery would buy complexity rather than security.

Credentials are stored in NVS. ESP32 flash encryption is available and is documented as future work alongside TLS.

### 8.2 ACLs

```
user probe-a4c1f8
topic write nms/v1/node/probe-a4c1f8/result
topic write nms/v1/node/probe-a4c1f8/monitor
topic write nms/v1/node/probe-a4c1f8/status
topic write nms/v1/node/probe-a4c1f8/telemetry
topic write nms/v1/announce
topic read  nms/v1/node/probe-a4c1f8/cmd
```

The server account holds read access across `nms/v1/#` and write access to `nms/v1/node/+/cmd`.

The resulting containment property: a compromised probe can neither issue commands to the fleet nor read another probe's results.

### 8.3 Error codes

| Code | Meaning |
|---|---|
| `busy` | A job is running and the queue is full. |
| `unsupported` | Command not in this node's `capabilities`. |
| `bad_args` | Arguments malformed or out of range. |
| `unreachable` | Target did not respond. |
| `timeout` | Operation exceeded its deadline. |
| `oom` | Insufficient heap to execute. |
| `cancelled` | Terminated by a `cancel` command. |
| `radio_conflict` | Requested operation conflicts with the current radio state. |

### 8.4 Reconnection

Wi-Fi and MQTT reconnects use **exponential backoff with jitter**, from 1 s to a 60 s ceiling. Jitter is required, not optional: without it, three probes returning after a broker restart retry in lockstep and stampede it.

### 8.5 Offline buffering

Results produced while disconnected go into a **bounded** queue. On overflow the oldest entries are dropped and the next published message sets `"dropped": <n>`. Admitting loss is required; silently truncating is not.

## 9. Conformance

Three layers, each with a distinct job:

**Schemas** (`protocol/schemas/`) — one JSON Schema per message type. These are normative. Prose is where ambiguity hides; a schema stating that `ts` is an integer of Unix seconds leaves nothing to argue about.

**Golden corpus** (`protocol/golden/`) — canonical `.json` fixtures both implementations test against byte-identically. Must include the edge cases that actually bite: a `done` with zero results, a chunk carrying `dropped`, a `wifi_survey` burst following a `surveying` gap, a sequence with a hole, and malformed messages that must be **rejected**.

**Conformance suite** — scenario tests driven over MQTT. Because both the virtual probe and the firmware implement this protocol, **one suite validates both**: it runs against `probe-server` with no hardware during subsystem 2, then against `probe-a4c1f8` unchanged in subsystem 3. Firmware that passes the suite the Python probe already passes is protocol-correct by construction.

Local Mosquitto is the only dependency. No hardware is required to develop or run any of it.

## 10. Deliverables

1. `protocol/schemas/` — JSON Schema per message type
2. `protocol/golden/` — fixture corpus including the edge cases above
3. Conformance suite skeleton plus its no-hardware runner
4. Mosquitto configuration: ACL template and per-node credential generation

Fleshing out the suite's scenarios happens in subsystems 2 and 3, when there is something to run it against.

## 11. Out of scope

Deferred deliberately, with reasons:

- **TLS / mutual TLS** — mbedTLS heap contends with promiscuous-mode packet buffers. Revisit if RAM profiling permits.
- **Flash encryption** for NVS-stored credentials.
- **Automatic enrollment** — see §8.1.
- **Server storage schema, REST/SSE API, UI** — subsystems 2 and 4.
- **Firmware internals** (radio scheduler, portal implementation) — subsystem 3.
- **~~Packet injection or deauthentication~~** — *(revised 2026-08-17)* the `wifi_deauth` capability is now in scope as a C2-gated, confirm-required security testing command. The `wifi_ids` capability (passive frame classification for intrusion detection) is also in scope. Both use standard ESP-IDF APIs (`esp_wifi_80211_tx`, `esp_wifi_set_promiscuous`). See firmware spec §6.4.
