// Worker task and recon job lifecycle.
//
// The worker owns the *execution* side of a job: it blocks on the MQTT task's
// job queue, emits the `accepted` -> `chunk`* -> `done`|`error` sequence, owns
// the `seq` counter, and polls the cross-task cancel flag between chunks. A
// recon runner supplies only the I/O and the chunk payloads — it never touches
// MQTT, the envelope, or seq numbering (that is what keeps runners host-testable
// and the wire format in one place).
//
// `probe/virtual_probe.py` is the reference for this lifecycle; where the two
// disagree, the golden corpus decides (protocol invariants).
#pragma once

#include <stddef.h>
#include <stdint.h>

// --- runner seam ----------------------------------------------------------
//
// A runner reads its already-validated args, performs its work, and hands each
// bounded result batch to `emit` as a serialized JSON *object* (e.g.
// {"open":[{...}]}). The worker wraps that object in a
// {"event":"chunk","job_id":..,"seq":N, ...} result envelope, so the runner
// must not add event/job_id/seq itself. Runners MUST call `cancelled` between
// units of work and return promptly once it is true.

// Emits one chunk payload object. `sink` is an opaque worker-owned handle passed
// back verbatim; runners never interpret it.
typedef void (*EmitChunkFn)(void* sink, const char* chunkObjJson);

// True once a `cancel` for the running job has been observed. Poll it between
// chunks; a runner that ignores it merely delays termination, it does not break
// correctness.
typedef bool (*IsCancelledFn)(void* sink);

// Context handed to a runner for one job.
struct RunnerCtx {
    const char* job_id;  // diagnostics only; the worker stamps every envelope
    const char* args;    // serialized args object, "{}" when the command sent none
};

// A recon runner. Returns true when the job completed (worker emits `done`),
// false on failure (worker emits `error`). On failure the runner writes a
// protocol error code (e.g. "timeout", "bad_args", "radio_conflict") into
// `errCode` and a short human message into `errMsg`. `*resultsOut` accumulates
// the number of result rows produced, for the `done` summary.
typedef bool (*RunnerFn)(const RunnerCtx& ctx, EmitChunkFn emit,
                         IsCancelledFn cancelled, void* sink,
                         uint32_t* resultsOut,
                         char* errCode, size_t errCodeSize,
                         char* errMsg, size_t errMsgSize);

// Looks up the runner registered for `cmd`, or nullptr if this firmware does not
// implement it. Exposed for the capability advertisement in mqtt_client so the
// `announce` list and the dispatch table cannot drift apart. See worker.cpp for
// the registry itself, where every implemented command — including
// `wifi_deauth` — is listed.
RunnerFn workerLookupRunner(const char* cmd);

// Writes the names of the registered recon commands into `out` (up to `max`),
// returning the count. This is what `announce.capabilities` is built from, so a
// node advertises exactly what it can dispatch — no more, no less.
size_t workerCapabilities(const char** out, size_t max);

// Spawns the worker FreeRTOS task on core 1 (8KB stack). Call
// once after mqttInit(); the task lives for the process lifetime.
void workerStart();
