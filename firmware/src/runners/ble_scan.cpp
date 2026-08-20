#include "ble_scan.h"

#include <string.h>
#include <stdio.h>

#include <ArduinoJson.h>

// BLE is only available on the device. The native host build compiles the arg
// parsing but returns `unsupported`, exactly like wifi_survey's radio guard.
#if defined(ESP_PLATFORM) || defined(ARDUINO)
#define NMS_HAS_BLE 1
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#else
#define NMS_HAS_BLE 0
#endif

// Copy a byte string into `out` keeping only printable ASCII (0x20..0x7e). A BLE
// local name is attacker-controlled advertisement data and may hold invalid
// UTF-8; passed through verbatim it would serialize to a JSON string the Python
// validator rejects. Same discipline as the banner_grab runner.
static void sanitizeAscii(const char* in, size_t inLen, char* out, size_t outSize) {
    size_t j = 0;
    for (size_t i = 0; i < inLen && j + 1 < outSize; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 0x20 && c <= 0x7e) out[j++] = (char)c;
    }
    out[j] = '\0';
}

bool runBleScan(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                void* sink, uint32_t* resultsOut,
                char* errCode, size_t errCodeSize,
                char* errMsg, size_t errMsgSize) {
    JsonDocument args;
    if (deserializeJson(args, ctx.args) != DeserializationError::Ok) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "args are not valid JSON");
        return false;
    }
    int durationS = args["duration_s"] | 5;
    if (durationS < 1 || durationS > 30) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "duration_s must be 1..30");
        return false;
    }
    bool active = args["active"] | true;

    if (cancelled(sink)) { *resultsOut = 0; return true; }

#if NMS_HAS_BLE
    // A BLE scan coexists with the WiFi association (shared radio, controller
    // arbitration) — no survey-style disconnect is needed. init() brings up the
    // BT controller; deinit(true) hands its RAM back when we're done.
    BLEDevice::init("nms-probe");
    BLEScan* scan = BLEDevice::getScan();
    if (scan == nullptr) {
        BLEDevice::deinit(true);
        snprintf(errCode, errCodeSize, "unsupported");
        snprintf(errMsg, errMsgSize, "BLE scanner unavailable");
        return false;
    }
    scan->setActiveScan(active);  // active = send scan requests for richer names
    scan->setInterval(100);
    scan->setWindow(99);

    // start() blocks for the whole window; on arduino-esp32 2.0.x it returns
    // BLEScanResults by value (3.x returns a pointer — a bring-up-time detail
    // flagged here since this can't be compiled without the board).
    BLEScanResults results = scan->start(durationS, /*is_continue=*/false);
    int count = results.getCount();

    static char out[900];
    char namebuf[40];
    uint32_t emitted = 0;
    int i = 0;
    while (i < count) {
        JsonDocument doc;
        JsonArray arr = doc["devices"].to<JsonArray>();
        for (; i < count; ++i) {
            BLEAdvertisedDevice d = results.getDevice(i);
            JsonObject o = arr.add<JsonObject>();

            // MAC — copy into a fresh buffer so ArduinoJson duplicates it rather
            // than aliasing the std::string that dies at loop end.
            std::string addr = d.getAddress().toString();
            char macbuf[18];
            strncpy(macbuf, addr.c_str(), sizeof(macbuf) - 1);
            macbuf[sizeof(macbuf) - 1] = '\0';
            o["mac"] = macbuf;

            std::string nm = d.getName();
            sanitizeAscii(nm.c_str(), nm.size(), namebuf, sizeof(namebuf));
            if (namebuf[0] != '\0') o["name"] = namebuf;
            else o["name"] = (const char*)nullptr;

            o["rssi"] = d.getRSSI();

            // Connectable if the advertisement PDU is ADV_IND (0) or
            // ADV_DIRECT_IND (1); scannable/non-connectable otherwise.
            esp_ble_evt_type_t at = d.getAdvType();
            o["connectable"] = (at == ESP_BLE_EVT_CONN_ADV || at == ESP_BLE_EVT_CONN_DIR_ADV);

            if (d.haveManufacturerData()) {
                std::string md = d.getManufacturerData();
                if (md.size() >= 2) {
                    // Company identifier: first two octets, little-endian (BT spec).
                    uint16_t cid = (uint16_t)(((uint8_t)md[1] << 8) | (uint8_t)md[0]);
                    char hex[5];
                    snprintf(hex, sizeof(hex), "%04X", cid);
                    o["manufacturer"] = hex;
                }
            }

            ++emitted;
            if (measureJson(doc) > 760) { ++i; break; }
        }
        size_t len = serializeJson(doc, out, sizeof(out));
        if (len > 0) emit(sink, out);
    }

    // A scan that saw nothing still reports one empty chunk so the job is honest.
    if (count == 0) emit(sink, "{\"devices\":[]}");

    scan->clearResults();
    BLEDevice::deinit(/*release_memory=*/true);

    *resultsOut = emitted;
    return true;
#else
    (void)emit; (void)sink; (void)active;
    *resultsOut = 0;
    snprintf(errCode, errCodeSize, "unsupported");
    snprintf(errMsg, errMsgSize, "ble_scan requires the ESP32 BLE radio (device build)");
    return false;
#endif
}
