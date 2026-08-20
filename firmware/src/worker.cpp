#include "worker.h"

#include <string.h>
#include <stdio.h>

#include <Arduino.h>  // millis()
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "mqtt_client.h"  // JobRequest, mqtt* job-lifecycle helpers
#include "envelope.h"     // PAYLOAD_MAX_BYTES

#include "runners/port_scan.h"
#include "runners/banner_grab.h"
#include "runners/dns.h"
#include "runners/discover.h"
#include "runners/trace.h"  // defines NMS_TRACE_AVAILABLE
#include "runners/wifi_survey.h"
#include "runners/ble_scan.h"
#include "runners/wifi_ids.h"
#include "runners/wifi_deauth.h"
#include "runners/monitor.h"

// --- runner registry ------------------------------------------------------
//
// One row per recon command this firmware actually implements. It is the single
// source of truth for both dispatch and the capability advertisement
// (mqtt_client calls workerLookupRunner), so a node can never announce a command
// it cannot run nor silently run one it never announced.
//
// The STATION-mode recon runners land here as implemented. `trace` is
// conditional on raw-ICMP support: when NMS_TRACE_AVAILABLE is 0 the
// row is compiled out, so the command is neither dispatched nor advertised and
// the server disables it for this node with no protocol change.
//
// `managesLifecycle` marks a runner that drives its own `accepted`/status and
// MQTT choreography — `wifi_survey`, `wifi_ids`, and `wifi_deauth` which publish
// `accepted` and status itself and disconnect/reconnect MQTT around the radio
// window. The worker skips its own `accepted` for these but still emits
// the final `done` (which drains once MQTT is back).
//
// `ble_scan` is a normal-lifecycle runner: BLE coexists with the WiFi
// association, so MQTT stays up and the worker emits `accepted`/`done` as usual.
struct RunnerRow {
    const char* cmd;
    RunnerFn fn;
    bool managesLifecycle;
};

static const RunnerRow REGISTRY[] = {
    { "port_scan", runPortScan, false },
    { "banner_grab", runBannerGrab, false },
    { "dns", runDns, false },
    { "discover", runDiscover, false },
#if NMS_TRACE_AVAILABLE
    { "trace", runTrace, false },
#endif
    { "wifi_survey", runWifiSurvey, true },
    { "ble_scan", runBleScan, false },
    { "wifi_ids", runWifiIds, true },
    { "wifi_deauth", runWifiDeauth, true },
    { nullptr, nullptr, false },  // sentinel: keeps the array non-empty
};
static const size_t REGISTRY_COUNT = sizeof(REGISTRY) / sizeof(REGISTRY[0]);

RunnerFn workerLookupRunner(const char* cmd) {
    for (size_t i = 0; i < REGISTRY_COUNT; ++i) {
        if (REGISTRY[i].cmd != nullptr && strcmp(cmd, REGISTRY[i].cmd) == 0) {
            return REGISTRY[i].fn;
        }
    }
    return nullptr;
}

static bool runnerManagesLifecycle(const char* cmd) {
    for (size_t i = 0; i < REGISTRY_COUNT; ++i) {
        if (REGISTRY[i].cmd != nullptr && strcmp(cmd, REGISTRY[i].cmd) == 0) {
            return REGISTRY[i].managesLifecycle;
        }
    }
    return false;
}

size_t workerCapabilities(const char** out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < REGISTRY_COUNT && n < max; ++i) {
        if (REGISTRY[i].cmd != nullptr) out[n++] = REGISTRY[i].cmd;
    }
    return n;
}

// --- per-job sink ---------------------------------------------------------
//
// Opaque handle the worker hands to a runner. It carries everything the runner's
// emit/cancel callbacks need without exposing MQTT: the job id (for envelope
// stamping), the running seq/chunk counters, and the cancel latch.
struct JobSink {
    const char* job_id;
    uint32_t seq;
    uint32_t chunks;
    bool cancelled;
};

// Composes {"event":"chunk","job_id":..,"seq":N, <runner fields>} and enqueues
// it on the outbox. The runner's payload is a serialized JSON object; its fields
// are merged by dropping the payload's leading '{'. An empty object contributes
// no fields.
static void emitChunk(void* sinkV, const char* chunkObjJson) {
    JobSink* sink = (JobSink*)sinkV;
    static char data[PAYLOAD_MAX_BYTES];

    int head = snprintf(data, sizeof(data),
                        "{\"event\":\"chunk\",\"job_id\":\"%s\",\"seq\":%u",
                        sink->job_id, (unsigned)sink->seq);
    if (head < 0 || (size_t)head >= sizeof(data)) {
        return;
    }

    bool hasFields = chunkObjJson != nullptr && chunkObjJson[0] == '{' &&
                     chunkObjJson[1] != '}' && chunkObjJson[1] != '\0';
    if (hasFields) {
        // chunkObjJson+1 still carries the payload's closing '}', so this both
        // merges the fields and closes the outer object.
        int rest = snprintf(data + head, sizeof(data) - (size_t)head, ",%s",
                            chunkObjJson + 1);
        if (rest < 0 || (size_t)head + (size_t)rest >= sizeof(data)) {
            return;  // over budget — drop rather than emit a truncated frame
        }
    } else {
        if ((size_t)head + 2 > sizeof(data)) {
            return;
        }
        data[head] = '}';
        data[head + 1] = '\0';
    }

    mqttEnqueueResult(data);
    sink->seq++;
    sink->chunks++;
}

