# Server (C2 + Storage) — Design Spec

**Subsystem 2 of 4** · Status: **approved, ready for implementation planning** · Date: 2026-08-16

## 1. Scope

The command-and-control server: MQTT bridge, storage, REST + SSE API, and the Python virtual probe. It consumes the contract defined in [Probe Protocol v1](2026-08-16-probe-protocol-design.md) and the `protocol` package built from it.

Not in scope: firmware (subsystem 3), the web interface (subsystem 4). This subsystem serves the API that subsystem 4 will consume, and provides the peer that subsystem 3 develops against.

## 2. Context

Today `app.py` is 415 lines holding models, a polling thread, TCP port probing, and routes, writing to two overlapping stores: `instance/nms.db` (SQLAlchemy, `devices` + 10,904 `ping_results`) and `network_monitor.db` (raw sqlite3, 5,091 `device_logs`). Both last recorded 2026-06-09. The view layer is destroyed; the backend works.

Subsystem 1 delivered the `protocol` package — schemas, validation, topic construction, port parsing, job tracking, credential generation — with 131 tests passing and no Flask dependency.

## 3. Locked decisions

| Decision | Choice | Rationale |
|---|---|---|
| Existing data | Fresh schema; archive both `.db` files | Old rows predate the node concept; migrating means inventing a `probe-server` origin for data no probe produced, reconciling two stores that disagree on granularity, for two-month-stale data. Nothing is deleted, just not carried forward. |
| Broker | Mosquitto in Docker, config in repo | Test isolation: the suite starts a throwaway broker with known ACLs rather than mutating a persistent service. Config becomes version-controlled infrastructure. |
| Virtual probe | Separate process, over the broker | Enables killing the probe to demonstrate Last Will node-death detection — impossible if it is a thread in the server. Makes it a true protocol peer the conformance suite can target identically to firmware. |
| Retention | 7 days raw, then hourly rollups | Three probes at 5 s produce ~52,000 cycles/day. Recent view stays full-resolution; long-range trends survive; SQLite remains viable. |
| Web auth | Single admin login, session-based | This console orders scans across a network; leaving it open is a real finding on a security project. Gives a coherent posture: probes authenticate to the broker, operators to the console. |
| Test broker | Pytest fixture starts a throwaway container | Hermetic suite, no setup ritual, no stale retained messages leaking between tests. Skips rather than fails when Docker is absent. |

## 4. Process topology

Four processes:

```
┌─────────────┐   MQTT    ┌──────────────┐   MQTT    ┌────────────────┐
│ ESP32 probe │◄─────────►│  Mosquitto   │◄─────────►│ virtual probe  │
│ (subsys 3)  │           │  (Docker)    │           │ probe-server   │
└─────────────┘           └──────┬───────┘           └────────────────┘
                                 │ MQTT
                          ┌──────▼───────┐    SSE
                          │ Flask server │──────────► browser (subsys 4)
                          │  + bridge    │
                          └──────────────┘
```

The server holds one MQTT client on a background thread, subscribed to `nms/v1/#`. Inbound messages are validated with `protocol.validate`, persisted, and fanned out to SSE subscribers. Command dispatch runs the other way: an HTTP POST becomes a publish to that node's `cmd` topic.

**The server must run as a single process.** SSE fan-out is in-memory, so multiple workers would each hold a separate event bus and each browser would see only the events that happened to land on its worker. Do not deploy under Gunicorn with `--workers > 1` without replacing `events.py` with an external bus.

## 5. Module structure

| Module | Responsibility |
|---|---|
| `server/db.py` | SQLAlchemy instance only, no models |
| `server/models.py` | Table definitions |
| `server/events.py` | In-memory pub/sub feeding SSE subscribers |
| `server/ingest.py` | Message-type handlers: announce, status, result, monitor, telemetry |
| `server/mqtt_bridge.py` | MQTT client thread: connection, subscribe, dispatch to `ingest` |
| `server/commands.py` | Job creation, publish to `cmd` topic, cancellation |
| `server/enrolment.py` | Node registration and credential/ACL generation |
| `server/maintenance.py` | Background thread: job-timeout sweep every 10 s, rollup and prune hourly |
| `server/auth.py` | Admin session login and the `@require_auth` decorator |
| `server/api.py` | REST blueprint |
| `server/stream.py` | SSE endpoint |
| `app.py` | Thin entry point: app factory wiring only |
| `probe/checks.py` | ICMP ping and TCP port probing, lifted from today's `app.py` |
| `probe/jobs.py` | Command implementations for the virtual probe |
| `probe/virtual_probe.py` | `probe-server` process: MQTT client, job execution, monitor scheduler |

