# Architecture

## System overview

NMS is a distributed wireless-security monitoring system: a fleet of ESP32 probes and one Python
command-and-control (C2) server, talking **Probe Protocol v1** over MQTT. Four pieces:

- **`protocol/`** — the contract: JSON Schemas, a golden fixture corpus, topic construction,
  port-spec parsing, the job state machine, and broker credential/ACL generation. No Flask, no
  MQTT client.
- **`server/` + `probe/`** — the C2 server (MQTT bridge, SQLite storage, REST + SSE API) and
  `probe-server`, a Python virtual probe that speaks the same protocol as a real node over the
  broker.
- **`firmware/`** — the ESP32 (classic WROOM-32, not S3) Arduino/PlatformIO probe.
- **`templates/` + `static/`** — the web operations console: dense mono tables, a live SSE feed,
  no build step.

## Data flow

Four processes, MQTT between them. The Flask server must run as a single process —
`server/events.py` is an in-memory bus, so a second worker would give each browser only the
events that landed on its worker. Do not add Gunicorn workers without replacing the bus.

```
ESP32 probe ──┐                        ┌── virtual probe (probe-server)
              ├── Mosquitto (Docker) ──┤
Flask server ─┘                        └── browser (SSE)
```

**Inbound** (`mqtt_bridge` → `ingest`): the bridge owns the paho client and thread; `dispatch()`
validates every payload with `protocol.validate` before anything touches the database and drops
what fails, then routes by topic leaf to a handler in `ingest.py`. Handlers take an
already-validated dict and write rows — no MQTT, no threading — which is why most test coverage
lives at that seam.

**Outbound** (`api` → `commands` → publisher): `commands.create_job()` builds the `cmd` message,
validates it, and publishes before writing the Job row, so a rejected command leaves no trace.
The publisher is pluggable (`commands.set_publisher`): the bridge registers the real one at
startup, tests register a sink. Nothing in `commands.py` imports paho, so the whole job lifecycle
is testable without a broker.

### Job lifecycle

`pending → accepted → {done | incomplete | error | timed_out}`. `incomplete` is the interesting
one: on `done`, `ingest` recomputes which `seq` values are missing below the highest received and
lands the job `incomplete` with `gaps` recorded rather than pretending it succeeded. Chunk storage
is idempotent on `(job_id, seq)` because QoS 1 redelivers. `cancel` does not mark the job terminal
server-side — the node answers with an `error` event carrying code `cancelled`, and ingest records
that. Timeouts slide from `last_event_at`, not creation, swept every 10s by
`server/maintenance.py`.

### Storage

Nine tables in `server/models.py`. Raw `monitor_cycles` are kept 7 days, then summarised into
hourly `monitor_rollups` and bulk-deleted. `roll_up_hour` is idempotent (re-running an hour
overwrites), and rollup must happen before pruning. The bulk DELETE bypasses the ORM cascade,
which is the sole reason `server/db.py` turns on SQLite's `PRAGMA foreign_keys=ON` — do not remove
it. `wifi_survey` chunks are additionally projected into the typed `ap_observations` table so
multi-vantage `GROUP BY bssid` queries work; no other command gets a projection.

`instance/nms.db` and `network_monitor.db` are stale pre-rewrite data, deliberately not migrated.
Ignore them.

## MQTT topic hierarchy

Namespace: `nms/v1`. The server subscribes to `nms/v1/#`; each node publishes on its own subtree
and subscribes only to its own `cmd` topic.

| Topic | Direction | Purpose |
| --- | --- | --- |
| `nms/v1/announce` | node → server | Capability advertisement on connect |
| `nms/v1/node/<node_id>/cmd` | server → node | Recon/control command dispatch |
| `nms/v1/node/<node_id>/result` | node → server | Chunked job results (`accepted`/`chunk`/`done`/`error`) |
| `nms/v1/node/<node_id>/status` | node → server | Online/offline state — retained, carries the LWT |
| `nms/v1/node/<node_id>/telemetry` | node → server | Periodic heap/uptime/RSSI/channel |
| `nms/v1/node/<node_id>/monitor` | node → server | Scheduled reachability-check cycle results |

