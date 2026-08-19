# Firmware Probe Implementation Plan — Subsystem 3 + Extended Capabilities

**Goal**: Implement the ESP32 firmware probe per the approved spec, then extend it with three powerful new capabilities borrowed from ESP32-Bit-Pirate that transform the capstone from a passive recon fleet into a **distributed penetration-testing and intrusion-detection platform**.

## What Makes This Impressive

The approved spec delivers a distributed passive recon fleet — already a strong capstone. The extensions turn it into something a panel will remember:

| Layer | Capabilities | Vantage |
|---|---|---|
| **Passive Network Recon** | `port_scan`, `banner_grab`, `dns`, `trace`, `discover` | Multi-probe TCP/IP |
| **Passive RF Survey** | `wifi_survey` (APs + client stations) | Multi-probe RSSI correlation |
| **Active WiFi Attack** | `wifi_deauth` — targeted deauthentication from any probe | C2-dispatched, distributed |
| **BLE Reconnaissance** | `ble_scan` — enumerate every BLE device in range | Multi-probe BLE correlation |
| **Distributed IDS** | `wifi_ids` — detect deauth attacks, rogue APs, evil twins | Real-time fleet-wide alerting |

The story: *"Three $5 microcontrollers, permanently deployed, doing everything from port scanning to deauth attacks to intrusion detection — all orchestrated from one command-and-control server."*

---

## User Review Required

> [!IMPORTANT]
> **Protocol & Server Changes**: The approved firmware spec treats the protocol and server as "finished contracts." The three new capabilities (`wifi_deauth`, `ble_scan`, `wifi_ids`) require schema additions in `protocol/`, new ingest handlers in `server/`, and new API endpoints. This plan covers all cross-subsystem changes needed.

> [!WARNING]  
> **WiFi Deauth is illegal** without explicit authorization on networks you own or have written permission to test. For capstone demonstration, use an isolated lab network with your own AP. The implementation includes a `confirm: true` safety flag in the command args — the server rejects deauth commands without it.

> [!IMPORTANT]
> **Hardware**: This targets the classic ESP32 WROOM-32 (not S3). BLE scanning uses the ESP32's built-in Bluetooth 4.2 LE radio. The ESP32 cannot do WiFi and BLE simultaneously at full performance — the plan handles this with explicit radio arbitration (§A.4).

---

## Open Questions

1. **BLE + WiFi coexistence**: The ESP32 shares a single 2.4 GHz radio between WiFi and BLE. During `ble_scan`, WiFi stays connected but throughput drops. During `wifi_ids` (promiscuous mode), BLE is unavailable. Is this acceptable, or do you want BLE scans to also disconnect from WiFi for cleaner results?

2. **Deauth burst count**: ESP32-Bit-Pirate defaults to repeated bursts. Should the C2 server expose a `bursts` parameter (more aggressive but noisier), or fix it at a conservative default (e.g., 5 bursts)?

3. **IDS alert mechanism**: When `wifi_ids` detects a deauth attack or rogue AP, should it (a) stream results as chunks like any other job, or (b) use a new dedicated MQTT topic `nms/v1/node/<id>/alert` for real-time alerting with higher priority? Option (b) is more impressive but requires a new topic.

---

## Proposed Changes

The plan is organized into 4 phases. Phases 1–2 implement the approved spec. Phases 3–4 add the extended capabilities.

---

### Phase 1: PlatformIO Skeleton + Provisioning + MQTT

The foundation: a firmware that boots, provisions itself, connects to MQTT, announces, and sends telemetry. No recon yet — just the lifecycle.

---

#### [NEW] [platformio.ini](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/platformio.ini)

PlatformIO project configuration:
- Board: `esp32dev` (WROOM-32)
- Framework: `arduino`
- Platform: `espressif32@6.x`
- Two environments: `esp32` (hardware) and `native` (host unit tests)
- Libraries: `PubSubClient` (MQTT), `ArduinoJson` (JSON), `DNSServer` + `WebServer` (captive portal)
- Build flags: `-DCORE_DEBUG_LEVEL=3`, `-DARDUINO_EVENT_RUNNING_CORE=0`
- Partition scheme: `min_spiffs` (maximize app flash)

