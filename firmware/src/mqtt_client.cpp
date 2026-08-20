#include "mqtt_client.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_random.h>

#include "identity.h"
#include "envelope.h"
#include "outbox.h"
#include "backoff.h"     // computeBackoffMs (extracted; host-testable)
#include "job_dedup.h"   // JobDedup / jobDedupSeen (extracted; host-testable)
#include "worker.h"  // workerLookupRunner / workerCapabilities — single source of truth
#include "runners/monitor.h"  // monitorSetFromArgs (set_monitor persistence)

// --- module state ---------------------------------------------------------

static WiFiClient s_net;
static PubSubClient s_mqtt(s_net);
static ProbeConfig s_cfg;

static QueueHandle_t s_jobQueue = nullptr;

static char s_topicCmd[48];
static char s_topicResult[48];
static char s_topicStatus[48];
static char s_topicTelemetry[48];
static char s_topicMonitor[48];

// Survey handoff: while a survey owns the radio+MQTT on the worker,
// the MQTT task (loop()) observes s_surveyPause, acknowledges, and steps aside.
static volatile bool s_surveyPause = false;
static volatile bool s_surveyPausedAck = false;

static char s_state[16] = "connecting";
static uint32_t s_jobsDone = 0;
static uint32_t s_bootTs = 0;         // Unix ts captured at first successful connect
static uint32_t s_lastTelemetryMs = 0;

// Cross-task cancel handoff (MQTT task sets, worker polls).
static volatile bool s_cancelPending = false;
static char s_cancelJobId[36];

// identify window end, in millis().
static uint32_t s_identifyUntilMs = 0;

// --- small helpers --------------------------------------------------------

static uint32_t nowTs() { return (uint32_t)time(nullptr); }

static void buildTopic(char* out, size_t size, const char* leaf) {
    snprintf(out, size, "nms/v1/node/%s/%s", getNodeId(), leaf);
}

// Publishes an envelope directly from the MQTT task. PubSubClient publishes at
// QoS 0 only (a library limitation, not a protocol choice); the retained flag
// is honored, which is what `status` actually depends on.
static bool publishDirect(const char* topic, const char* type,
                          const char* dataJson, bool retained) {
    static char buf[PAYLOAD_MAX_BYTES + 1];
    size_t n = buildEnvelope(buf, sizeof(buf), type, getNodeId(), nowTs(), dataJson);
    if (n == 0) {
        return false;
    }
    return s_mqtt.publish(topic, (const uint8_t*)buf, n, retained);
}

static void emitResultDirect(const char* dataJson) {
    publishDirect(s_topicResult, "result", dataJson, /*retained=*/false);
}

static bool isRecon(const char* cmd) {
    static const char* const RECON[] = {"port_scan", "banner_grab", "dns",
                                        "trace", "discover", "wifi_survey"};
    for (const char* r : RECON) {
        if (strcmp(cmd, r) == 0) {
            return true;
        }
    }
    return false;
}

static bool isCapable(const char* cmd) {
    // A command is capable iff the worker has a runner for it — this is the same
    // registry that builds `announce.capabilities`, so the gate and the
    // advertisement cannot disagree.
    return workerLookupRunner(cmd) != nullptr;
}

static bool seenBefore(const char* jobId) {
    // The ring's owner stays this (single-writer) MQTT task; only the pure logic
    // lives in job_dedup so the native suite can test it.
    static JobDedup s_dedup{};
    return jobDedupSeen(s_dedup, jobId);
}

// --- announce / status / telemetry payloads -------------------------------

