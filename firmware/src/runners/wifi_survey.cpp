#include "wifi_survey.h"
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

static const size_t AP_CAP_MAX = 96;
static const size_t CLIENT_CAP_MAX = 160;
static const uint8_t DEFAULT_CHANNELS[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

static void macToStr(const uint8_t* m, char* out) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

bool runWifiSurvey(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                   void* sink, uint32_t* resultsOut,
                   char* errCode, size_t errCodeSize,
                   char* errMsg, size_t errMsgSize) {
    (void)cancelled;  // a survey runs with MQTT down, so no cancel can arrive mid-window
    JsonDocument args;
    if (deserializeJson(args, ctx.args) != DeserializationError::Ok) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "args are not valid JSON");
        return false;
    }
    int durationS = args["duration_s"] | 0;
    if (durationS < 1 || durationS > 300) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "duration_s must be 1..300");
        return false;
    }
    bool passive = args["passive"] | false;

    // Requested channels, else the 1..13 default (Bit-Pirate handleSniff sweep).
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
        memcpy(channels, DEFAULT_CHANNELS, sizeof(DEFAULT_CHANNELS));
        nCh = sizeof(DEFAULT_CHANNELS);
    }

#if NMS_HAS_RADIO
    // Size the survey buffers from free heap, capped (spec §6.3, §7): use at most
    // ~1/6 of free heap, split between APs and clients.
    size_t budget = ESP.getFreeHeap() / 6;
    size_t apCap = budget / 2 / sizeof(ApRecord);
    size_t clientCap = budget / 2 / sizeof(ClientRecord);
    if (apCap > AP_CAP_MAX) apCap = AP_CAP_MAX;
    if (apCap < 8) apCap = 8;
    if (clientCap > CLIENT_CAP_MAX) clientCap = CLIENT_CAP_MAX;
    if (clientCap < 8) clientCap = 8;

    SurveyBuffers buf;
    memset(&buf, 0, sizeof(buf));
    buf.aps = (ApRecord*)malloc(apCap * sizeof(ApRecord));
    buf.clients = (ClientRecord*)malloc(clientCap * sizeof(ClientRecord));
    if (buf.aps == nullptr || buf.clients == nullptr) {
        free(buf.aps);
        free(buf.clients);
        snprintf(errCode, errCodeSize, "oom");
        snprintf(errMsg, errMsgSize, "survey buffer allocation failed");
        return false;
    }
    buf.apCap = apCap;
    buf.clientCap = clientCap;

    bool ok = radioRunSurvey(ctx.job_id, (uint16_t)durationS, channels, nCh, passive,
                             buf, errCode, errCodeSize, errMsg, errMsgSize);
    if (!ok) {  // radio_conflict — nothing collected
        free(buf.aps);
        free(buf.clients);
        return false;
    }

    // Serialize APs then clients into <=1024-byte chunks; fold the overflow drop
    // count into the first chunk (§8.5).
    static char out[900];
    char mac[18], bss[18];
    uint32_t dropped = buf.apDropped + buf.clientDropped;
    bool droppedEmitted = false;

    size_t i = 0;
    while (i < buf.apCount) {
        JsonDocument doc;
        JsonArray arr = doc["aps"].to<JsonArray>();
        for (; i < buf.apCount; ++i) {
            JsonObject o = arr.add<JsonObject>();
            macToStr(buf.aps[i].bssid, bss);
            o["bssid"] = bss;
            o["ssid"] = buf.aps[i].ssid;
            o["channel"] = buf.aps[i].channel;
            o["rssi"] = buf.aps[i].rssi;
            o["auth"] = radioAuthName(buf.aps[i].auth);
            o["hidden"] = buf.aps[i].hidden;
            if (measureJson(doc) > 760) { ++i; break; }
        }
        if (!droppedEmitted && dropped > 0) { doc["dropped"] = dropped; droppedEmitted = true; }
        size_t len = serializeJson(doc, out, sizeof(out));
        if (len > 0) emit(sink, out);
    }

    size_t j = 0;
    while (j < buf.clientCount) {
        JsonDocument doc;
        JsonArray arr = doc["clients"].to<JsonArray>();
        for (; j < buf.clientCount; ++j) {
            JsonObject o = arr.add<JsonObject>();
            macToStr(buf.clients[j].mac, mac);
            macToStr(buf.clients[j].bssid, bss);
            o["mac"] = mac;
            o["bssid"] = bss;
            o["rssi"] = buf.clients[j].rssi;
            if (measureJson(doc) > 760) { ++j; break; }
        }
        if (!droppedEmitted && dropped > 0) { doc["dropped"] = dropped; droppedEmitted = true; }
        size_t len = serializeJson(doc, out, sizeof(out));
        if (len > 0) emit(sink, out);
    }

    // Always emit at least one chunk so a survey that saw nothing still reports.
    if (buf.apCount == 0 && buf.clientCount == 0) {
        if (dropped > 0) {
            JsonDocument doc;
            doc["aps"].to<JsonArray>();
            doc["clients"].to<JsonArray>();
            doc["dropped"] = dropped;
            size_t len = serializeJson(doc, out, sizeof(out));
            if (len > 0) emit(sink, out);
        } else {
            emit(sink, "{\"aps\":[],\"clients\":[]}");
        }
    }

    *resultsOut = (uint32_t)(buf.apCount + buf.clientCount);
    free(buf.aps);
    free(buf.clients);
    return true;
#else
    (void)emit; (void)sink; (void)passive; (void)channels; (void)nCh;
    *resultsOut = 0;
    snprintf(errCode, errCodeSize, "unsupported");
    snprintf(errMsg, errMsgSize, "wifi_survey requires the ESP32 radio (device build)");
    return false;
#endif
}
