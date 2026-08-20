// Persisted probe configuration in NVS.
//
// The operator transcribes broker coordinates and the server-issued MQTT
// credentials into the captive portal; this is where they land so
// they survive reboots. The firmware never invents credentials.
#pragma once

#include <stdint.h>

// NVS namespace holding the config blob. Kept as a named constant because
// clearConfig() (factory reset) must target exactly this namespace.
static const char* NVS_NAMESPACE = "nms";

// Field sizes include the trailing NUL. SSID and WPA2 passphrase limits come
// from 802.11 (32 / 63 chars); label matches announce.schema.json (maxLength 64).
struct ProbeConfig {
    char ssid[33];         // WiFi SSID (max 32 chars + NUL)
    char wifi_pass[65];    // WiFi passphrase (max 63 chars, open network -> empty)
    char broker_host[64];  // Mosquitto host (IP or name)
    uint16_t broker_port;  // Mosquitto port (typically 1883)
    char mqtt_user[32];    // MQTT username (defaults to node_id in the portal)
    char mqtt_pass[64];    // MQTT password (server-generated, transcribed once)
    char label[65];        // Optional friendly label (announce, maxLength 64)
    bool valid;            // True once a config has been saved and re-read
};

// Loads config from NVS into `out`. On a fresh device (or after clearConfig)
// the namespace is empty: `out` is zeroed and out.valid is false, which the
// boot sequence reads as `unprovisioned`. Returns out.valid.
bool loadConfig(ProbeConfig& out);

// Writes `cfg` to NVS as a single blob and marks it valid. Returns true on a
// successful commit. Called by the portal's POST /save handler before reboot.
bool saveConfig(const ProbeConfig& cfg);

// Erases the config namespace, returning the node to `unprovisioned`. This is
// the GPIO0-hold recovery path for a changed broker IP or a
// mistyped password. Returns true on success.
bool clearConfig();
