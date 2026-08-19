// wifi_survey recon runner (spec §6.1–§6.3; protocol result keys "aps"/"clients").
// A radio-owning runner: it does NOT stream through the normal worker lifecycle —
// radioRunSurvey publishes `accepted` and the retained `surveying` status itself
// and performs the clean MQTT disconnect/reconnect (§6.1). This runner allocates
// the bounded survey buffers (sized from free heap, §7), invokes the sequence,
// then serializes the collected APs and client stations into chunks.
//
// Chunk shape: {"aps":[{bssid,ssid,channel,rssi,auth,hidden}],
//               "clients":[{mac,bssid,rssi}]}, with "dropped" folded in on
// overflow (§8.5). Registered with managesLifecycle=true so the worker does not
// emit its own `accepted`.
#pragma once

#include "../worker.h"

bool runWifiSurvey(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                   void* sink, uint32_t* resultsOut,
                   char* errCode, size_t errCodeSize,
                   char* errMsg, size_t errMsgSize);
