# Firmware Probe — Design Spec

**Subsystem 3 of 4** · Status: **approved, ready for implementation planning** · Date: 2026-08-17

## 1. Scope

The ESP32 firmware probe: a hardware node that provisions itself over a captive portal, authenticates to Mosquitto as itself, executes recon jobs, monitors devices on its own schedule, and speaks [Probe Protocol v1](2026-08-16-probe-protocol-design.md) well enough to pass the conformance suite the Python virtual probe already passes.

Not in scope: the web interface (subsystem 4). The server and protocol are finished and are treated here as fixed contracts — **this subsystem changes neither**, with one exception: retargeting the conformance suite at hardware (§9.3).

## 2. Context

Subsystem 1 delivered the `protocol` package — schemas, golden corpus, topics, port parsing, credentials. Subsystem 2 delivered the C2 server, storage, REST/SSE API, and `probe-server`, the Python virtual probe. 257 tests pass; the four conformance scenarios skip pending a running Docker daemon.

The virtual probe is the reference implementation. `probe/virtual_probe.py` is 177 lines and holds the entire protocol behaviour — announce, status, the job state machine, busy rejection, cancellation, monitor-config persistence — with the publisher injected. **This firmware is a second implementation of that same behaviour**, and where the two disagree, the golden corpus decides.

## 3. Locked decisions

| Decision | Choice | Rationale |
|---|---|---|
| **Target chip** | **Classic ESP32 (WROOM-32), not ESP32-S3** | See §3.1. This is a deviation from subsystems 1 and 2, and is deliberate. |
| Framework | Arduino on PlatformIO | Fastest path to working firmware. `esp_wifi_*` radio APIs remain fully available as direct C calls because Arduino-ESP32 is built on ESP-IDF, so nothing is given up on the radio side — only boilerplate. |
| Concurrency | Two FreeRTOS tasks, bounded queue between them | A blocking scan inside `loop()` starves MQTT keepalive and fires the Last Will, reporting a node dead while it works. Structural fix, not a careful one. §5. |
| Buffering | One bounded outbox serves both offline buffering and cross-task handoff | §8.5 already mandates a bounded drop-oldest queue. Reusing it as the task boundary means one buffering mechanism to keep correct, not two. |
| MQTT ownership | The MQTT task is the sole writer to `PubSubClient` | `PubSubClient` is not thread-safe. Single-writer discipline removes the need for a mutex. |
| `wifi_survey` technique | `esp_wifi_scan_start` for APs, promiscuous sniffing for clients, in one radio window | Scan returns `wifi_ap_record_t` with `authmode` already decoded, which is far more reliable than parsing RSN/WPA information elements out of raw beacons. Scan cannot see client stations, and the client list is the novel data. One disconnect yields both. §6.3. |
| Capabilities | All six recon commands | Full parity with the virtual probe. `trace` is conditional on §10.1. |
| PSRAM | Detected at boot, never assumed | Board variant is unconfirmed. The survey buffer is sized from what is found and the result reported in `announce`. |
| Heap rules | No `String` in job or publish paths; static `ArduinoJson` documents; raised `PubSubClient` buffer | §7.5's one-job-at-a-time rule exists because of heap headroom. These are the constraints that make it hold. §7. |
| Testing | Three layers, majority host-run | §9. Firmware is where test discipline usually collapses into flash-and-squint. |

### 3.1 Deviation: ESP32 rather than ESP32-S3

Subsystems 1 and 2 name the ESP32-S3 throughout. The hardware actually in hand is a classic ESP32. This subsystem targets the classic part, and the choice costs nothing:

| Requirement | Classic ESP32 |
|---|---|
| WiFi station + MQTT | Yes |
| `esp_wifi_set_promiscuous` and the RX callback | Yes — monitor mode is not an S3-only feature |
| `esp_wifi_scan_start` | Yes |
| TCP connect scanning, banner grab | Yes, lwIP sockets |
| ICMP ping, DNS, TTL manipulation | Yes — `ping_sock` and `IP_TTL` are lwIP, not chip-specific |
| SoftAP captive portal, NVS | Yes |
| `identify` LED | Yes, and simpler: a plain GPIO LED rather than the S3's addressable WS2812 |
| RAM for a bounded survey buffer | 520 KB SRAM, roughly 250–300 KB free heap after the WiFi stack |

The S3 was inherited from ESP32-Bit-Pirate's platform choice (protocol spec §2.1), not derived from a requirement.

**No code change is required anywhere.** `protocol/schemas/announce.schema.json` declares `chip` as any string of length ≥ 1; `esp32s3` appears only in test fixtures and prose examples, never as a constraint. A node announcing `"chip": "esp32"` is protocol-valid today.

