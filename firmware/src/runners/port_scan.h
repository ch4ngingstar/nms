// port_scan recon runner (spec §6.4 nmap mapping, §7.5; protocol result key
// "open"). TCP connect scan reporting the open/closed/filtered trichotomy, with
// only the open ports surfaced per target — matching probe/jobs.py:port_scan
// ("the ports found open on it") and the golden result_chunk_port_scan fixture.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../worker.h"  // RunnerCtx, EmitChunkFn, IsCancelledFn

// Pure, host-testable port-spec parser (spec §7.2, §9.1) — the C++ side of the
// contract in protocol/ports.py. Parses "22,80,443,8000-8100" into ascending,
// de-duplicated ports written to `out` (up to `maxPorts`). Returns the count on
// success, or -1 on malformed input (empty element, non-decimal, out of 1..65535,
// or low>high in a range) so the runner can answer `bad_args`. If the expansion
// would exceed `maxPorts` it fills what fits and returns that count.
int parsePortSpec(const char* spec, uint16_t* out, size_t maxPorts);

// RunnerFn: connect-scan every target's ports, emitting one {"open":[...]} chunk
// per target (further split if a target has enough open ports to approach the
// 1024-byte cap). See worker.h for the RunnerFn contract.
bool runPortScan(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                 void* sink, uint32_t* resultsOut,
                 char* errCode, size_t errCodeSize,
                 char* errMsg, size_t errMsgSize);