static void emitAnnounce() {
    // Built with ArduinoJson because `label` is operator-supplied and must be
    // JSON-escaped; the fixed-shape payloads below use snprintf. Sized for a
    // 64-char label plus the full capability list with headroom under the cap.
    char data[384];
    JsonDocument d;
    d["label"] = s_cfg.label;
    d["fw"] = FW_VERSION;
    d["chip"] = "esp32";  // classic WROOM-32, not the S3
    d["mac"] = getMacString();
    d["free_heap"] = (uint32_t)ESP.getFreeHeap();
    JsonArray caps = d["capabilities"].to<JsonArray>();
    const char* capNames[8];
    size_t nc = workerCapabilities(capNames, 8);
    for (size_t i = 0; i < nc; ++i) {
        caps.add(capNames[i]);
    }
    serializeJson(d, data, sizeof(data));
    publishDirect("nms/v1/announce", "announce", data, /*retained=*/false);
}

static void emitStatusOnline() {
    strncpy(s_state, "online", sizeof(s_state) - 1);
    char data[64];
    snprintf(data, sizeof(data), "{\"state\":\"online\",\"since\":%u,\"job\":null}",
             (unsigned)s_bootTs);
    publishDirect(s_topicStatus, "status", data, /*retained=*/true);
}

// --- inbound command handling ---------------------------------------------

static void handleIdentify(const char* jobId, const char* argsJson) {
    // Parse duration_s (default 3s). Non-blocking: main's loop blinks the LED
    // until the window closes, so the MQTT task is never stalled.
    int duration = 3;
    JsonDocument a;
    if (deserializeJson(a, argsJson) == DeserializationError::Ok) {
        duration = a["duration_s"] | 3;
    }
    if (duration < 1) duration = 1;
    s_identifyUntilMs = millis() + (uint32_t)duration * 1000u;

    char data[80];
    snprintf(data, sizeof(data), "{\"event\":\"accepted\",\"job_id\":\"%s\"}", jobId);
    emitResultDirect(data);
    snprintf(data, sizeof(data),
             "{\"event\":\"done\",\"job_id\":\"%s\",\"chunks\":0,\"results\":0,\"duration_ms\":0}",
             jobId);
    emitResultDirect(data);
}

static void emitAcceptedDone(const char* jobId) {
    char data[96];
    snprintf(data, sizeof(data), "{\"event\":\"accepted\",\"job_id\":\"%s\"}", jobId);
    emitResultDirect(data);
    snprintf(data, sizeof(data),
             "{\"event\":\"done\",\"job_id\":\"%s\",\"chunks\":0,\"results\":0,\"duration_ms\":0}",
             jobId);
    emitResultDirect(data);
}

static void emitError(const char* jobId, const char* code, const char* message) {
    char data[160];
    snprintf(data, sizeof(data),
             "{\"event\":\"error\",\"job_id\":\"%s\",\"code\":\"%s\",\"message\":\"%s\"}",
             jobId, code, message);
    emitResultDirect(data);
}

static void onMessage(char* /*topic*/, uint8_t* payload, unsigned int length) {
    char cmd[24];
    char jobId[36];
    static char args[512];
    if (!parseCmd((const char*)payload, length, cmd, sizeof(cmd),
                  jobId, sizeof(jobId), args, sizeof(args))) {
        return;  // malformed or non-cmd envelope — rejected silently
    }

    // Control commands are the mandatory baseline: handled
    // here, never queued, never rejected as unsupported, never deduped (a repeat
    // cancel must still take effect).
    if (strcmp(cmd, "cancel") == 0) {
        strncpy(s_cancelJobId, jobId, sizeof(s_cancelJobId) - 1);
        s_cancelJobId[sizeof(s_cancelJobId) - 1] = '\0';
        s_cancelPending = true;
        return;
    }
    if (strcmp(cmd, "identify") == 0) {
        handleIdentify(jobId, args);
        return;
    }
    if (strcmp(cmd, "get_config") == 0) {
        emitAcceptedDone(jobId);
        return;
    }
    if (strcmp(cmd, "set_monitor") == 0) {
        // Persist to NVS and apply; the worker's idle tick runs due cycles.
        // Control command — never rejected, never queued.
        monitorSetFromArgs(args);
        emitAcceptedDone(jobId);
        return;
    }
    if (strcmp(cmd, "reboot") == 0) {
        emitAcceptedDone(jobId);
        s_mqtt.loop();  // best-effort flush before the reset
        delay(300);
        ESP.restart();
        return;
    }

    // Recon commands: dedupe (QoS-1 redelivery safety), then gate on
    // advertised capability, then hand to the worker's job queue. isCapable is
    // driven by the worker registry, so a command the node did not advertise in
    // `announce` is rejected here as `unsupported` — e.g.
    // wifi_survey (no radio runner yet) or trace when raw ICMP is unavailable.
    if (seenBefore(jobId)) {
        return;
    }
    if (!isRecon(cmd) || !isCapable(cmd)) {
        emitError(jobId, "unsupported", "command not supported by this node");
        return;
    }

    JobRequest req;
    strncpy(req.cmd, cmd, sizeof(req.cmd) - 1);
    req.cmd[sizeof(req.cmd) - 1] = '\0';
    strncpy(req.job_id, jobId, sizeof(req.job_id) - 1);
    req.job_id[sizeof(req.job_id) - 1] = '\0';
    strncpy(req.args, args, sizeof(req.args) - 1);
    req.args[sizeof(req.args) - 1] = '\0';

    // Depth-1 queue: a job already running (queue full) means busy.
    if (s_jobQueue == nullptr || xQueueSend(s_jobQueue, &req, 0) != pdTRUE) {
        emitError(jobId, "busy", "a job is already running");
    }
}

