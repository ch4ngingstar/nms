// Firmware entry point.
//
// setup() reads NVS and forks: unprovisioned -> captive portal; provisioned ->
// join WiFi, sync time, connect MQTT (with Last Will), announce, and spawn the
// worker task on core 1. loop() is the MQTT task on core 0: it services
// PubSubClient, drains the outbox, and emits telemetry.

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "identity.h"
#include "portal.h"
#include "outbox.h"
#include "mqtt_client.h"
#include "worker.h"

// Classic ESP32 WROOM-32 onboard LED — plain GPIO, not the S3's addressable
// WS2812. Used for the `identify` blink.
static const int LED_PIN = 2;

static ProbeConfig g_cfg;
static bool g_provisioned = false;

// The worker task (core 1) and its recon job lifecycle now live in worker.cpp;
// setup() spawns it via workerStart() once MQTT is initialized.

// --- bring-up helpers ------------------------------------------------------

static void connectWifi(const ProbeConfig& cfg) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.ssid, cfg.wifi_pass);

    uint32_t attempt = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        // Re-issue begin periodically in case the first association was missed.
        if (++attempt % 20 == 0) {
            WiFi.disconnect();
            WiFi.begin(cfg.ssid, cfg.wifi_pass);
        }
    }
}

static void syncTime() {
    // ts must be real Unix seconds (minimum 1e9). Pull it from
    // SNTP; without this every envelope would carry seconds-since-boot and fail
    // validation server-side.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    uint32_t waited = 0;
    while (time(nullptr) < 1000000000 && waited < 15000) {
        delay(250);
        waited += 250;
    }
}

// --- Arduino lifecycle -----------------------------------------------------

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    outboxInit();

    g_provisioned = loadConfig(g_cfg);
    if (!g_provisioned) {
        // Unprovisioned: raise the SoftAP portal and stay there until the
        // operator submits the form, which saves NVS and reboots.
        startPortal();
        return;
    }

    connectWifi(g_cfg);
    syncTime();

    mqttInit(g_cfg);

    // Spawn the worker on core 1; the MQTT task is this Arduino loop() on the
    // other core. The core split keeps a long scan from starving MQTT keepalive.
    workerStart();

    // First connect happens on the first loop() tick via the backoff path.
}

void loop() {
    if (!g_provisioned) {
        handlePortal();
        return;
    }

    if (!mqttConnected()) {
        digitalWrite(LED_PIN, LOW);
        mqttReconnectWithBackoff();
        return;
    }

    mqttLoop();
    mqttDrainOutbox();
    mqttEmitTelemetry();

    // identify blink: toggle a few times a second while the window is open,
    // otherwise hold the LED off.
    if (mqttIdentifyActive()) {
        digitalWrite(LED_PIN, (millis() / 250) % 2 ? HIGH : LOW);
    } else {
        digitalWrite(LED_PIN, LOW);
    }
}
