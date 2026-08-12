#include "wifi_ids.h"
#include "../radio.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <ArduinoJson.h>

#if defined(ESP_PLATFORM) || defined(ARDUINO)
#define NMS_HAS_RADIO 1
#include <Arduino.h>  // ESP.getFreeHeap()
#else
#define NMS_HAS_RADIO 0
#endif

static const size_t ALERT_CAP_MAX = 128;
static const size_t KNOWN_MAX = 32;
static const uint8_t DEFAULT_IDS_CHANNELS[] = {1, 6, 11};

static void macToStr(const uint8_t* m, char* out) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Parse "AA:BB:CC:DD:EE:FF" (case-insensitive) into 6 bytes. Returns false on any
// malformed field so a bad known_aps entry is skipped rather than mis-matched.
static bool parseMac(const char* s, uint8_t out[6]) {
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

bool runWifiIds(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                void* sink, uint32_t* resultsOut,
                char* errCode, size_t errCodeSize,
                char* errMsg, size_t errMsgSize) {
    (void)cancelled;  // runs with MQTT down, so no cancel can arrive mid-window
    JsonDocument args;
    if (deserializeJson(args, ctx.args) != DeserializationError::Ok) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "args are not valid JSON");
        return false;
    }
    int durationS = args["duration_s"] | 30;
    if (durationS < 1 || durationS > 300) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "duration_s must be 1..300");
        return false;
    }

    // Requested channels, else the 1/6/11 non-overlapping default.
    uint8_t channels[14];
    size_t nCh = 0;
    JsonArrayConst chans = args["channels"].as<JsonArrayConst>();
    if (!chans.isNull()) {
        for (JsonVariantConst c : chans) {
            int ch = c.as<int>();
            if (ch >= 1 && ch <= 14 && nCh < sizeof(channels)) channels[nCh++] = (uint8_t)ch;
        }
    }
    if (nCh == 0) {
        memcpy(channels, DEFAULT_IDS_CHANNELS, sizeof(DEFAULT_IDS_CHANNELS));
        nCh = sizeof(DEFAULT_IDS_CHANNELS);
    }

    // Operator allow-list. A malformed bssid entry is skipped, not fatal.
    static IdsKnownAp known[KNOWN_MAX];
    size_t nKnown = 0;
    JsonArrayConst kaps = args["known_aps"].as<JsonArrayConst>();
    if (!kaps.isNull()) {
        for (JsonVariantConst k : kaps) {
            if (nKnown >= KNOWN_MAX) break;
            const char* bssid = k["bssid"] | (const char*)nullptr;
            if (!parseMac(bssid, known[nKnown].bssid)) continue;
            const char* ssid = k["ssid"] | "";
            strncpy(known[nKnown].ssid, ssid, sizeof(known[nKnown].ssid) - 1);
            known[nKnown].ssid[sizeof(known[nKnown].ssid) - 1] = '\0';
            ++nKnown;
        }
    }

#if NMS_HAS_RADIO
    // Size the alert buffer from free heap, clamped (§7 memory discipline).
    size_t cap = (ESP.getFreeHeap() / 12) / sizeof(IdsAlert);
    if (cap > ALERT_CAP_MAX) cap = ALERT_CAP_MAX;
    if (cap < 16) cap = 16;

    IdsBuffers buf;
    memset(&buf, 0, sizeof(buf));
    buf.alerts = (IdsAlert*)malloc(cap * sizeof(IdsAlert));
    if (buf.alerts == nullptr) {
        snprintf(errCode, errCodeSize, "oom");
        snprintf(errMsg, errMsgSize, "IDS alert buffer allocation failed");
        return false;
    }
    buf.alertCap = cap;

    bool ok = radioRunIds(ctx.job_id, (uint16_t)durationS, channels, nCh,
                          known, nKnown, buf, errCode, errCodeSize, errMsg, errMsgSize);
    if (!ok) {  // radio_conflict — nothing collected
        free(buf.alerts);
        return false;
    }

    // Serialize alerts into <=1024-byte chunks; fold the overflow drop count into
    // the first chunk (§8.5).
    static char out[900];
    char m1[18], m2[18], m3[18];
    bool droppedEmitted = false;

    size_t i = 0;
    while (i < buf.alertCount) {
        JsonDocument doc;
        JsonArray arr = doc["alerts"].to<JsonArray>();
        for (; i < buf.alertCount; ++i) {
            const IdsAlert& a = buf.alerts[i];
            JsonObject o = arr.add<JsonObject>();
            if (a.kind == IDS_DEAUTH_FLOOD) {
                o["type"] = "deauth_flood";
                macToStr(a.src, m1); o["source_mac"] = m1;
                macToStr(a.dst, m2); o["target_mac"] = m2;
                o["channel"] = a.channel;
                o["count"] = a.count;
                o["first_seen"] = a.firstSeen;
                o["last_seen"] = a.lastSeen;
            } else if (a.kind == IDS_EVIL_TWIN) {
                o["type"] = "evil_twin";
                macToStr(a.src, m1); o["bssid"] = m1;
                o["ssid"] = a.ssid;
                macToStr(a.expected, m3); o["expected_bssid"] = m3;
                o["channel"] = a.channel;
                o["rssi"] = a.rssi;
            } else {  // IDS_ROGUE_AP
                o["type"] = "rogue_ap";
                macToStr(a.src, m1); o["bssid"] = m1;
                o["ssid"] = a.ssid;
                o["channel"] = a.channel;
                o["rssi"] = a.rssi;
            }
            if (measureJson(doc) > 760) { ++i; break; }
        }
        if (!droppedEmitted && buf.alertDropped > 0) {
            doc["dropped"] = buf.alertDropped;
            droppedEmitted = true;
        }
        size_t len = serializeJson(doc, out, sizeof(out));
        if (len > 0) emit(sink, out);
    }

    // Frame statistics always close the job as a final chunk, so an IDS run that
    // raised no alerts still reports what it saw (and always emits >=1 chunk).
    {
        JsonDocument doc;
        JsonObject fs = doc["frame_stats"].to<JsonObject>();
        fs["total"] = buf.total;
        fs["management"] = buf.mgmt;
        fs["data"] = buf.data;
        fs["control"] = buf.ctrl;
        fs["deauth"] = buf.deauth;
        fs["probe_request"] = buf.probeReq;
        if (!droppedEmitted && buf.alertDropped > 0) doc["dropped"] = buf.alertDropped;
        size_t len = serializeJson(doc, out, sizeof(out));
        if (len > 0) emit(sink, out);
    }

    *resultsOut = (uint32_t)buf.alertCount;
    free(buf.alerts);
    return true;
#else
    (void)emit; (void)sink; (void)channels; (void)nCh; (void)known; (void)nKnown;
    *resultsOut = 0;
    snprintf(errCode, errCodeSize, "unsupported");
    snprintf(errMsg, errMsgSize, "wifi_ids requires the ESP32 radio (device build)");
    return false;
#endif
}