```ini
[env:esp32]
platform = espressif32@6.9.0
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps =
    knolleary/PubSubClient@^2.8
    bblanchon/ArduinoJson@^7.0
build_flags =
    -DCORE_DEBUG_LEVEL=3
    -DMQTT_MAX_PACKET_SIZE=1280
board_build.partitions = min_spiffs.csv

[env:native]
platform = native
build_flags = -std=c++17
test_framework = unity
```

---

#### [NEW] [firmware/src/main.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/main.cpp)

Arduino entry point. `setup()` reads NVS, decides provisioned vs. unprovisioned path. `loop()` is the MQTT task on core 0 (Arduino loop runs on core 1 by default — we'll pin it to core 0 in `platformio.ini` or use `xTaskCreatePinnedToCore`).

Key responsibilities:
- Read NVS config at boot
- If unprovisioned → start SoftAP + captive portal (§4.3 of spec)
- If provisioned → connect WiFi with backoff, connect MQTT with LWT, subscribe to `cmd` topic
- Spawn worker task on core 1
- In loop: service `PubSubClient.loop()`, drain outbox, emit telemetry every 30s

---

#### [NEW] [firmware/src/config.h](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/config.h) / [config.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/config.cpp)

NVS configuration management:
- `struct ProbeConfig { char ssid[33]; char wifi_pass[65]; char broker_host[64]; uint16_t broker_port; char mqtt_user[32]; char mqtt_pass[64]; char label[65]; bool valid; }`
- `loadConfig()` — reads from NVS namespace `"nms"`
- `saveConfig(const ProbeConfig&)` — writes to NVS
- `clearConfig()` — erases NVS namespace (factory reset via GPIO0 hold)

---

#### [NEW] [firmware/src/identity.h](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/identity.h) / [identity.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/identity.cpp)

Node identity derivation:
- `getNodeId()` → `"probe-"` + low 3 bytes of `esp_efuse_mac_get_default()` in lowercase hex
- Must match `^probe-(?:[0-9a-f]{6}|server)$`
- `getMacString()` → colon-delimited lowercase MAC for `announce`

---

#### [NEW] [firmware/src/portal.h](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/portal.h) / [portal.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/portal.cpp)

Captive portal provisioning:
- SoftAP named `nms-probe-<node_id>` (open, no password)
- `DNSServer` wildcard → `192.168.4.1`
- `WebServer` serves one HTML form at `/` with fields: WiFi SSID, WiFi password, broker host, broker port, MQTT user (defaults to node_id), MQTT password, label
- `POST /save` → validates, calls `saveConfig()`, reboots
- HTML is a `const char PROGMEM[]` string — minimal, functional, no external dependencies

---

#### [NEW] [firmware/src/mqtt_client.h](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/mqtt_client.h) / [mqtt_client.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/mqtt_client.cpp)

MQTT connection management (runs on core 0):
- Wraps `PubSubClient` — sole owner, no mutex needed
- `connect()` — sets client ID to `node_id`, registers LWT on `nms/v1/node/<id>/status` with `{"state":"offline","reason":"lwt"}`, sets buffer size to 1280
- `reconnect()` — exponential backoff 1s → 60s with jitter (spec §8.1), host-testable pure function for backoff calculation
- `publishEnvelope(topic, type, data)` — constructs the universal envelope `{v, type, node, msg_id, ts, data}`, serializes with ArduinoJson, publishes
- `drainOutbox()` — pops from the FreeRTOS queue and publishes; called from `loop()`
- `onMessage(topic, payload)` — parses inbound `cmd`, dispatches to worker or handles `cancel`/`identify` immediately
- Telemetry emission every 30s: `{free_heap, uptime_s, rssi, channel, state, jobs_done}`

---

#### [NEW] [firmware/src/outbox.h](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/outbox.h) / [outbox.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/outbox.cpp)

Bounded cross-task queue:
- `QueueHandle_t` of pre-serialized JSON `char[]` frames
- Capacity: 32 slots (configurable)
- `enqueue(const char* topic, const char* json, size_t len)` — if full, drops oldest, increments `dropped` counter
- `dequeue(OutboxFrame& frame)` → true if a frame was available
- `getAndResetDropped()` → returns drop count and resets to 0; next published message carries `"dropped": N`
- Thread-safe: only the worker task enqueues, only the MQTT task dequeues — single-producer single-consumer, no mutex needed

---

#### [NEW] [firmware/src/envelope.h](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/envelope.h) / [envelope.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/envelope.cpp)

Protocol envelope construction and parsing (host-testable, no Arduino dependencies):
- `buildEnvelope(buf, bufSize, type, nodeId, data)` — constructs `{v:1, type, node, msg_id, ts, data}` into a char buffer
- `generateMsgId(buf, len)` — 16-char hex token (8 random bytes)
- `parseCmd(json, len, &cmd, &jobId, &args)` — extracts command fields from inbound `cmd` envelope
- All functions operate on `char[]` buffers, no `String`, no heap allocation

---

### Phase 2: Worker Task + Recon Commands

The six recon commands from the approved spec, plus control commands and the radio state machine.

---

#### [NEW] [firmware/src/worker.h](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/worker.h) / [worker.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/worker.cpp)

Worker task (core 1):
- FreeRTOS task pinned to core 1, 8KB stack
- Receives job requests via a `QueueHandle_t` from the MQTT task (single-element queue — one job at a time)
- State: `idle` or `running(job_id)`
- When idle and monitor config exists: checks if a monitor cycle is due
- Job execution flow mirrors `VirtualProbe._run()`:
  1. Set `_busy = true`
  2. Enqueue `accepted` result to outbox
  3. Call the appropriate runner function
  4. Runner yields chunks → enqueue each as `chunk` result with incrementing `seq`
  5. Between chunks, check `_cancelFlag` (set by MQTT task on `cancel` command)
  6. On completion → enqueue `done` with `{chunks, results, duration_ms}`
  7. On error → enqueue `error` with appropriate code
  8. Set `_busy = false`
- Duplicate `job_id` suppression: ring buffer of last 8 job IDs, checked before dispatch

---

#### [NEW] [firmware/src/radio.h](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/radio.h) / [radio.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/radio.cpp)

Radio state machine — the critical piece (spec §6):
- States: `STATION` (normal, MQTT up) and `PROMISCUOUS` (off-AP, sniffing)
- `RadioLock` — single owner arbitration. `tryAcquire(mode)` returns false if already held → caller emits `radio_conflict` error
- **Survey sequence** (spec §6.1 — "the single most important line in this document"):
  1. Publish `accepted` while still connected
  2. Publish retained `status: surveying` with `expect_back_in`
  3. **Clean MQTT DISCONNECT** (suppresses LWT)
  4. Leave AP → enter promiscuous mode
  5. Collect APs via `esp_wifi_scan_start` + clients via promiscuous callback with channel hopping
  6. Reassociate WiFi, reconnect MQTT
  7. Drain survey buffer as chunks through outbox
  8. Publish retained `status: online`

Survey buffer:
- Bounded array of `ApRecord` and `ClientRecord` structs, sized at boot from `ESP.getFreeHeap()`
- Deduplicated by BSSID (APs) and MAC (clients), keeping strongest RSSI
- On overflow: increment `dropped` counter

---

#### [NEW] [firmware/src/runners/](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/runners/)

One file per recon command. Each runner is a function that takes parsed `args` and a callback to emit chunks:

##### [port_scan.h/.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/runners/port_scan.cpp)
- TCP connect scan using lwIP sockets (from Bit-Pirate's `NmapService`)
- Non-blocking `connect()` + `select()` with 600ms timeout
- Classifies: `open` (connect succeeds), `closed` (ECONNREFUSED), `filtered` (timeout)
- Concurrency clamped to 8 (lwIP `CONFIG_LWIP_MAX_SOCKETS`)
- Port spec parsing: `"22,80,443,8000-8100"` — reuses `protocol/ports.py` logic in C++
- Chunks: `{"open": [{"host", "port", "state", "rtt_ms"}]}`

##### [banner_grab.h/.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/runners/banner_grab.cpp)
- Raw TCP connect, read first 256 bytes with 2s timeout
- Chunks: `{"banners": [{"host", "port", "text", "bytes", "truncated"}]}`

##### [dns.h/.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/runners/dns.cpp)
- UDP DNS query builder and response parser (host-testable)
- Handles A, AAAA, CNAME, MX, TXT, NS, PTR records
- Name compression pointer support
- Chunks: `{"answers": [{"name", "type", "ttl", "value"}]}`

##### [trace.h/.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/runners/trace.cpp)
- ICMP traceroute using raw sockets with incrementing TTL
- **Risk §10.1**: `CONFIG_LWIP_RAW` may not be available. First build spike tests this.
- If unavailable: not advertised in `capabilities`, server disables gracefully
- Chunks: `{"hops": [{"ttl", "addr", "rtt_ms", "timeout"}]}`

##### [discover.h/.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/runners/discover.cpp)
- ICMP ping sweep across subnet
- ARP cache inspection as fallback
- Chunks: `{"hosts": [{"ip", "mac", "rtt_ms", "method"}]}`

##### [wifi_survey.h/.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/runners/wifi_survey.cpp)
- Orchestrates the radio state machine survey sequence
- AP scan: `esp_wifi_scan_start()` → `esp_wifi_scan_get_ap_records()` (from Bit-Pirate's `WifiService::scanDetailedNetworks`)
- Client sniffing: `esp_wifi_set_promiscuous(true)` + callback parsing 802.11 frame addresses (from Bit-Pirate's `snifferCallback`)
- Channel hopping across requested channels, 200ms dwell per channel
- Chunks: `{"aps": [...], "clients": [...]}`

##### [monitor.h/.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/runners/monitor.cpp)
- Scheduled monitoring cycles (spec §8.3)
- Config persisted in NVS, survives reboots
- Per-device: ICMP ping + TCP port probes
- Results: `{"cycle_ts", "results": [{"id", "status", "latency_ms", "ports"}]}`

---

### Phase 3: Extended Capabilities — The Bit-Pirate Superpowers

Three new commands that elevate the capstone from "distributed scanner" to "distributed security platform." Each requires protocol schema additions, server changes, and firmware runners.

---

#### Capability 1: `wifi_deauth` — Distributed Deauthentication Attack

**What it does**: On command from C2, a probe kicks all clients off a target AP by injecting IEEE 802.11 deauthentication frames. In a fleet, you can deauth from multiple vantage points simultaneously — something no single-device tool can do.

**Technique** (directly from Bit-Pirate's [`WifiService::deauthAttack()`](file:///C:/Users/alityan/.gemini/antigravity/scratch/ESP32-Bit-Pirate/src/Services/WifiService.cpp)):
1. Switch to `WIFI_MODE_AP` (required for raw frame TX)
2. Set channel to target AP's channel
3. Enter promiscuous mode briefly to discover connected client stations (sniff for `To-DS=1` data frames matching target BSSID)
4. Craft 26-byte deauth management frame: `0xC0,0x00` (Frame Control) + target MAC + source BSSID + reason code `0x02`
5. Inject via `esp_wifi_80211_tx(WIFI_IF_AP, pkt, 26, true)`
6. Override ESP-IDF sanity check: `extern "C" int ieee80211_raw_frame_sanity_check(int32_t, int32_t, int32_t) { return 0; }`
7. Send broadcast deauth + unicast deauth to each discovered station
8. Reconnect to own AP, resume MQTT

**Command schema** (`cmd.args`):
```json
{
  "target_bssid": "AA:BB:CC:DD:EE:FF",
  "channel": 6,
  "duration_s": 10,
  "bursts": 5,
  "confirm": true
}
```
- `confirm: true` is mandatory — server rejects without it (safety interlock)
- `duration_s` caps the attack window

**Result chunks**:
```json
{
  "event": "chunk", "seq": 0,
  "deauth": {
    "target_bssid": "AA:BB:CC:DD:EE:FF",
    "channel": 6,
    "clients_found": 3,
    "frames_sent": 18,
    "duration_ms": 10200
  }
}
```

**Radio interaction**: Uses the same survey disconnect sequence (publish `accepted` → retained status `"attacking"` → clean DISCONNECT → attack → reconnect → report). This is critical — without the clean disconnect, the LWT fires and the server thinks the node died.

##### Files:

###### [NEW] [firmware/src/runners/wifi_deauth.h/.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/runners/wifi_deauth.cpp)
- Client discovery via promiscuous sniffing (Bit-Pirate's `clientSnifferCallback` pattern)
- Raw deauth frame construction and injection
- Sanity check override
- ~150 lines

###### [NEW] [protocol/schemas/cmd_wifi_deauth.schema.json](file:///C:/Users/alityan/OneDrive/Desktop/nms/protocol/schemas/cmd_wifi_deauth.schema.json)
- Schema for `wifi_deauth` command args

###### [MODIFY] [server/commands.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/commands.py)
- Add `wifi_deauth` to the recon command dispatch
- Enforce `confirm: true` validation

###### [MODIFY] [server/ingest.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/ingest.py)
- Handle `deauth` chunk key in result processing

---

#### Capability 2: `ble_scan` — Distributed BLE Device Enumeration

**What it does**: Scans for BLE (Bluetooth Low Energy) devices in range, reporting name, MAC, RSSI, advertisement data, and connectability. With multiple probes, you get **multi-vantage BLE correlation** — the same Bluetooth device seen by three probes at different signal strengths, enabling rough location estimation.

**Technique** (from Bit-Pirate's [`BluetoothService::scanDevices()`](file:///C:/Users/alityan/.gemini/antigravity/scratch/ESP32-Bit-Pirate/src/Services/BluetoothService.cpp)):
1. Initialize BLE stack: `BLEDevice::init("nms-probe")`
2. Get scanner: `BLEScan* scan = BLEDevice::getScan()`
3. Configure active scan: `scan->setActiveScan(true)` (sends scan requests for richer data)
4. Execute: `BLEScanResults* results = scan->start(duration_s)`
5. For each device: extract name, MAC, RSSI, parse AD type flags for connectability
6. Release: `scan->clearResults()`
7. Deinitialize BLE to free RAM: `BLEDevice::deinit()`

**BLE + WiFi coexistence**: The ESP32's radio controller handles coexistence automatically, but performance degrades. BLE scan runs while WiFi stays connected (MQTT alive), just slower. No survey-style disconnect needed.

**Command schema**:
```json
{
  "duration_s": 10,
  "active": true
}
```

**Result chunks**:
```json
{
  "event": "chunk", "seq": 0,
  "devices": [
    {
      "mac": "AA:BB:CC:DD:EE:FF",
      "name": "iPhone",
      "rssi": -62,
      "connectable": true,
      "ad_flags": 6,
      "manufacturer": "004C"
    }
  ]
}
```

**Server-side BLE correlation**: New `ble_observations` table paralleling `ap_observations` — same BSSID/MAC seen by multiple probes with different RSSI values. New API endpoint `GET /api/rf/ble` returns grouped observations.

##### Files:

###### [NEW] [firmware/src/runners/ble_scan.h/.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/runners/ble_scan.cpp)
- BLE scan using ESP32 BLE Arduino library
- AD payload parsing for connectability, manufacturer data, service UUIDs
- ~120 lines

###### [NEW] [protocol/schemas/cmd_ble_scan.schema.json](file:///C:/Users/alityan/OneDrive/Desktop/nms/protocol/schemas/cmd_ble_scan.schema.json)
- Schema for `ble_scan` command args

###### [MODIFY] [server/models.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/models.py)
- Add `BleObservation` model (mac, name, rssi, connectable, manufacturer, node_id, job_id, observed_at)

###### [MODIFY] [server/ingest.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/ingest.py)
- Project `ble_scan` chunks into `ble_observations` table (like `ap_observations` for wifi_survey)

###### [MODIFY] [server/api.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/api.py)
- `GET /api/rf/ble` — BLE devices grouped by MAC with multi-node RSSI, parallel to `/api/rf/aps`

---

#### Capability 3: `wifi_ids` — Distributed Wireless Intrusion Detection

**What it does**: Enters promiscuous mode and watches for **security-relevant 802.11 events**: deauthentication floods, rogue access points (APs not in a known-good list), evil twin attacks (known SSID on unexpected BSSID), and excessive probe requests (device tracking). This turns the fleet into a **distributed wireless IDS** — the most impressive capability for a capstone.

**Technique** (extends Bit-Pirate's [`snifferCallback()`](file:///C:/Users/alityan/.gemini/antigravity/scratch/ESP32-Bit-Pirate/src/Services/WifiService.cpp) with security classification):
1. Enter promiscuous mode (same survey disconnect sequence)
2. Register callback that classifies every frame:
   - **Deauth/Disassoc detection**: Frame type 0 (management), subtype 0xC (deauth) or 0xA (disassoc) → count per source MAC per channel
   - **Rogue AP detection**: Beacon frames (subtype 0x8) with BSSID not in the `known_aps` allow-list → flag as rogue
   - **Evil twin detection**: Beacon with known SSID but unknown BSSID → flag as evil twin
   - **Probe request tracking**: Subtype 0x4 → log station MAC + requested SSID
3. Hop channels 1–13, dwell 200ms each
4. After `duration_s`: reassociate, reconnect, stream results

**Command schema**:
```json
{
  "duration_s": 60,
  "channels": [1, 6, 11],
  "known_aps": [
    {"bssid": "AA:BB:CC:DD:EE:FF", "ssid": "LabNetwork"}
  ]
}
```

**Result chunks**:
```json
{
  "event": "chunk", "seq": 0,
  "alerts": [
    {
      "type": "deauth_flood",
      "source_mac": "DE:AD:BE:EF:00:01",
      "target_mac": "FF:FF:FF:FF:FF:FF",
      "channel": 6,
      "count": 47,
      "first_seen": 1755302400,
      "last_seen": 1755302430
    },
    {
      "type": "rogue_ap",
      "bssid": "11:22:33:44:55:66",
      "ssid": "Free WiFi",
      "channel": 1,
      "rssi": -45
    },
    {
      "type": "evil_twin",
      "bssid": "11:22:33:44:55:66",
      "ssid": "LabNetwork",
      "expected_bssid": "AA:BB:CC:DD:EE:FF",
      "channel": 6,
      "rssi": -40
    }
  ],
  "frame_stats": {
    "total": 15420,
    "management": 2100,
    "data": 12800,
    "control": 520,
    "deauth": 47,
    "probe_request": 312
  }
}
```

##### Files:

###### [NEW] [firmware/src/runners/wifi_ids.h/.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/src/runners/wifi_ids.cpp)
- Extended promiscuous callback with frame classification
- Deauth counter per source MAC (bounded ring buffer)
- Known-AP matching for rogue/evil twin detection
- Frame statistics accumulator
- ~250 lines

###### [NEW] [protocol/schemas/cmd_wifi_ids.schema.json](file:///C:/Users/alityan/OneDrive/Desktop/nms/protocol/schemas/cmd_wifi_ids.schema.json)
- Schema for `wifi_ids` command args

###### [MODIFY] [server/models.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/models.py)
- Add `IdsAlert` model (type, source_mac, target_mac, channel, count, node_id, job_id, detected_at)

###### [MODIFY] [server/ingest.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/ingest.py)
- Project `wifi_ids` alert chunks into `ids_alerts` table

###### [MODIFY] [server/api.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/api.py)
- `GET /api/security/alerts` — IDS alerts with fleet-wide correlation
- `GET /api/security/rogue-aps` — Rogue AP detections across probes

---

### Phase 4: Protocol & Server Extensions (Cross-Cutting)

Changes needed across `protocol/` and `server/` to support the three new capabilities.

---

#### [MODIFY] [protocol/schemas/announce.schema.json](file:///C:/Users/alityan/OneDrive/Desktop/nms/protocol/schemas/announce.schema.json)
- Add `"wifi_deauth"`, `"ble_scan"`, `"wifi_ids"` to the capabilities enum

#### [MODIFY] [protocol/schemas/result.schema.json](file:///C:/Users/alityan/OneDrive/Desktop/nms/protocol/schemas/result.schema.json)
- Add `"deauth"`, `"devices"`, `"alerts"`, `"frame_stats"` as valid chunk payload keys

#### [MODIFY] [protocol/schemas/status.schema.json](file:///C:/Users/alityan/OneDrive/Desktop/nms/protocol/schemas/status.schema.json)
- Add `"attacking"` to the state enum (for wifi_deauth radio window)

#### [NEW] [protocol/golden/](file:///C:/Users/alityan/OneDrive/Desktop/nms/protocol/golden/) (new fixtures)
- Valid and invalid fixtures for each new command and result type
- Byte-identical contract between Python validator and C++ firmware

#### [MODIFY] [server/commands.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/commands.py)
- Register new commands in dispatch table
- `wifi_deauth`: validate `confirm: true` before publishing

#### [MODIFY] [server/ingest.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/ingest.py)
- Handle new chunk payload keys: `deauth`, `devices`, `alerts`, `frame_stats`
- Project into new tables

#### [MODIFY] [server/api.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/api.py)
- `GET /api/rf/ble` — BLE observations grouped by MAC with multi-probe RSSI
- `GET /api/security/alerts` — IDS alerts timeline
- `GET /api/security/rogue-aps` — Rogue AP detections
- `POST /api/nodes/<id>/jobs` — accept new command types

#### [MODIFY] [server/models.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/server/models.py)
- `BleObservation` table
- `IdsAlert` table

---

### Phase 5: Testing

Three layers per the approved spec, extended for new capabilities.

---

#### Layer 1: Native Unit Tests (host, no board)

##### [NEW] [firmware/test/test_envelope.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/test/test_envelope.cpp)
Envelope construction, msg_id generation, parsing

##### [NEW] [firmware/test/test_port_parser.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/test/test_port_parser.cpp)
Port spec parsing (`"22,80,8000-8100"`, edge cases like `low > high`)

##### [NEW] [firmware/test/test_outbox.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/test/test_outbox.cpp)
Bounded queue: enqueue, dequeue, overflow drop-oldest, dropped counter

##### [NEW] [firmware/test/test_backoff.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/test/test_backoff.cpp)
Exponential backoff with jitter: bounds checking, ceiling at 60s

##### [NEW] [firmware/test/test_job_state.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/test/test_job_state.cpp)
Job state machine: busy rejection, duplicate job_id suppression, cancel flag, seq counting

##### [NEW] [firmware/test/test_golden_corpus.cpp](file:///C:/Users/alityan/OneDrive/Desktop/nms/firmware/test/test_golden_corpus.cpp)
Read golden fixtures from `protocol/golden/`, validate C++ encoder/decoder produces identical output

#### Layer 2: Golden Corpus (shared fixtures)

The existing `protocol/golden/` fixtures plus new fixtures for `wifi_deauth`, `ble_scan`, `wifi_ids` commands and results. Both `tests/test_golden_corpus.py` and `firmware/test/test_golden_corpus.cpp` read the same files.

#### Layer 3: Conformance over MQTT (live board)

##### [MODIFY] [tests/test_conformance.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/tests/test_conformance.py)
- Add `--node-id`, `--broker-host`, `--broker-port` pytest options
- When pointed at external broker: skip Docker container fixture, skip in-process probe
- New scenarios for extended capabilities:
  - **Scenario 7**: `ble_scan` dispatched, chunks received with `devices` key
  - **Scenario 8**: `wifi_deauth` dispatched with `confirm: true`, probe disconnects and reconnects, `deauth` chunk received
  - **Scenario 9**: `wifi_ids` dispatched, probe enters promiscuous mode, reconnects, `alerts` + `frame_stats` chunks received

#### Python Test Extensions

##### [MODIFY] [tests/test_api.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/tests/test_api.py)
- Tests for `/api/rf/ble`, `/api/security/alerts`, `/api/security/rogue-aps`

##### [NEW] [tests/test_ingest_extended.py](file:///C:/Users/alityan/OneDrive/Desktop/nms/tests/test_ingest_extended.py)
- Ingest handlers for `deauth`, `devices`, `alerts` chunk types
- BLE observation projection
- IDS alert projection

---

## Verification Plan

### Automated Tests

```bash
# Phase 1-2: Firmware native tests (no board needed)
cd firmware && pio test -e native

# Phase 4: Python protocol + server tests
cd .. && python -m pytest tests/ -q

# Phase 5: Conformance against hardware (board + broker required)
python -m pytest tests/test_conformance.py --node-id=probe-a4c1f8 --broker-host=192.168.1.100 --broker-port=1883
```

### Manual Verification

1. **Provisioning flow**: Flash board → SoftAP appears → connect phone → fill form → board reboots and connects to MQTT
2. **Port scan from C2**: `POST /api/nodes/probe-a4c1f8/jobs {"cmd":"port_scan","args":{"targets":["192.168.1.1"],"ports":"22,80,443"}}` → chunks stream back with open/closed/filtered
3. **WiFi survey**: Dispatch survey → node publishes `surveying` status → disconnects → reconnects → AP and client list appears
4. **BLE scan**: Dispatch → node scans for 10s → BLE devices appear in `/api/rf/ble` with RSSI
5. **WiFi deauth demo** (isolated lab AP): Dispatch with `confirm:true` → client devices lose connection → `deauth` chunk reports clients found and frames sent
6. **WiFi IDS demo**: Run deauth from another device → dispatch `wifi_ids` to probe → probe detects and reports `deauth_flood` alert
7. **Multi-probe correlation**: Same AP/BLE device seen by 3 probes at different RSSI → visible in `/api/rf/aps` and `/api/rf/ble`

---

## Implementation Order

| Step | What | Depends On | Estimated Effort |
|------|------|-----------|-----------------|
| 1 | PlatformIO skeleton + identity + config/NVS | Nothing | 2h |
| 2 | Captive portal provisioning | Step 1 | 3h |
| 3 | MQTT client + outbox + envelope | Step 1 | 4h |
| 4 | Worker task + job state machine | Step 3 | 3h |
| 5 | `port_scan` + `banner_grab` runners | Step 4 | 4h |
| 6 | `dns` + `discover` runners | Step 4 | 3h |
| 7 | `trace` runner (spike raw socket first) | Step 4 | 2h |
| 8 | Radio state machine + `wifi_survey` runner | Step 4 | 5h |
| 9 | Monitor cycle runner + NVS persistence | Step 4 | 3h |
| 10 | Native unit tests (layers 1 + 2) | Steps 3–9 | 4h |
| 11 | Conformance suite retargeting | Step 10 | 2h |
| 12 | **Protocol extensions** (schemas, golden fixtures) | Nothing | 2h |
| 13 | **`wifi_deauth` runner** | Steps 8, 12 | 4h |
| 14 | **`ble_scan` runner** | Steps 4, 12 | 3h |
| 15 | **`wifi_ids` runner** | Steps 8, 12 | 5h |
| 16 | **Server extensions** (models, ingest, API) | Step 12 | 4h |
| 17 | Extended conformance scenarios | Steps 13–16 | 3h |
| 18 | NOTICE file (Bit-Pirate attribution) | Nothing | 0.5h |
| **Total** | | | **~56h** |

Steps 12 and 1–4 can run in parallel. Steps 5–9 and 13–15 can partially overlap.
