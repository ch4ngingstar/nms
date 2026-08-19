// wifi_ids recon runner (spec §6.4; protocol result keys "alerts"/"frame_stats").
//
// A passive distributed wireless IDS. Like wifi_survey it is a radio-owning
// runner (managesLifecycle=true): radioRunIds performs the §6.1 disconnect
// sequence and the retained `surveying` status itself, then listens in
// promiscuous mode classifying frames — it transmits nothing. It is the
// detection counterpart to the deauth attack this firmware deliberately omits:
// it *detects* deauth floods rather than producing them, plus rogue-AP and
// evil-twin beacons against an operator-supplied allow-list.
//
// Chunk shapes: {"alerts":[{type,...}]} (one or more, "dropped" folded in on
// overflow) followed by a final {"frame_stats":{total,management,data,control,
// deauth,probe_request}}. Registered with managesLifecycle=true so the worker
// does not emit its own `accepted`.
#pragma once

#include "../worker.h"

bool runWifiIds(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                void* sink, uint32_t* resultsOut,
                char* errCode, size_t errCodeSize,
                char* errMsg, size_t errMsgSize);
