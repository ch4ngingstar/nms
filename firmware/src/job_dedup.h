// Duplicate job_id suppression ring (protocol §7.1, §5) — pure and host-testable,
// split out of mqtt_client so the native Unity suite can exercise it without the
// PubSubClient / FreeRTOS translation unit.
//
// QoS 1 redelivers, so the same `cmd` can arrive twice; the MQTT task remembers
// the last few job_ids it accepted and ignores a repeat. Only the branchless ring
// logic lives here — the ring's *owner* stays the MQTT task in mqtt_client (spec
// §5 single-writer), which holds one instance and never shares it across tasks.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Depth of the ring: the last 8 accepted job_ids are remembered (protocol §7.1).
static const size_t JOB_DEDUP_CAPACITY = 8;

// A job_id copy slot. 36 bytes fits a UUID-style id plus NUL; longer ids are
// truncated (they still compare consistently against their own truncation).
static const size_t JOB_DEDUP_ID_MAX = 36;

// Drop-oldest ring of recently accepted job_ids. Zero-initialize (`JobDedup{}` or
// static storage): empty slots hold "" and never match a real job_id.
struct JobDedup {
    char ids[JOB_DEDUP_CAPACITY][JOB_DEDUP_ID_MAX];
    uint8_t idx;  // next slot to overwrite
};

// Returns true if `jobId` is already in the ring (a QoS-1 duplicate to ignore).
// Otherwise records it (overwriting the oldest slot) and returns false.
bool jobDedupSeen(JobDedup& ring, const char* jobId);
