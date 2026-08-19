// ble_scan recon runner (spec §6.4; protocol result key "devices").
//
// Enumerates BLE devices in range using the Arduino-ESP32 BLE library
// (BLEDevice::getScan), following ESP32-Bit-Pirate's BluetoothService::scanDevices()
// pattern (MIT, see firmware/NOTICE). Unlike wifi_survey/wifi_ids this is a
// STATION-mode job: the ESP32 shares its 2.4 GHz radio between WiFi and BLE via
// the controller's built-in coexistence, so MQTT stays connected (throughput just
// drops for the scan window) and NO §6.1 disconnect sequence is used. It therefore
// runs through the normal worker lifecycle (managesLifecycle=false): the worker
// emits `accepted`/`done`; this runner only emits the chunks.
//
// Chunk shape: {"devices":[{mac,name,rssi,connectable,manufacturer}]}, with
// "dropped" folded in on overflow (§8.5). A scan that saw nothing still emits one
// {"devices":[]} chunk so the job reports honestly.
#pragma once

#include "../worker.h"

bool runBleScan(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                void* sink, uint32_t* resultsOut,
                char* errCode, size_t errCodeSize,
                char* errMsg, size_t errMsgSize);