// --- public API -----------------------------------------------------------

void mqttInit(const ProbeConfig& cfg) {
    s_cfg = cfg;
    buildTopic(s_topicCmd, sizeof(s_topicCmd), "cmd");
    buildTopic(s_topicResult, sizeof(s_topicResult), "result");
    buildTopic(s_topicStatus, sizeof(s_topicStatus), "status");
    buildTopic(s_topicTelemetry, sizeof(s_topicTelemetry), "telemetry");
    buildTopic(s_topicMonitor, sizeof(s_topicMonitor), "monitor");

    s_mqtt.setServer(s_cfg.broker_host, s_cfg.broker_port);
    // Must clear the protocol's 1024-byte payload cap plus envelope overhead;
    // PubSubClient's 256-byte default would silently truncate large frames.
    s_mqtt.setBufferSize(1280);
    s_mqtt.setCallback(onMessage);

    if (s_jobQueue == nullptr) {
        s_jobQueue = xQueueCreate(1, sizeof(JobRequest));
    }
}

bool mqttConnect() {
    // Last Will: retained on `status` so a restarting server learns of an
    // ungraceful death on subscribe. Built as a full
    // envelope, matching the virtual probe's will payload.
    static char will[PAYLOAD_MAX_BYTES + 1];
    size_t willLen = buildEnvelope(will, sizeof(will), "status", getNodeId(),
                                   nowTs(), "{\"state\":\"offline\",\"reason\":\"lwt\"}");
    bool ok;
    if (willLen > 0) {
        ok = s_mqtt.connect(getNodeId(), s_cfg.mqtt_user, s_cfg.mqtt_pass,
                            s_topicStatus, /*willQos=*/1, /*willRetain=*/true, will);
    } else {
        ok = s_mqtt.connect(getNodeId(), s_cfg.mqtt_user, s_cfg.mqtt_pass);
    }
    if (!ok) {
        return false;
    }

    if (s_bootTs == 0) {
        s_bootTs = nowTs();
    }
    s_mqtt.subscribe(s_topicCmd, /*qos=*/1);
    emitAnnounce();
    emitStatusOnline();
    return true;
}

bool mqttConnected() { return s_mqtt.connected(); }

void mqttReconnectWithBackoff() {
    static uint32_t attempt = 0;
    if (mqttConnect()) {
        attempt = 0;
        return;
    }
    delay(computeBackoffMs(attempt++, esp_random()));
}

void mqttLoop() { s_mqtt.loop(); }

void mqttDrainOutbox() {
    OutboxFrame frame;
    while (outboxDequeue(frame)) {
        if (!s_mqtt.publish(frame.topic, (const uint8_t*)frame.json, frame.len,
                            /*retained=*/false)) {
            break;  // link went down mid-drain; the rest stays queued
        }
    }
}

