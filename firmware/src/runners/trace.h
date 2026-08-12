// trace recon runner (result key "hops"). ICMP
// traceroute with incrementing IP TTL, reading time-exceeded from intermediate
// hops. Requires a raw ICMP socket (CONFIG_LWIP_RAW); if raw
// sockets are unavailable, trace is simply NOT advertised — the server disables
// it for this node with no protocol change. NMS_TRACE_AVAILABLE gates both the
// worker registration and the announce list.
//
// Chunk shape: {"hops":[{"ttl","addr","rtt_ms","timeout"}]}, one chunk per target.
#pragma once

#include <stddef.h>

#include "../worker.h"

// Default OFF until raw-socket support is confirmed on the board (a
// throwaway spike). Flip with -DNMS_ENABLE_TRACE=1 once verified, or rely on
// CONFIG_LWIP_RAW when the toolchain defines it truthy.
#if defined(NMS_ENABLE_TRACE)
#define NMS_TRACE_AVAILABLE NMS_ENABLE_TRACE
#elif defined(CONFIG_LWIP_RAW) && CONFIG_LWIP_RAW
#define NMS_TRACE_AVAILABLE 1
#else
#define NMS_TRACE_AVAILABLE 0
#endif

bool runTrace(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
              void* sink, uint32_t* resultsOut,
              char* errCode, size_t errCodeSize,
              char* errMsg, size_t errMsgSize);