// Latches cancellation for the running job. mqttTakeCancel hands over a pending
// cancel's target id; a cancel for a different (stale) job is consumed and
// dropped, since only the running job can be its target on a depth-1 queue.
static bool isCancelled(void* sinkV) {
    JobSink* sink = (JobSink*)sinkV;
    if (sink->cancelled) {
        return true;
    }
    char cid[36];
    if (mqttTakeCancel(cid, sizeof(cid)) && strcmp(cid, sink->job_id) == 0) {
        sink->cancelled = true;
    }
    return sink->cancelled;
}

// --- lifecycle events (worker side, via the outbox) -----------------------

static void emitAccepted(const char* jobId) {
    char data[96];
    snprintf(data, sizeof(data), "{\"event\":\"accepted\",\"job_id\":\"%s\"}", jobId);
    mqttEnqueueResult(data);
}

static void emitDone(const char* jobId, uint32_t chunks, uint32_t results,
                     uint32_t durationMs) {
    char data[160];
    snprintf(data, sizeof(data),
             "{\"event\":\"done\",\"job_id\":\"%s\",\"chunks\":%u,\"results\":%u,"
             "\"duration_ms\":%u}",
             jobId, (unsigned)chunks, (unsigned)results, (unsigned)durationMs);
    mqttEnqueueResult(data);
}

static void emitError(const char* jobId, const char* code, const char* message) {
    char data[192];
    snprintf(data, sizeof(data),
             "{\"event\":\"error\",\"job_id\":\"%s\",\"code\":\"%s\",\"message\":\"%s\"}",
             jobId, code, message);
    mqttEnqueueResult(data);
}

// --- job execution --------------------------------------------------------

static void runJob(const JobRequest& req) {
    mqttSetState("busy");
    emitAccepted(req.job_id);

    RunnerFn fn = workerLookupRunner(req.cmd);
    if (fn == nullptr) {
        // Reached only if the mqtt_client capability gate and this registry ever
        // drift; the gate rejects unsupported recon first. Answer honestly.
        emitError(req.job_id, "unsupported", "command not supported by this node");
    } else {
        JobSink sink = {req.job_id, /*seq=*/0, /*chunks=*/0, /*cancelled=*/false};
        RunnerCtx ctx = {req.job_id, req.args};

        uint32_t results = 0;
        char errCode[24] = {0};
        char errMsg[96] = {0};
        uint32_t start = millis();
        bool ok = fn(ctx, emitChunk, isCancelled, &sink, &results,
                     errCode, sizeof(errCode), errMsg, sizeof(errMsg));
        uint32_t durationMs = millis() - start;

        if (sink.cancelled) {
            // A cancelled job is not a success: the node reports the cancellation
            // as an error event with code "cancelled". ingest
            // records that; cancel is not marked terminal server-side.
            emitError(req.job_id, "cancelled", "job cancelled by operator");
        } else if (ok) {
            emitDone(req.job_id, sink.chunks, results, durationMs);
        } else {
            emitError(req.job_id, errCode[0] ? errCode : "error",
                      errMsg[0] ? errMsg : "runner failed");
        }
    }

    mqttIncJobsDone();
    mqttSetState("online");
}

// --- task -----------------------------------------------------------------

// Idle hook: when no job is running, a persisted monitor schedule may be due
// ("monitor cycles when idle"). monitorTick() is cheap when no
// schedule is set or none is due; when a cycle is due it runs the ping/port
// probes and enqueues one `monitor` frame through the outbox. It self-gates on
// radioInStation() so cycles are skipped during a survey window.
static void checkMonitorDue() {
    monitorTick();
}

static void workerTask(void* /*arg*/) {
    // Load the persisted monitor schedule on this core before the first idle
    // tick; set_monitor (core 0) and monitorTick (this core) coordinate through
    // the monitor module's own mutex thereafter.
    monitorInit();

    JobRequest req;
    for (;;) {
        if (xQueueReceive(mqttJobQueue(), &req, pdMS_TO_TICKS(200)) == pdTRUE) {
            runJob(req);
        } else {
            checkMonitorDue();  // idle tick
        }
        // Drop any cancel with no running job to target so it does not wedge the
        // latch for the next job — a cancel is only meaningful against the job
        // that was running when it arrived.
        char cancelId[36];
        mqttTakeCancel(cancelId, sizeof(cancelId));
    }
}

void workerStart() {
    // Core 1, 8KB stack: the core split keeps a long scan off
    // core 0 so it cannot starve MQTT keepalive on the MQTT task.
    xTaskCreatePinnedToCore(workerTask, "worker", 8192, nullptr, /*priority=*/1,
                            nullptr, /*core=*/1);
}