`node_id` matches `^probe-(?:[0-9a-f]{6}|server)$`, derived from the low three MAC bytes. Broker
ACLs scope each node to its own subtree, so a node can publish neither another node's results nor
read another node's commands.

## Protocol invariants

Full wire-format reference — envelope, topics, commands, result shapes, error codes — is in
[`docs/PROTOCOL.md`](docs/PROTOCOL.md). The invariants below are the parts that are shared with
the C++ firmware and not free to adjust:

- `ts` is Unix seconds, UTC, never milliseconds. Envelope is `{v, type, node, msg_id, ts, data}`.
- A published payload must not exceed 1024 bytes; producers chunk to stay under it.
- Control commands (`set_monitor`, `cancel`, `identify`, `reboot`, `get_config`) dispatch
  regardless of advertised capabilities; recon commands require the capability in the node's
  `announce`.
- `protocol/schemas/` is normative — prose loses to schema. `protocol/golden/` fixtures are a
  byte-identical contract between the Python validator and the firmware, which is why
  `.gitattributes` pins everything to LF. Adding a message field means schema + golden fixture,
  not just Python.

`probe/virtual_probe.py` is the reference implementation of node behaviour. Firmware is a second
implementation of the same state machine; where they disagree, the golden corpus decides.

## Hardware setup

Target board: classic ESP32 WROOM-32 (`board = esp32dev` in `firmware/platformio.ini`), not the
S3. Framework is Arduino-on-PlatformIO with `espressif32@6.9.0`, so `esp_wifi_*` ESP-IDF calls are
available directly.

**RF-Sentinel wiring (CC1101 sub-GHz, `rf_sniff`).** The `rf_sniff` runner drives a CC1101
transceiver on the ESP32's VSPI bus — a *separate* radio from the 2.4 GHz WiFi/BLE core, so the
sweep never leaves the AP and MQTT stays connected throughout (it runs the normal worker
lifecycle, not the survey disconnect sequence). Wiring for the classic WROOM-32:

| CC1101 pin | ESP32 GPIO | Notes |
| --- | --- | --- |
| SCK | GPIO18 | VSPI clock |
| MISO / SO | GPIO19 | VSPI MISO |
| MOSI / SI | GPIO23 | VSPI MOSI |
| CSN / CS | GPIO5 | VSPI SS (strapping pin, idles high — fine as CS) |
| GDO0 | GPIO4 | RX carrier-sense / async data out (interrupt-capable) |
| GDO2 | GPIO25 | secondary status (reserved; wired for completeness) |
| VCC | 3V3 | **3.3 V only — 5 V destroys the module** |
| GND | GND | |

GPIO2 is deliberately avoided: it drives the onboard LED the `identify` command blinks. The driver
is `lsatan/SmartRC-CC1101-Driver-Lib` (MIT), pinned in `firmware/platformio.ini` under `[env:esp32]`
only — the native host build has no CC1101 and returns `unsupported`, like the other radio runners.
This table is mirrored at the top of `firmware/src/runners/rf_sniff.cpp`.

**Power.** WiFi TX and promiscuous-mode radio windows draw current spikes well above what a weak
USB port or hub can source cleanly; a brownout reset mid-scan is the usual symptom, not a hang.
Use a real USB data cable into a mainboard port (not a hub) during development, and if
`wifi_survey`/`wifi_ids`/`wifi_deauth` cause resets specifically (they all drop and reconnect the
radio), suspect power before suspecting the code.

**Flashing.** `monitor_speed` is fixed at 115200 in `platformio.ini`. PlatformIO's default upload
speed for `esp32dev` is faster (usually 921600); if `pio run -t upload` fails to sync with the
bootloader, add `upload_speed = 115200` to `[env:esp32]` and retry — slower but reliable over
marginal cables or USB-UART adapters. Hold BOOT during the "Connecting..." phase if the board
doesn't auto-reset into the bootloader, which is common on WROOM-32 dev boards without
auto-program circuitry.

