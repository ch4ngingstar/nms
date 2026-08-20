// RF-Sentinel: passive sub-GHz carrier sweep on a CC1101 (result key "carriers").
//
// ── CC1101 wiring — classic ESP32 WROOM-32, VSPI ──────────────────────────────
//
//   CC1101 pin   ESP32 GPIO   Notes
//   ----------   ----------   ---------------------------------------------------
//   SCK          GPIO18       VSPI CLK
//   MISO / SO    GPIO19       VSPI MISO
//   MOSI / SI    GPIO23       VSPI MOSI
//   CSN / CS     GPIO5        VSPI SS (strapping pin, idles high — fine as CS)
//   GDO0         GPIO4        RX carrier-sense / async data out (interrupt-capable)
//   GDO2         GPIO25       secondary status (reserved; wired for completeness)
//   VCC          3V3          CC1101 is 3.3 V ONLY — 5 V destroys it
//   GND          GND
//
// GPIO2 is deliberately NOT used: it drives the onboard LED that the `identify`
// command blinks (see main.cpp). The VSPI default bus (18/19/23/5) is chosen so
// the standard ELECHOUSE/SmartRC default pin map applies with only GDO pins
// overridden. This table is mirrored in ARCHITECTURE.md.
//
// ── Library ───────────────────────────────────────────────────────────────────
//
// SmartRC-CC1101-Driver-Lib (LSatan, MIT) — the most widely-used, lightweight
// CC1101 driver on the PlatformIO registry: a thin SPI register wrapper with no
// RTOS or heap footprint of its own. Pinned in firmware/platformio.ini under the
// esp32 env only; the native host build has no CC1101 and returns `unsupported`,
// exactly like ble_scan's radio guard.
//
// ── Behaviour ─────────────────────────────────────────────────────────────────
//
// Purely passive: the CC1101 is held in RX and transmits nothing. The sweep steps
// across [freq_min_mhz, freq_max_mhz] at STEP_MHZ, tuning the synthesizer to each
// step, dwelling, and sampling the RSSI register. A step whose peak RSSI clears
// the detection threshold is reported as a carrier; steps at the noise floor are
// not, so the "carriers" array holds detections, not every tuning position.
//
// Conforms to protocol/schemas/args/rf_sniff.schema.json and the "carriers" key
// in protocol/schemas/result.schema.json.

#include "rf_sniff.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <ArduinoJson.h>

// CC1101 register access is device-only. The native host build compiles the arg
// parsing and returns `unsupported`, mirroring ble_scan / wifi_survey.
#if defined(ESP_PLATFORM) || defined(ARDUINO)
#define NMS_HAS_CC1101 1
#include <Arduino.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#else
#define NMS_HAS_CC1101 0
#endif

// VSPI + GDO pin map (see the wiring table above).
static const int CC1101_SCK  = 18;
static const int CC1101_MISO = 19;
static const int CC1101_MOSI = 23;
static const int CC1101_CS   = 5;
static const int CC1101_GDO0 = 4;
static const int CC1101_GDO2 = 25;

// Sweep constants.
static const float STEP_MHZ = 0.25f;       // matches the virtual probe's grid
static const float RX_BW_KHZ = 58.0f;      // CC1101 RX filter bandwidth reported
static const int   RSSI_SAMPLES_PER_STEP = 4;
static const int   OUT_BUDGET = 760;       // batch flush threshold (payload cap)
static const size_t MAX_STEPS = 4096;      // guards a pathological wide+fine sweep

// Detection threshold in dBm as a function of the requested gain (0..64): higher
// gain lowers the bar, so more of the noise floor counts as a carrier. On real
// silicon `gain` also biases the CC1101 AGC/LNA registers; here it sets the
// software carrier-sense threshold, which is what determines what gets reported.
static int detectionThresholdDbm(int gain) {
    // gain 64 -> -95 dBm (most sensitive); gain 0 -> ~-76 dBm (least sensitive).
    return -95 + (int)lroundf((64 - gain) * 0.30f);
}