Where a second board is added later, `node_id` is MAC-derived, so flashing the same firmware is the entire enrolment step on the node side.

## 4. Identity and provisioning

### 4.1 Identity

`node_id` is `"probe-"` followed by the low three bytes of the factory MAC in lowercase hex, read via `esp_efuse_mac_get_default()`.

This must match `protocol/topics.py` exactly, whose regex is `^probe-(?:[0-9a-f]{6}|server)$`. A mismatch is not a soft failure — every topic construction and every ACL entry is rejected.

`fw` must satisfy `^[0-9]+\.[0-9]+\.[0-9]+$` (announce schema). Firmware ships as `1.0.0`.

### 4.2 Boot sequence

```
boot
 └─► read NVS config
      ├─ absent ──► state `unprovisioned`
      │              SoftAP `nms-probe-<id>` + captive portal on 192.168.4.1
      │              operator submits form ──► write NVS ──► reboot
      └─ present ─► state `connecting`
                     join WiFi (backoff §8.1)
                     connect MQTT, Last Will registered on `status`
                     subscribe `cmd`, publish `announce`, publish retained `status: online`
                     load monitor config from NVS, start scheduler
                     enter the two-task steady state (§5)
```

### 4.3 Captive portal

SoftAP plus a wildcard `DNSServer` pointing every lookup at the device, and a `WebServer` serving one form. Fields: WiFi SSID, WiFi password, broker host, broker port, MQTT username (defaults to `node_id`), MQTT password, and the optional friendly label.

Credentials come from `POST /api/nodes` on the server, which generates the password and the matching Mosquitto ACL block and displays the plaintext exactly once (server spec §11). The operator transcribes them into this portal. The firmware never invents credentials.

All portal fields are re-editable without reflashing. A held boot button (GPIO0) for five seconds clears NVS and returns the node to `unprovisioned`, which is the recovery path when a broker IP changes or a password is mistyped.

## 5. Task topology

Two FreeRTOS tasks, pinned to separate cores, with one queue between them.

```
        ┌──────────────────────────┐         ┌──────────────────────────┐
        │  MQTT task (core 0)      │◄────────│  Worker task (core 1)    │
        │  • PubSubClient.loop()   │ outbox  │  • one job at a time     │
        │  • drains outbox         │ (queue) │  • monitor cycles when   │
        │  • publishes telemetry   │         │    idle                  │
        │  • dispatches inbound cmd│────────►│  • never publishes       │
        └──────────────────────────┘  flags  └──────────────────────────┘
```

**MQTT task** is the only code that touches `PubSubClient`. It services the client, drains the outbox, and emits telemetry every 30 s at QoS 0. Inbound commands are parsed here and handed to the worker as a job request; `cancel` and `identify` are handled immediately without involving the worker, per protocol §7.5.

**Worker task** executes one job at a time and runs monitor cycles when idle. It enqueues result frames onto the outbox and never publishes. Between units of work it checks the cancel flag — the same shape as `VirtualProbe._run` checking `self._cancel_job` between chunks.

**Outbox** is a bounded FreeRTOS queue of pre-serialized frames. On overflow the oldest is dropped and a counter increments; the next published message carries `"dropped": <n>`, per protocol §8.5. Because the queue simply stops draining when MQTT is down, offline buffering needs no separate mechanism.

Duplicate `job_id` suppression lives in the MQTT task: a small ring of recently seen job IDs, checked before dispatch. This is what makes QoS 1 redelivery safe (protocol §7.1).

## 6. Radio scheduling

The radio has two states and cannot be in both.

| State | Meaning |
|---|---|
| `STATION` | Associated to the AP; MQTT up; normal operation |
| `PROMISCUOUS` | Off the AP, sniffing; MQTT down by design |

A single owner arbitrates. Any operation requesting the radio while it is held returns error code `radio_conflict`.

### 6.1 The survey sequence

Protocol §6.4 is mandatory and its steps are not optional:

1. Publish `accepted` for the job — **before** any disconnect, so the server can distinguish a survey in progress from a node that never received the command
2. Publish retained `status` `{"state":"surveying","expect_back_in":<seconds>}`
3. Clean MQTT `DISCONNECT` — this is what suppresses the Last Will
4. Leave the AP; enter `PROMISCUOUS`
5. Collect into a bounded RAM buffer (§6.3)
6. Reassociate, reconnect MQTT
7. Publish the buffered `chunk` frames with `seq` starting at 0, then `done`
8. Publish retained `status` `{"state":"online"}`

Skipping step 3 produces a false node-death alert on every survey. That is the single most important line in this document.