`ingest.py` is separate from `mqtt_bridge.py` deliberately. The bridge owns the connection and the thread; the handlers own the semantics. Handlers are therefore testable by calling them with a dict — no broker, no threading, no timing — which is where most test coverage lives.

## 6. Data model

### 6.1 Tables

**`nodes`** — fleet registry.
`node_id` (PK, text), `label`, `fw`, `chip`, `mac`, `capabilities` (JSON array), `state`, `first_seen`, `last_seen`, `last_status_ts`.

**`devices`** — monitoring targets.
`id` (PK), `name`, `ip`, `role`, `enabled` (bool), `node_id` (FK, **nullable**).

A null `node_id` means every probe monitors this device; a set value restricts it to one. A probe on a different subnet physically cannot reach every target, so per-node assignment is real — but a join table is premature when one nullable column covers both cases.

**`monitor_cycles`** — one row per node per monitoring tick.
`id` (PK), `node_id` (FK), `cycle_ts`, `received_at`. Unique on (`node_id`, `cycle_ts`) so a redelivered QoS-1 message cannot double-insert.

**`monitor_results`** — per-device outcome within a cycle.
`id` (PK), `cycle_id` (FK, cascade delete), `device_id` (FK), `status` (`up`/`down`/`unknown`), `latency_ms` (nullable), `ports` (JSON).

**`monitor_rollups`** — hourly summary surviving pruning.
`id` (PK), `node_id`, `device_id`, `hour_ts`, `samples`, `up_count`, `latency_min`, `latency_avg`, `latency_max`. Unique on (`node_id`, `device_id`, `hour_ts`).

**`jobs`** — one row per dispatched command.
`job_id` (PK, text), `node_id` (FK), `cmd`, `args` (JSON), `state`, `created_at`, `accepted_at`, `finished_at`, `chunks`, `results`, `duration_ms`, `error_code`, `error_message`, `gaps` (JSON array).

`state` mirrors `protocol.job.JobState`: `pending`, `accepted`, `done`, `incomplete`, `error`, `timed_out`.

**`job_chunks`** — raw result chunks.
`id` (PK), `job_id` (FK, cascade delete), `seq`, `payload` (JSON), `received_at`. Unique on (`job_id`, `seq`) — this is what makes redelivery idempotent.

**`telemetry`** — node health samples.
`id` (PK), `node_id` (FK), `ts`, `free_heap`, `uptime_s`, `rssi`, `channel`, `state`, `jobs_done`.

**`ap_observations`** — projected RF survey results.
`id` (PK), `node_id` (FK), `job_id` (FK), `bssid`, `ssid`, `channel`, `rssi`, `auth`, `hidden`, `observed_at`. Indexed on (`bssid`, `observed_at`).

### 6.2 Why only RF survey gets a projection

Every command's chunks land raw in `job_chunks`, which is sufficient for the console to render results and gives full replay. `wifi_survey` chunks are *additionally* exploded into `ap_observations`, because that is the one dataset requiring real SQL: `GROUP BY bssid` with `rssi` compared across `node_id` is the multi-vantage correlation that distinguishes this project from a single-device tool. Typed tables for port scans, traceroutes, and DNS answers would serve no query that currently exists.

### 6.3 Retention

`server/maintenance.py` runs one background thread waking every **10 seconds**. Two jobs run at different cadences on that tick:

- **Every tick:** sweep for jobs past their deadline and mark them `timed_out` (§8). This must be frequent — an hourly sweep would leave a hung job undetected for up to an hour.
- **Once per hour:** roll up and prune.
  1. For every complete hour older than 7 days, upsert `monitor_rollups` from `monitor_results` — `samples`, `up_count`, and min/avg/max latency over non-null `latency_ms`.
  2. Delete `monitor_cycles` older than 7 days; `monitor_results` follows by cascade.

`job_chunks`, `telemetry`, and `ap_observations` are **not** pruned in this version — they accumulate far more slowly, being driven by operator actions rather than a 5-second timer. Revisit if `telemetry` becomes a problem at 30-second samples across a large fleet.

`GET /api/devices/<id>/history` reads `monitor_results` for windows inside 7 days and `monitor_rollups` beyond, so the transition is invisible to callers.

## 7. MQTT bridge and ingest

### 7.1 Bridge

`mqtt_bridge.py` runs a paho-mqtt client on a background thread with `loop_forever`, using automatic reconnect with exponential backoff and jitter (spec §8.4). It subscribes to `nms/v1/#` and routes each message to `ingest` by topic leaf.