bool runRfSniff(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                void* sink, uint32_t* resultsOut,
                char* errCode, size_t errCodeSize,
                char* errMsg, size_t errMsgSize) {
    JsonDocument args;
    if (deserializeJson(args, ctx.args) != DeserializationError::Ok) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "args are not valid JSON");
        return false;
    }

    // Validate at the boundary, matching the schema and protocol/validate.py:
    // both endpoints in-band, and the range not inverted.
    if (!args["freq_min_mhz"].is<float>() || !args["freq_max_mhz"].is<float>()) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "freq_min_mhz and freq_max_mhz are required");
        return false;
    }
    float freqMin = args["freq_min_mhz"].as<float>();
    float freqMax = args["freq_max_mhz"].as<float>();
    int durationS = args["duration_s"] | 10;
    int gain = args["gain"] | 32;

    if (freqMin < 300.0f || freqMin > 928.0f || freqMax < 300.0f || freqMax > 928.0f) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "frequencies must be within 300..928 MHz");
        return false;
    }
    if (freqMin > freqMax) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "freq range inverted: min > max");
        return false;
    }
    if (durationS < 1 || durationS > 60) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "duration_s must be 1..60");
        return false;
    }
    if (gain < 0 || gain > 64) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "gain must be 0..64");
        return false;
    }

    if (cancelled(sink)) { *resultsOut = 0; return true; }

#if NMS_HAS_CC1101
    ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);
    if (!ELECHOUSE_cc1101.getCC1101()) {
        snprintf(errCode, errCodeSize, "radio_conflict");
        snprintf(errMsg, errMsgSize, "CC1101 not detected on VSPI");
        return false;
    }
    ELECHOUSE_cc1101.Init();
    ELECHOUSE_cc1101.setRxBW(RX_BW_KHZ);

    const int threshold = detectionThresholdDbm(gain);
    size_t steps = (size_t)((freqMax - freqMin) / STEP_MHZ) + 1;
    if (steps > MAX_STEPS) steps = MAX_STEPS;
    // Split the duration budget evenly across steps, with a settle floor so the
    // synthesizer has time to lock before RSSI is read.
    uint32_t dwellMs = (uint32_t)((durationS * 1000UL) / steps);
    if (dwellMs < 2) dwellMs = 2;

    static char out[900];
    JsonDocument doc;
    JsonArray arr = doc["carriers"].to<JsonArray>();
    uint32_t detected = 0;

    for (size_t i = 0; i < steps; ++i) {
        if (cancelled(sink)) break;

        float freq = freqMin + STEP_MHZ * (float)i;
        ELECHOUSE_cc1101.setMHZ(freq);
        ELECHOUSE_cc1101.SetRx();
        delay(1);  // synthesizer settle

        int peak = -128;
        uint32_t hits = 0;
        uint32_t start = millis();
        int taken = 0;
        while (taken < RSSI_SAMPLES_PER_STEP || (millis() - start) < dwellMs) {
            int r = ELECHOUSE_cc1101.getRssi();
            if (r > peak) peak = r;
            if (r >= threshold) ++hits;
            ++taken;
            if ((millis() - start) >= dwellMs && taken >= RSSI_SAMPLES_PER_STEP) break;
            delay(1);
        }

        if (peak >= threshold) {
            JsonObject o = arr.add<JsonObject>();
            o["freq_mhz"] = roundf(freq * 100.0f) / 100.0f;  // two decimals
            o["rssi"] = peak;
            o["bandwidth_khz"] = (int)RX_BW_KHZ;
            o["packets"] = hits;  // RSSI samples above threshold this dwell
            ++detected;

            if (measureJson(doc) > OUT_BUDGET) {
                size_t len = serializeJson(doc, out, sizeof(out));
                if (len > 0) emit(sink, out);
                doc.clear();
                arr = doc["carriers"].to<JsonArray>();
            }
        }
    }

    ELECHOUSE_cc1101.setSidle();  // leave the radio idle, not stuck in RX

    // Flush the trailing batch. A sweep that detected nothing still emits one
    // {"carriers":[]} chunk so the job reports honestly.
    if (arr.size() > 0 || detected == 0) {
        size_t len = serializeJson(doc, out, sizeof(out));
        if (len > 0) emit(sink, out);
    }

    *resultsOut = detected;
    return true;
#else
    (void)emit; (void)sink;
    *resultsOut = 0;
    snprintf(errCode, errCodeSize, "unsupported");
    snprintf(errMsg, errMsgSize, "rf_sniff requires a CC1101 on VSPI (device build)");
    return false;
#endif
}