void mqttEmitTelemetry() {
    uint32_t now = millis();
    if (s_lastTelemetryMs != 0 && (now - s_lastTelemetryMs) < 30000u) {
        return;
    }
    s_lastTelemetryMs = now;

    // rssi/channel are only meaningful while associated; clamp rssi into the
    // schema's [-100, 0] range defensively.
    int rssi = WiFi.RSSI();
    if (rssi > 0) rssi = 0;
    if (rssi < -100) rssi = -100;
    int channel = WiFi.channel();
    if (channel < 1) channel = 1;
    if (channel > 14) channel = 14;

    char data[192];
    snprintf(data, sizeof(data),
             "{\"free_heap\":%u,\"uptime_s\":%u,\"rssi\":%d,\"channel\":%d,"
             "\"state\":\"%s\",\"jobs_done\":%u}",
             (unsigned)ESP.getFreeHeap(), (unsigned)(millis() / 1000u), rssi,
             channel, s_state, (unsigned)s_jobsDone);
    publishDirect(s_topicTelemetry, "telemetry", data, /*retained=*/false);
}

void mqttEnqueueResult(const char* dataJson) {
    static char buf[PAYLOAD_MAX_BYTES + 1];
    size_t n = buildEnvelope(buf, sizeof(buf), "result", getNodeId(), nowTs(), dataJson);
    if (n > 0) {
        outboxEnqueue(s_topicResult, buf, n);
    }
}

void mqttEnqueueMonitor(const char* dataJson) {
    static char buf[PAYLOAD_MAX_BYTES + 1];
    size_t n = buildEnvelope(buf, sizeof(buf), "monitor", getNodeId(), nowTs(), dataJson);
    if (n > 0) {
        outboxEnqueue(s_topicMonitor, buf, n);
    }
}

// --- survey choreography ---------------------------------------------------

void mqttPublishAccepted(const char* jobId) {
    char data[96];
    snprintf(data, sizeof(data), "{\"event\":\"accepted\",\"job_id\":\"%s\"}", jobId);
    publishDirect(s_topicResult, "result", data, /*retained=*/false);
}

void mqttPublishSurveying(uint32_t expectBackInS) {
    strncpy(s_state, "surveying", sizeof(s_state) - 1);
    s_state[sizeof(s_state) - 1] = '\0';
    char data[80];
    snprintf(data, sizeof(data), "{\"state\":\"surveying\",\"expect_back_in\":%u}",
             (unsigned)expectBackInS);
    publishDirect(s_topicStatus, "status", data, /*retained=*/true);
}

void mqttDisconnectClean() { s_mqtt.disconnect(); }

void mqttSurveyPauseBegin() {
    s_surveyPausedAck = false;
    s_surveyPause = true;
}
void mqttSurveyPauseEnd() {
    s_surveyPause = false;
    s_surveyPausedAck = false;
}
bool mqttSurveyPaused() { return s_surveyPause; }
void mqttSurveyMarkPausedAck() { s_surveyPausedAck = true; }
bool mqttSurveyPausedAck() { return s_surveyPausedAck; }

QueueHandle_t mqttJobQueue() { return s_jobQueue; }

bool mqttTakeCancel(char* jobIdOut, size_t size) {
    if (!s_cancelPending) {
        return false;
    }
    strncpy(jobIdOut, s_cancelJobId, size - 1);
    jobIdOut[size - 1] = '\0';
    s_cancelPending = false;
    return true;
}

bool mqttIdentifyActive() {
    return s_identifyUntilMs != 0 && (int32_t)(s_identifyUntilMs - millis()) > 0;
}

void mqttSetState(const char* state) {
    strncpy(s_state, state, sizeof(s_state) - 1);
    s_state[sizeof(s_state) - 1] = '\0';
}

void mqttIncJobsDone() { ++s_jobsDone; }