Every inbound payload passes `protocol.validate.validate_message` **before** anything touches the database. A message that fails validation is logged with the node id, topic, and the specific schema error, then dropped. Malformed input from a probe must never become a database row.

### 7.2 Handlers

| Topic leaf | Handler | Effect |
|---|---|---|
| `announce` | `handle_announce` | Upsert `nodes`; refresh `capabilities`, `fw`, `label`; set `first_seen` once |
| `status` | `handle_status` | Update `nodes.state` and `last_status_ts`; emit `node_status` event |
| `telemetry` | `handle_telemetry` | Insert `telemetry`; update `last_seen` |
| `monitor` | `handle_monitor` | Insert `monitor_cycles` + `monitor_results`; emit `monitor_cycle` |
| `result` | `handle_result` | Drive the job state machine (§8) |

An `announce` from an unknown `node_id` is accepted and creates the row — the broker ACL already established that it is an authorised node, so rejecting here would add nothing.

### 7.3 Event bus

`events.py` holds a set of subscriber queues. Handlers publish typed events; `stream.py` drains a per-client queue into the SSE response.

Each queue is bounded at **500 events**. A client whose queue fills is disconnected rather than allowed to grow memory without limit — with three probes emitting monitor cycles every 5 seconds plus telemetry, a browser tab left open on a suspended laptop would otherwise accumulate events indefinitely. Publishing never blocks on a slow consumer.

## 8. Job lifecycle

`POST /api/nodes/<id>/jobs` with `{"cmd": ..., "args": {...}}`:

1. Reject with 400 if `cmd` is not in that node's `capabilities` (recon commands only — control commands are a mandatory baseline per protocol spec §6.2).
2. Validate `args` by constructing the full command message and passing it through `protocol.validate`.
3. Generate `job_id`, insert `jobs` row with state `pending`.
4. Publish to `nms/v1/node/<id>/cmd` at QoS 1.
5. Return `202 Accepted` with the `job_id`.

Inbound `result` messages drive a `protocol.job.JobTracker` per active job:

- `accepted` → state `accepted`, set `accepted_at`
- `chunk` → insert `job_chunks` (ignoring duplicate `seq` by unique constraint), record seq for gap detection, emit `job_event`; if the parent job's `cmd` is `wifi_survey`, also project `aps` into `ap_observations`
- `done` → state becomes `done`, or **`incomplete`** if `JobTracker.gaps` is non-empty; store summary and `gaps`
- `error` → state `error` with `code` and `message`

The maintenance thread (§6.3) sweeps every 10 seconds and marks a job `timed_out` when no event has arrived within its deadline.

**Deadline definition.** A job's deadline is **120 seconds since its last received event** — `created_at` for a job with no events yet, otherwise the most recent `accepted` or `chunk`. Sliding from the last event rather than from creation means a long but healthy streaming scan is never killed for taking a while; only genuine silence trips it.

For `wifi_survey` the deadline becomes `expect_back_in + 60` seconds, taken from the `surveying` status the node published before disconnecting. That absence is announced and bounded by design (protocol spec §6.4), so it must not be mistaken for a hung job.

## 9. Virtual probe

`probe/virtual_probe.py` is a standalone process taking broker host, credentials, and node id (`probe-server`) from environment or CLI. It behaves exactly as firmware must:

- Publishes `announce` on connect with `capabilities` covering what it implements: `port_scan`, `banner_grab`, `dns`, `trace`, `discover`. It does **not** claim `wifi_survey` — a server has no radio, and this is precisely the case the `capabilities` mechanism exists to handle.
- Registers a Last Will on its `status` topic.
- Executes one job at a time, rejecting concurrent work with `busy` (protocol spec §7.5).
- Streams chunks capped at 1024 bytes with incrementing `seq`, then a terminal `done`.
- Persists its `set_monitor` config to a local JSON file and self-schedules monitoring, buffering results while disconnected and flushing on reconnect with `dropped` set on overflow.

`probe/checks.py` carries forward the working ICMP and TCP logic from today's `app.py` — `build_ping_command`, `parse_latency_ms`, and `check_port` move essentially unchanged.

## 10. API

```
GET    /api/nodes                  fleet: state, label, capabilities, last_seen
GET    /api/nodes/<id>             detail + latest telemetry
POST   /api/nodes                  enrol: returns credentials + ACL block
GET    /api/devices                monitoring targets
POST   /api/devices                add target, push set_monitor
PATCH  /api/devices/<id>
DELETE /api/devices/<id>
GET    /api/devices/<id>/history   raw under 7 days, rollups beyond
POST   /api/nodes/<id>/jobs        issue command → 202 + job_id
GET    /api/jobs/<job_id>          state, summary, gaps
GET    /api/jobs/<job_id>/chunks   results
POST   /api/jobs/<job_id>/cancel
GET    /api/rf/aps                 observations grouped by bssid, rssi per node
GET    /api/stream                 SSE
POST   /api/login, /api/logout
```

