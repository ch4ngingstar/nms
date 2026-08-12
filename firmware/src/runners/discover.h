// discover recon runner (result key "hosts"). ICMP echo sweep across the
// requested targets (single IPs or a CIDR
// like "192.168.1.0/24"), with a TCP-liveness fallback when raw ICMP is
// unavailable and best-effort ARP-table MAC enrichment.
//
// Chunk shape: {"hosts":[{"ip","mac","rtt_ms","method"}]}, only live hosts.
// NOTE this intentionally diverges from probe/jobs.py:discover, which yields the
// schema-invalid {"up":[...]}; the result schema's key is "hosts".
#pragma once

#include <stddef.h>

#include "../worker.h"

bool runDiscover(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                 void* sink, uint32_t* resultsOut,
                 char* errCode, size_t errCodeSize,
                 char* errMsg, size_t errMsgSize);
