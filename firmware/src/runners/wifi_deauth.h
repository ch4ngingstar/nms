#pragma once

#include "../worker.h"

bool runWifiDeauth(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                   void* sink, uint32_t* resultsOut,
                   char* errCode, size_t errCodeSize,
                   char* errMsg, size_t errMsgSize);
