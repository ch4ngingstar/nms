// ble_scan recon runner (result key "devices").
//
// Enumerates BLE devices in range using the Arduino-ESP32 BLE library
// (BLEDevice::getScan), following ESP32-Bit-Pirate's BluetoothService::scanDevices()
// pattern (MIT, see firmware/NOTICE). Unlike wifi_survey/wifi_ids this is a
// STATION-mode job: the ESP32 shares its 2.4 GHz radio between WiFi and BLE via
// the controller's built-in coexistence, so MQTT stays connected (throughput just
// drops for the scan window) and NO survey-style disconnect sequence is used. It
// therefore runs through the normal worker lifecycle (managesLifecycle=false): the
// worker emits `accepted`/`done`; this runner only emits the chunks.
//
// Chunk shape: {"devices":[{mac,name,rssi,connectable,manufacturer}]}, with
// "dropped" folded in on overflow. A scan that saw nothing still emits one
// {"devices":[]} chunk so the job reports honestly.
//
// BLE-IDS: a second, duplicate-including callback counts every raw Apple
// Continuity (company id 0x004C) / Google Fast Pair (0x00E0) advertisement
// seen during the window — BLEScan's deduped device list alone can't see a
// flood or a Flipper-style rotating-MAC/identical-payload spam pattern. If the
// packet rate for either company exceeds BLE_SPAM_RATE_THRESHOLD_PPS, or the
// same payload repeats from BLE_SPAM_MAC_ROTATE_THRESHOLD or more distinct
// MACs, one extra {"alerts":[{"type":"ble_spam_flood","rate":N,
// "company_id":"0x004c"}]} chunk is emitted after the devices chunk(s).
#pragma once

#include "../worker.h"

bool runBleScan(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                void* sink, uint32_t* resultsOut,
                char* errCode, size_t errCodeSize,
                char* errMsg, size_t errMsgSize);
