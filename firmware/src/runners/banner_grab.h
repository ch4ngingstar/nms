// banner_grab recon runner (spec §6.4 `nc` mapping; protocol result key
// "banners"). Raw TCP connect, read what a service emits on connect. Mirrors
// probe/jobs.py:banner_grab — one {"banners":[{host,port,banner}]} result, with
// banner null when nothing was read or the connect failed.
#pragma once

#include <stddef.h>

#include "../worker.h"

bool runBannerGrab(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                   void* sink, uint32_t* resultsOut,
                   char* errCode, size_t errCodeSize,
                   char* errMsg, size_t errMsgSize);
