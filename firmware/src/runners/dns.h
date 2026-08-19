// dns recon runner (spec §6.4; protocol result key "answers"). Unlike the
// virtual probe (probe/jobs.py:dns), which delegates to getaddrinfo and can only
// return addresses, the firmware speaks DNS on the wire so it can return
// A/AAAA/CNAME/MX/TXT/NS/PTR records. The builder and response parser are pure
// and host-tested against name-compression cases (spec §9.1).
//
// Chunk shape: {"answers":[{"name","type","ttl","value"}]}.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../worker.h"

// DNS TYPE codes we build queries for / decode in responses.
enum DnsType {
    DNS_A = 1,
    DNS_NS = 2,
    DNS_CNAME = 5,
    DNS_PTR = 12,
    DNS_MX = 15,
    DNS_TXT = 16,
    DNS_AAAA = 28,
};

// Maps "A"/"AAAA"/"CNAME"/"MX"/"TXT"/"NS"/"PTR" (case-insensitive) to a TYPE
// code, or 0 if unrecognized. Its inverse names a decoded record's type.
uint16_t dnsTypeFromName(const char* name);
const char* dnsTypeName(uint16_t type);

// Builds a standard recursive query (one question, RD=1) for `name`/`qtype` into
// `out`. Returns the message length, or 0 if it would not fit. Pure, host-testable.
size_t dnsBuildQuery(const char* name, uint16_t qtype, uint16_t id,
                     uint8_t* out, size_t outSize);

// One decoded answer, rendered to text (`value`) by record type.
struct DnsAnswer {
    char name[128];
    uint16_t type;
    uint32_t ttl;
    char value[192];
};

// Parses the answer section of a response message, decoding names through
// compression pointers. Returns the count written to `out` (<= maxAnswers), or
// -1 on a malformed message. Pure, host-testable — no sockets.
int dnsParseResponse(const uint8_t* msg, size_t len, DnsAnswer* out, size_t maxAnswers);

// RunnerFn: query each target name and stream {"answers":[...]} chunks.
bool runDns(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
            void* sink, uint32_t* resultsOut,
            char* errCode, size_t errCodeSize,
            char* errMsg, size_t errMsgSize);