### 6.2 Interaction with monitoring

Monitor cycles falling due during a survey are **skipped, not queued**. The node reports observations it actually made; manufacturing a cycle for a window when the radio was elsewhere would put fiction into the history table. The gap is visible in the data, which is the honest outcome.

### 6.3 Survey collection

Within the single radio window:

- **APs** — `esp_wifi_scan_start` across the requested `channels`, honouring the `passive` argument. Returns `wifi_ap_record_t` with BSSID, SSID, channel, RSSI, and `authmode` already decoded. Mapped to the protocol's `{bssid, ssid, channel, rssi, auth, hidden}`.
- **Clients** — promiscuous mode for the remaining `duration_s`, hopping the requested channels. Data and management frames yield station MACs and their associated BSSID from the address fields, mapped to `{mac, bssid, rssi}`.

Both accumulate into a bounded buffer sized at boot from detected PSRAM, deduplicated by BSSID and by station MAC, keeping the strongest RSSI seen. On overflow the §8.5 `dropped` semantics apply — the firmware must never exhaust the heap to avoid admitting loss.

This survey buffer is distinct from the outbox and does not contradict §3's single-buffering rule. They hold different things at different stages: the survey buffer accumulates **domain records** (AP and client observations) while the radio is off the AP, and is drained by serializing them into chunks; the outbox holds **already-serialized frames** awaiting publication. Survey chunks pass through the outbox like any other frame once they exist.

Chunking obeys the 1024-byte message cap unchanged. The only difference from a streamed job is *when* the chunks are published, not *how* (protocol §7.2).

### 6.4 Techniques adopted from ESP32-Bit-Pirate

Protocol spec §2.1 names ESP32-Bit-Pirate as design inspiration and obliges the project to cite it as a consulted reference. Its source was read for the read-only radio and network-recon patterns; the concrete mapping is:

| Bit-Pirate command | Technique borrowed | Our command |
|---|---|---|
| `scan` | Active AP scan yielding BSSID, channel, RSSI, decoded auth, hidden/open flags | `wifi_survey` (AP list) |
| `sniff` | Passive promiscuous capture, channel-hopping 1–13 | `wifi_survey` (client list) |
| `discovery` | ICMP sweep across a local subnet to enumerate live hosts | `discover` |
| `ping` | ICMP echo for reachability and latency | monitor `ping` check |
| `nmap` | TCP connect scan reporting the **open / closed / filtered** trichotomy | `port_scan`, monitor `port` check |
| `nc` | Raw TCP connect for reading what a service emits | `banner_grab` |
| `lookup mac` | **OUI → vendor resolution** from a MAC's first three bytes | server-side enrichment (§6.5) |

The near one-to-one correspondence is the point: the recon *primitives* are established prior art and the project does not claim them. What the project contributes is everything around them — the fleet, the protocol, the persistence, and the multi-vantage correlation — which Bit-Pirate, a single handheld tool, structurally cannot do.

**Extended capabilities (spec revision 2026-08-17):** Three additional capabilities are adopted from Bit-Pirate's techniques and extended for distributed fleet operation:

| Bit-Pirate command | Technique borrowed | Our command |
|---|---|---|
| `deauth` | Raw 802.11 deauth frame construction via `esp_wifi_80211_tx()`, client discovery via promiscuous sniffing | `wifi_deauth` (C2-gated, `confirm: true` required) |
| `sniff` (extended) | Promiscuous frame classification by type/subtype for security event detection | `wifi_ids` (passive IDS: deauth flood detection, rogue AP, evil twin) |
| `scan` (BLE) | BLE device enumeration via `BLEDevice::getScan()`, AD payload parsing | `ble_scan` (multi-probe BLE correlation) |

`wifi_deauth` and `wifi_ids` use the same radio disconnect sequence as `wifi_survey` (§6.1). `ble_scan` runs with WiFi coexistence (no disconnect needed). All three are dispatched exclusively through the C2 server and advertised in `capabilities`.

Also not adopted: the interactive-console commands (`ssh`, `telnet`, `modbus`, `http analyze`, `webui`, `waterfall`). They serve a one-operator handheld tool; a headless fleet node reporting to a C2 server has no use for an on-device shell.

### 6.5 OUI vendor enrichment

`wifi_survey` reports raw BSSIDs and client MACs, exactly as the protocol already specifies. **Vendor resolution happens on the server, not the probe** — and the reason is the project's thesis in miniature. The IEEE OUI registry is tens of thousands of entries; holding it on an ESP32 with ~250 KB of heap is out of the question, but it is nothing to a server backed by SQLite. So the probe emits `a4:c1:f8:…`, and the server annotates it "Espressif" before it reaches the console.