`GET /api/rf/aps` is the endpoint that earns the project its originality claim: which access points the fleet sees, and how signal strength differs between probes — a question no single-device tool can answer.

`GET /api/stream` emits four event types — `node_status`, `job_event`, `monitor_cycle`, `telemetry` — on one stream, with the browser filtering client-side. Browsers cap concurrent connections per origin and the console needs job events and node status at once, so one stream beats four endpoints.

## 11. Security

**Web console.** A single admin password, hashed with `werkzeug.security.generate_password_hash`, read from `NMS_ADMIN_PASSWORD_HASH` in the environment. Login establishes a Flask session; `@require_auth` guards every `/api/*` route except `/api/login`. `SECRET_KEY` comes from the environment and the process refuses to start without it, so a default key can never reach a deployment.

**Node enrolment.** `POST /api/nodes` takes `{"node_id": "probe-a4c1f8", "label": "Lab North"}` — `node_id` required and validated by `protocol.topics.validate_node_id`, `label` optional. It creates the `nodes` row in state `unprovisioned`, then calls `protocol.credentials` to produce a password and ACL block, returning both plus the exact commands to apply them:

```
docker compose exec mosquitto mosquitto_passwd -b /mosquitto/config/passwd <node_id> <password>
# append the returned ACL block to config/mosquitto/aclfile
docker compose restart mosquitto
```

**The server never writes broker configuration itself.** Mosquitto password files require its own hashing format, and reimplementing that in Python to avoid one documented command would be a poor trade. The server generates and instructs; the operator applies. The plaintext password is shown exactly once, at generation.

## 12. Configuration and deployment

`docker-compose.yml` at the repository root defines the `mosquitto` service, binding `config/mosquitto/` into the container:

- `mosquitto.conf` — listener 1883, `allow_anonymous false`, `password_file`, `acl_file`, persistence on
- `aclfile` — server account plus one block per node, committed
- `passwd` — **gitignored**, contains hashes

`scripts/run_all.py` starts the broker, waits for it to accept connections, then launches the server and the virtual probe, so day-to-day work is one command despite three processes.

## 13. Testing

**Handler unit tests** — call `ingest` handlers with dicts, assert rows. No broker, no threads. Most coverage lives here.

**API tests** — Flask test client against in-memory SQLite, covering the REST surface and the auth gate.

**Integration tests** — a session-scoped pytest fixture starts a Mosquitto container on a free port with test-specific ACLs and tears it down; if Docker is unavailable these **skip rather than fail**, so unit tests still run anywhere. Covers: bridge ingestion end to end, command dispatch, and the virtual probe answering a real job.

**Conformance suite** (protocol spec §9, layer 3) — scenario tests over MQTT, pointed at any node. Runs against `probe-server` here and against firmware unchanged in subsystem 3. Required scenarios:

1. Clean streamed job: `accepted` → chunks → `done`
2. Deliberate `seq` gap → job lands `incomplete`, `gaps` recorded
3. Second command while busy → `error` with code `busy`
4. `cancel` mid-stream → terminal `error` with code `cancelled`
5. Graceful `surveying` disconnect → **no** false offline
6. Ungraceful kill → Last Will fires, node marked `offline`

Scenarios 5 and 6 test the design's cleverest element, and killing the virtual probe process is exactly how to exercise them.

## 14. Deliverables

1. `server/` package as laid out in §5
2. `probe/` package with the virtual probe process
3. Fresh schema, plus `server/maintenance.py` covering job timeouts, rollups, and pruning
4. `docker-compose.yml` and `config/mosquitto/`
5. `scripts/run_all.py`
6. Conformance suite completing protocol spec §9
7. New dependencies: `paho-mqtt`, `pytest` fixtures for Docker

## 15. Out of scope

- **Web interface** — subsystem 4. This subsystem ends at the API.
- **Firmware** — subsystem 3.
- **TLS on MQTT, flash encryption** — deferred in protocol spec §11.
- **Multi-worker deployment** — precluded by the in-memory event bus (§4).
- **Migrating the 16,000 legacy rows** — archived, per §3.
- **Pruning `job_chunks`, `telemetry`, `ap_observations`** — operator-driven growth rates; revisit when measured.
