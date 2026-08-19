// Bounded cross-task outbox (firmware spec §5, §6.3; protocol §8.5).
//
// One FreeRTOS queue serves two jobs at once: it hands pre-serialized frames
// from the worker task (core 1) to the MQTT task (core 0), and — because it
// simply stops draining while MQTT is down — it is also the offline buffer.
// On overflow the oldest frame is dropped and a counter increments; the next
// producer to build a frame folds that count into its `dropped` field.
//
// Single-producer / single-consumer: only the worker enqueues, only the MQTT
// task dequeues. FreeRTOS queue primitives are internally interrupt-safe, so no
// additional mutex is needed.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "envelope.h"  // PAYLOAD_MAX_BYTES

// Sized so a frame holds a full-cap envelope (protocol §5.3) plus the longest
// per-node topic. 32 slots is the offline-buffer depth.
static const size_t OUTBOX_TOPIC_MAX = 80;
static const size_t OUTBOX_JSON_MAX = PAYLOAD_MAX_BYTES;
static const size_t OUTBOX_CAPACITY = 32;

// A pre-serialized frame ready to publish: NUL-terminated topic plus the raw
// JSON payload and its byte length (the payload is published by length, not by
// NUL, so it may contain no terminator).
struct OutboxFrame {
    char topic[OUTBOX_TOPIC_MAX];
    char json[OUTBOX_JSON_MAX];
    uint16_t len;
};

// Creates the underlying queue. Call once at boot before any enqueue/dequeue.
void outboxInit();

// Copies one frame into the queue. If the queue is full, drops the oldest frame
// (incrementing the dropped counter) and retries so the newest data survives.
// Returns false only if the queue is uninitialized or the payload exceeds
// OUTBOX_JSON_MAX; true once the frame is queued.
bool outboxEnqueue(const char* topic, const char* json, size_t len);

// Pops the next frame into `out` without blocking. Returns true if one was
// available, false if the queue was empty.
bool outboxDequeue(OutboxFrame& out);

// Returns the number of frames dropped since the last call and resets it to 0.
uint32_t outboxGetAndResetDropped();
