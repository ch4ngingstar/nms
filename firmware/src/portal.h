// Captive portal provisioning (spec §4.3).
//
// When the node boots unprovisioned it raises an open SoftAP named
// "nms-probe-<node_id>", answers every DNS lookup with its own address so any
// browser navigation lands on the form, and serves one form that captures the
// WiFi and broker/MQTT coordinates. POST /save persists them and reboots into
// the provisioned path.
#pragma once

// Brings up SoftAP + DNS + web server. Call once from setup() on the
// unprovisioned path. After this returns, pump the portal from loop() with
// handlePortal().
void startPortal();

// Services one DNS request and one HTTP client. Call repeatedly from loop()
// while unprovisioned; it does not block.
void handlePortal();
