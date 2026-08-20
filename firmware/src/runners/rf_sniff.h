// rf_sniff recon runner (result key "carriers") — RF-Sentinel sub-GHz sensor.
//
// Sweeps a sub-GHz band with a CC1101 transceiver on its own SPI bus, tuning to
// each step, dwelling, and reading the received-signal-strength register to
// report the carriers present. Purely passive: the CC1101 is held in RX, it
// transmits nothing — the sub-GHz counterpart to wifi_ids' passive listening.
//
// The CC1101 is a SEPARATE radio from the ESP32's 2.4 GHz WiFi/BLE core, wired
// to VSPI. It does not touch the WiFi association, so — unlike wifi_survey /
// wifi_ids, which must leave the AP — MQTT stays connected for the whole sweep.
// rf_sniff therefore runs the normal worker lifecycle (managesLifecycle=false):
// the worker emits `accepted`/`done`; this runner only emits the chunks.
//
// Chunk shape: {"carriers":[{freq_mhz,rssi,bandwidth_khz,packets}]}, batched to
// stay under the payload cap. A sweep that found no carrier above the noise
// floor still emits one {"carriers":[]} chunk so the job reports honestly.
//
// Conforms to protocol/schemas/args/rf_sniff.schema.json (freq_min_mhz /
// freq_max_mhz in 300..928, duration_s 1..60, gain 0..64) and to the "carriers"
// key in protocol/schemas/result.schema.json. See rf_sniff.cpp for the GPIO
// pinout and ARCHITECTURE.md for the wiring table.
#pragma once

#include "../worker.h"

bool runRfSniff(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                void* sink, uint32_t* resultsOut,
                char* errCode, size_t errCodeSize,
                char* errMsg, size_t errMsgSize);
