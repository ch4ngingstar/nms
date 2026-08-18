# Security design & responsible use

## Scope

All testing is performed on **isolated lab networks owned by the author**, with no
third-party devices or networks involved. This system is built to *detect* wireless
intrusions; it is detection-first by design.

## Protocol threat model

The Probe Protocol v1 contract is validated at every boundary:

- **Identity.** `node_id` matches `^probe-(?:[0-9a-f]{6}|server)$` and is derived from the
  low three bytes of the device MAC. Broker ACLs scope each node to its own topics, so a
  node cannot publish as another.
- **Input validation.** Every inbound payload is schema-validated **before** it touches the
  database; anything that fails validation is dropped at the bridge, not persisted.
- **Idempotency.** MQTT QoS-1 can redeliver. Result chunks are keyed on `(job_id, seq)`, so
  a redelivered chunk is a no-op rather than a duplicate.
- **Bounded payloads.** Producers chunk to keep every published payload under 1024 bytes.
- **Credentials.** The server generates per-node broker credentials and ACL blocks; it never
  writes broker configuration itself and never commits secrets.

## Wireless intrusion detection

The `wifi_ids` runner reports alerts that the server projects into a typed timeline
(`GET /api/security/alerts`):

- **Deauthentication flood** — a burst of 802.11 deauth frames from one source, reported with
  `source_mac`, `target_mac`, `channel`, and a frame `count`:

  ```json
  {"type": "deauth_flood", "source_mac": "de:ad:be:ef:00:11",
   "target_mac": "a0:b7:65:11:22:33", "channel": 6, "count": 240}
  ```

- **Rogue AP** — an access point broadcasting a monitored SSID from an unexpected BSSID.
- **Evil twin** — a clone of a known AP on a different BSSID, carrying the expected BSSID for
  comparison.

The end-to-end path — probe report → typed alert → operator timeline — is covered by
`tests/test_attack_replay.py`.

## Dual-use: wifi_deauth

The firmware implements `wifi_deauth`: a C2-dispatched job that transmits standard 26-byte
802.11 deauthentication frames — broadcast, plus any clients discovered via a short promiscuous
sniff of the target BSSID — for an operator-specified `duration_s` and `bursts`. It is
registered in the worker dispatch table and advertised in `announce.capabilities` like any other
recon command; there is no on-device trigger, so it only runs when the C2 server publishes a
`cmd` for it.

The job schema requires `target_bssid` and `confirm: true`, and the runner rejects the job if
`confirm` is missing or false. This is an explicit-intent check on the command args, not an
authorization boundary — anyone able to publish a `cmd` for a node can set `confirm: true`.
Access control is the broker ACL scoping described under Identity, above, not this flag.

The adopted techniques are standard, publicly documented ESP-IDF / Arduino-ESP32 APIs. The
frame-construction reference is the MIT-licensed ESP32-Bit-Pirate project; a technique-by-
technique record of what was adopted from it is in [`firmware/NOTICE`](../firmware/NOTICE).