**Serial output.** The firmware doesn't add custom `Serial.println` diagnostics yet — what you see
at 115200 baud is the standard ESP-IDF ROM bootloader banner plus whatever the Arduino-ESP32 core
logs at `CORE_DEBUG_LEVEL=3`. Expected shape on a clean boot:

```
ets Jun  8 2016 00:22:57
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:1
load:0x3fff0018,len:4
load:0x3fff001c,len:1216
entry 0x400806ac
[   312][I][WiFiGeneric.cpp:...] Connecting to WiFi...
[  1958][I][WiFiGeneric.cpp:...] STA IP: 192.168.1.47
```

This is the standard ESP32 boot / WiFi-connect shape, not a trace captured from this project's own
hardware — the fleet hasn't been verified on real boards yet (see project notes for the current
USB-enumeration blocker). Treat it as what to expect, not proof of anything.

## Running locally

```bash
pip install -r requirements-dev.txt

python -m pytest tests/ -q                       # full suite (~7s)
python -m pytest tests/test_ingest_result.py -q  # one file
python -m pytest tests/test_conformance.py -q    # scenario suite over a live broker
python -m pytest tests/ -q -k "gap or timeout"   # by name

python app.py                                    # server alone (dev, debug, no broker)
python scripts/run_all.py                        # broker + server + virtual probe, one command
python -m probe.virtual_probe                    # virtual probe alone
docker compose up -d mosquitto                   # broker only
python scripts/gen_node_credentials.py probe-a4c1f8
```

There is no linter, formatter, or build step configured. Tests are the only gate.

`tests/test_conformance.py` starts a throwaway Mosquitto container on a free port. It **skips
rather than fails** when the Docker daemon is down — a green run showing `4 skipped` means the
conformance scenarios did not execute. Check for skips before claiming protocol behaviour is
verified.

Required environment: `SECRET_KEY` (the app factory refuses to start without it),
`NMS_ADMIN_PASSWORD_HASH` (werkzeug hash; login always fails without it), `NMS_DATABASE_URI`,
`NMS_BROKER_HOST/PORT/USER/PASS`.

## Security considerations

This is security-research tooling built and tested on isolated lab networks owned by the
developer, as a university computer engineering / network security capstone project.

The firmware uses three ESP-IDF / Arduino-ESP32 radio APIs beyond the recon-only baseline:
`esp_wifi_80211_tx()` (raw 802.11 frame transmission, used by `wifi_deauth`),
`esp_wifi_set_promiscuous()` (monitor mode, used by `wifi_ids` for
deauth-flood/rogue-AP/evil-twin detection), and `BLEDevice::getScan()` (BLE enumeration, used by
`ble_scan`).

`wifi_deauth` is dual-use and worth being explicit about: it's a C2-dispatched job that transmits
real 802.11 deauthentication frames — broadcast plus any clients discovered via a short
promiscuous sniff of the target BSSID. It's registered in the firmware's worker dispatch table and
advertised in `announce.capabilities` like any other command; there's no on-device trigger, so it
only runs when the server publishes a `cmd` for it. The job schema requires `target_bssid` and
`confirm: true`, but that's an explicit-intent check on the command args, not an authorization
boundary — anyone who can publish a `cmd` for a node can set `confirm: true`. Actual access
control is the broker ACL scoping described above, not that flag.

Full threat model, the `wifi_ids` alert shapes, and the ESP32-Bit-Pirate attribution are in
[`docs/SECURITY.md`](docs/SECURITY.md) and [`firmware/NOTICE`](firmware/NOTICE).

## Conventions

- Docstrings explain the why — the invariant, the edge case, the reason something is the way it
  is — not what the next line does.
- Never commit `config/mosquitto/passwd`. The server generates node passwords and ACL blocks and
  returns the commands an operator runs; it never writes broker config itself.
- Stray zero-byte files named `` ` ``, `'`, `node`, `high\`` etc. are shell-quoting accidents.
  Delete them; don't commit them.