This turns the `GET /api/rf/aps` screen from a hex dump into readable intelligence — *"three probes see an Apple device and a Cisco AP, at these three signal strengths"* — which is precisely the multi-vantage question no single-device tool can answer.

Placement: this is a **server/UI enhancement (subsystem 2/4), not firmware.** It requires no protocol change and no firmware change — the probe's contract is already correct. It is recorded here because studying Bit-Pirate is what surfaced it, and because the heap argument for pushing it server-side is the same argument that shapes this entire subsystem. It is a recommended follow-up, tracked in §11, and does not gate the firmware.

## 7. Memory discipline

Protocol §7.5 permits one job at a time explicitly because an ESP32 running a scan with an open socket set plus JSON serialization has limited heap headroom. These rules are what make that constraint sufficient:

- **No `String`** in the job execution or publish paths. Arduino's `String` fragments the heap, and fragmentation is what turns a working scan into an `oom` three hours into an uptime.
- **Static `ArduinoJson` documents**, sized at compile time against the 1024-byte message cap.
- **`PubSubClient` buffer raised** past 1024 bytes via `setBufferSize()`. The library defaults to 256, and the failure mode is silent truncation of exactly the large chunks that matter.
- **`port_scan` concurrency clamped** to the lwIP socket budget. `CONFIG_LWIP_MAX_SOCKETS` defaults to 10 on ESP32; a `concurrency` argument above roughly 8 exhausts descriptors and fails in a way that looks like network trouble. The firmware clamps and proceeds rather than erroring — the operator asked for speed, not for a specific descriptor count.
- **Survey buffer bounded** at boot from detected heap and PSRAM.
- `free_heap` is reported in every telemetry message; a downward trend across uptime is the leak signal (protocol §6.5).

## 8. Error handling

### 8.1 Reconnection

WiFi and MQTT reconnection use exponential backoff from 1 s to a 60 s ceiling **with jitter** (protocol §8.4). Jitter is not decorative: three probes returning after a broker restart retry in lockstep and stampede it. The backoff calculator is pure logic and is host-tested.

### 8.2 Error codes

The firmware emits, per protocol §8.3: `busy`, `unsupported`, `bad_args`, `unreachable`, `timeout`, `oom`, `cancelled`, `radio_conflict`.

`unsupported` is returned only for recon commands absent from the advertised `capabilities`. Control commands — `set_monitor`, `cancel`, `identify`, `reboot`, `get_config` — are a mandatory baseline and are never rejected as unsupported (protocol §6.2).

### 8.3 Scheduled monitoring

`set_monitor` writes the configuration to NVS and the node executes it on its own timer, surviving both server restarts and its own reboots (protocol §7.6). Each cycle pings and TCP-probes the listed devices, producing `{id, status, latency_ms, ports}` per device with `latency_ms` null when status is not `up`. Results publish to the `monitor` topic through the outbox, so a broker outage buffers rather than discards them.

The node reports observations and holds no alerting logic. Consecutive-failure interpretation belongs to the server.

## 9. Testing

Three layers, mirroring protocol spec §9. The majority of coverage runs on the development machine with no board attached.

### 9.1 Native unit tests

PlatformIO's `native` environment compiles the host-testable modules on the PC with Unity. No Arduino headers, no network, no board.

Covered: envelope construction and parsing; the port-spec parser (`"22,80,443,8000-8100"`, and `low > high` → `bad_args`); the job state machine including duplicate-`job_id` suppression, busy rejection, and cancel; `seq` counting and chunk size-capping; backoff with jitter; the bounded outbox and its `dropped` counter; monitor-schedule timing; config serialization; and the DNS message builder and response parser, including name-compression pointers.

This is the direct analogue of `server/ingest.py` carrying most of subsystem 2's coverage: the transport owns the connection, the logic owns the semantics, and the logic is what gets tested exhaustively.

### 9.2 Golden corpus

`protocol/golden/` already holds valid and invalid fixtures, and today only Python reads them. The firmware's encoder and decoder are tested against **the same files**, byte-for-byte — valid fixtures must parse, invalid fixtures must be rejected.

This is the strongest available guarantee that the C++ and Python do not drift into mutually incompatible dialects, and it costs almost nothing because the corpus exists.

### 9.3 Conformance over MQTT

`tests/test_conformance.py` runs against the real board. This requires the one change this subsystem makes outside `firmware/`:

The suite currently hardcodes `NODE = "probe-server"`, starts a throwaway Mosquitto container on a free port, and runs `VirtualProbe` in-process. It gains pytest options — `--node-id`, `--broker-host`, `--broker-port` — that, when pointed at an external broker, skip both the container fixture and the in-process probe while leaving the server bridge and every assertion unchanged.

Scenarios 1, 2, 3, 4, and 5 then run against hardware unattended. Scenarios 3 (busy) and 4 (cancel), currently covered only at the unit level in `test_virtual_probe.py` because their timing is non-deterministic over a broker, become genuinely exercisable against a board whose scans take real time.

Scenario 6 (ungraceful kill → Last Will → node marked offline) requires physically removing power, so it runs attended: the test prompts for the unplug and skips if run non-interactively. Inducing the condition in firmware would defeat what the scenario tests.

### 9.4 Development environment

The board reaches Mosquitto on the development PC over home WiFi. This requires `docker-compose.yml` to publish 1883 on the LAN interface rather than loopback, a Windows Firewall inbound rule for it, and the Docker daemon running — which it currently is not. Layers 9.1 and 9.2 are unaffected, so the build is not blocked, but the subsystem is not finished until layer 9.3 has run.

A connectivity smoke test is the first step of bring-up, so a blocked network is diagnosed in minutes rather than mistaken for a firmware bug.

## 10. Risks

### 10.1 `trace` and raw ICMP sockets

`trace` requires a raw ICMP socket to receive time-exceeded messages from intermediate hops, which depends on `CONFIG_LWIP_RAW` being compiled into the Arduino-ESP32 prebuilt libraries. **This is unverified.**

The implementation plan therefore opens with a throwaway spike answering that one question on the actual board, before any `trace` runner is written. If raw sockets are unavailable, `trace` is simply not advertised in `capabilities` — the protocol was designed for exactly this, the server disables the command for that node with no changes, and the virtual probe continues to offer it. Discovering this in an hour is cheap; discovering it three days into a runner is not.

### 10.2 Promiscuous-mode client capture

Client-station capture depends on traffic occurring during the sniff window. A quiet network yields few clients through no fault of the firmware. The AP list, coming from an active scan, does not have this property and is the reliable half of the survey.

## 11. Deliverables

1. `firmware/` — PlatformIO project, module structure per §5 and the split in §9.1
2. Native unit test suite (§9.1)
3. Golden-corpus conformance tests in C++ (§9.2)
4. Conformance suite retargeting: pytest options and external-broker support (§9.3)
5. `NOTICE` — discharging the ESP32-Bit-Pirate attribution obligation required by protocol spec §2.1, which is currently unmet. It records what was borrowed (the read-only recon techniques mapped in §6.4) and what was deliberately not (the attack commands), so the citation is specific rather than a bare link.
6. A flashing and provisioning runbook
7. **Recommended follow-up, not gating this subsystem:** server-side OUI vendor enrichment (§6.5), landing in subsystem 2/4. Needs no firmware or protocol change.

### Extended capability deliverables (spec revision 2026-08-17)

8. `wifi_deauth` runner — raw 802.11 deauth frame construction and injection via `esp_wifi_80211_tx()`, following Bit-Pirate's `WifiService::deauthAttack()` pattern. Safety interlock: `confirm: true` required in command args.
9. `ble_scan` runner — BLE device enumeration using Arduino-ESP32 BLE library, following Bit-Pirate's `BluetoothService::scanDevices()` pattern.
10. `wifi_ids` runner — passive promiscuous-mode frame classification for distributed wireless intrusion detection (deauth flood detection, rogue AP detection, evil twin detection).
11. Protocol schema additions for the three new capabilities (`cmd_wifi_deauth.schema.json`, `cmd_ble_scan.schema.json`, `cmd_wifi_ids.schema.json`).
12. Server extensions: `BleObservation` and `IdsAlert` models, ingest handlers, API endpoints (`/api/rf/ble`, `/api/security/alerts`).

## 12. Out of scope

- **Web interface** — subsystem 4
- **TLS on MQTT, flash encryption** — deferred in protocol spec §11; mbedTLS heap contends with promiscuous-mode packet buffers, which is precisely the contention this chip has least room for
- **OTA firmware update** — three boards within arm's reach do not justify the flash partitioning and rollback machinery
- **~~Packet injection or deauthentication~~** — *(revised 2026-08-17)* `wifi_deauth` is now in scope as a C2-gated capability with `confirm: true` safety interlock. `wifi_ids` (passive frame classification) is also in scope. See §6.4 and §11.
- **Changes to `protocol/` or `server/`** — both are finished contracts, with the exceptions of §9.3 (conformance retargeting) and §11.8–11.12 (schema and server extensions for the three new capabilities)
