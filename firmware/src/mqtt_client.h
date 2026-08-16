// MQTT connection management.
//
// This module is the sole owner of the PubSubClient — the single-writer
// discipline that lets it skip a mutex. It runs entirely on the MQTT task
// (core 0): it dials the broker, publishes announce/status/telemetry directly,
// drains the worker's outbox, and parses inbound commands. Recon jobs are
// handed to the worker task through a depth-1 queue; cancel/identify/reboot/
// get_config are handled here immediately.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "backoff.h"  // computeBackoffMs — extracted; declared here for callers

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Firmware version reported in `announce.fw`; must satisfy ^\d+\.\d+\.\d+$.
#define FW_VERSION "1.0.0"

// A recon job handed to the worker task. `args` holds the serialized args
// object; sizes match the outbox/envelope budget.
struct JobRequest {
    char cmd[24];
    char job_id[36];
    char args[512];
};

// Stores a copy of the config, points PubSubClient at the broker, raises its
// buffer to 1280 bytes, registers the message callback, and creates the worker
// job queue. Call once at boot (provisioned path).
void mqttInit(const ProbeConfig& cfg);

// Attempts one connect: client id = node_id, LWT retained on the status topic
// carrying {"state":"offline","reason":"lwt"}. On success subscribes to `cmd`,
// publishes `announce`, and publishes retained `status:online`. Returns whether
// the broker connection is up.
bool mqttConnect();

// True while the broker connection is established.
bool mqttConnected();

// One backoff step: if reconnect fails, sleeps for an exponentially growing,
// jittered interval (1s..60s). Call from loop() while offline.
void mqttReconnectWithBackoff();

// Services the PubSubClient (keepalive + inbound dispatch). Call every loop.
void mqttLoop();

// Publishes every frame currently queued in the outbox. Call every loop.
void mqttDrainOutbox();

// Publishes a telemetry sample if the 30s interval has elapsed.
void mqttEmitTelemetry();

// Builds a `result` envelope around `dataJson` and enqueues it on the outbox.
// This is the worker task's single path to the wire — it never touches
// PubSubClient itself.
void mqttEnqueueResult(const char* dataJson);

// Builds a `monitor` envelope around `dataJson` and enqueues it on the node's
// monitor topic. Called by the monitor scheduler from the worker.
void mqttEnqueueMonitor(const char* dataJson);

// --- survey choreography ---------------------------------------------------
//
// During a survey the worker/radio owns PubSubClient directly while the MQTT task
// steps aside, so exactly one task writes it at a time (single-writer).
// These are the operations the radio drives on that borrowed client.

// Publishes `accepted`/retained `surveying` directly (QoS 0) while still
// connected — the first two steps of the survey handoff, before the disconnect.
void mqttPublishAccepted(const char* jobId);
void mqttPublishSurveying(uint32_t expectBackInS);

// Clean MQTT DISCONNECT — the final handoff step, which suppresses the Last Will.
void mqttDisconnectClean();

// Pause/resume handoff: the radio sets the pause, waits for the MQTT task to
// acknowledge (it stops touching PubSubClient and returns early from loop()),
// drives the survey, then resumes. Ack is one-shot per pause.
void mqttSurveyPauseBegin();
void mqttSurveyPauseEnd();
bool mqttSurveyPaused();
void mqttSurveyMarkPausedAck();
bool mqttSurveyPausedAck();

// The worker task receives JobRequests from this queue (depth 1). Valid after
// mqttInit.
QueueHandle_t mqttJobQueue();

// Worker polls this: if a cancel arrived for a job, copies its id into
// jobIdOut and returns true (clearing the pending flag). Otherwise false.
bool mqttTakeCancel(char* jobIdOut, size_t size);

// True while an `identify` window is open, so the caller can blink the LED.
bool mqttIdentifyActive();

// Updates the state string reported in status/telemetry (e.g. "busy").
void mqttSetState(const char* state);

// Increments the completed-job counter reported in telemetry.
void mqttIncJobsDone();

// computeBackoffMs now lives in backoff.h, included above.
