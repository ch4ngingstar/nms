// Protocol envelope construction and parsing.
//
// Deliberately free of Arduino and network dependencies so it compiles in the
// PlatformIO `native` env and is unit-tested on the host against the golden
// corpus. Everything operates on caller-owned char[]
// buffers: no Arduino String, no returned heap.
#pragma once

#include <stddef.h>
#include <stdint.h>

// msg_id is 16 lowercase hex chars (8 random bytes). The envelope schema allows
// 8..26; 16 sits comfortably inside and matches the virtual probe's
// secrets.token_hex(8) reference (probe/virtual_probe.py).
static const size_t MSG_ID_HEX_LEN = 16;

// A single published payload must not exceed 1024 bytes.
// buildEnvelope refuses to produce anything larger, which forces producers to
// chunk rather than silently emit an over-cap frame.
static const size_t PAYLOAD_MAX_BYTES = 1024;

// Writes MSG_ID_HEX_LEN hex chars + NUL into buf (needs >= 17 bytes). Returns
// the number of hex chars written (MSG_ID_HEX_LEN), or 0 if buf is too small.
size_t generateMsgId(char* buf, size_t bufSize);

// Builds {"v":1,"type":<type>,"node":<node>,"msg_id":<generated>,"ts":<ts>,
// "data":<dataJson>} into `out`. `dataJson` is an already-serialized JSON object
// (e.g. "{\"event\":\"accepted\",\"job_id\":\"job-1\"}") embedded verbatim.
// Returns the serialized length (excluding NUL), or 0 if the result would
// exceed `outSize` or PAYLOAD_MAX_BYTES.
size_t buildEnvelope(char* out, size_t outSize, const char* type,
                     const char* node, uint32_t ts, const char* dataJson);

// Parses an inbound `cmd` envelope. Validates v == 1, type ==
// "cmd", and the presence of data.cmd and data.job_id. On success, copies
// data.cmd and data.job_id (truncated to fit their buffers) and serializes
// data.args into argsOut ("{}" when args is absent). Returns false — rejecting
// the message — on any parse or validation failure.
bool parseCmd(const char* json, size_t len,
              char* cmdOut, size_t cmdSize,
              char* jobIdOut, size_t jobIdSize,
              char* argsOut, size_t argsSize);
