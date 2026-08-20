#include "outbox.h"

#include <string.h>

#if defined(ESP_PLATFORM) || defined(ARDUINO)

// --- device build: FreeRTOS queue ----------------------------------------

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static QueueHandle_t s_queue = nullptr;

// Cross-core counter: the worker (producer) increments on overflow, the MQTT
// task (consumer) reads-and-clears. std::atomic keeps that race-free without a
// mutex; the queue itself carries the frames.
static std::atomic<uint32_t> s_dropped{0};

void outboxInit() {
    if (s_queue == nullptr) {
        s_queue = xQueueCreate(OUTBOX_CAPACITY, sizeof(OutboxFrame));
    }
}

bool outboxEnqueue(const char* topic, const char* json, size_t len) {
    if (s_queue == nullptr || len > OUTBOX_JSON_MAX) {
        return false;
    }

    OutboxFrame frame;
    // topic is used as a C string by PubSubClient::publish, so it must be
    // NUL-terminated; the payload is published by length and need not be.
    size_t i = 0;
    for (; topic[i] != '\0' && i + 1 < OUTBOX_TOPIC_MAX; ++i) {
        frame.topic[i] = topic[i];
    }
    frame.topic[i] = '\0';
    memcpy(frame.json, json, len);
    frame.len = (uint16_t)len;

    // Non-blocking send. On a full queue, drop the oldest frame and retry so the
    // freshest observation wins (drop-oldest semantics).
    if (xQueueSend(s_queue, &frame, 0) != pdTRUE) {
        OutboxFrame discarded;
        if (xQueueReceive(s_queue, &discarded, 0) == pdTRUE) {
            s_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        // If the retry still fails (consumer refilled it in between) the send is
        // reported as failed rather than looping — the caller treats that as a
        // further drop opportunity on its next attempt.
        return xQueueSend(s_queue, &frame, 0) == pdTRUE;
    }
    return true;
}

bool outboxDequeue(OutboxFrame& out) {
    if (s_queue == nullptr) {
        return false;
    }
    return xQueueReceive(s_queue, &out, 0) == pdTRUE;
}

uint32_t outboxGetAndResetDropped() {
    return s_dropped.exchange(0, std::memory_order_relaxed);
}

#else

// --- native host shim ------------------------------------------------------
//
// A plain drop-oldest ring with semantics identical to the FreeRTOS queue above:
// bounded capacity, drop-oldest on overflow, read-and-clear dropped counter.
// The host tests drive producer and consumer sequentially on one thread, so no
// atomics or FreeRTOS primitives are needed — which is the whole point of the
// shim: outbox coverage runs under g++/Unity without a board.
//
// Unlike the device build (whose outboxInit is create-once so a reconnect keeps
// buffered frames), this outboxInit resets the ring — exactly what a unit test's
// setUp wants between cases. The enqueue/dequeue/drop contract is what the tests
// pin, and it matches byte for byte.

static OutboxFrame s_ring[OUTBOX_CAPACITY];
static size_t s_head = 0;    // index of the oldest queued frame
static size_t s_count = 0;   // number of frames currently queued
static uint32_t s_dropped = 0;
static bool s_inited = false;

void outboxInit() {
    s_head = 0;
    s_count = 0;
    s_dropped = 0;
    s_inited = true;
}

bool outboxEnqueue(const char* topic, const char* json, size_t len) {
    if (!s_inited || len > OUTBOX_JSON_MAX) {
        return false;
    }
    if (s_count == OUTBOX_CAPACITY) {
        // Full: drop the oldest so the freshest observation wins.
        s_head = (s_head + 1) % OUTBOX_CAPACITY;
        --s_count;
        ++s_dropped;
    }
    size_t tail = (s_head + s_count) % OUTBOX_CAPACITY;
    OutboxFrame& frame = s_ring[tail];

    size_t i = 0;
    for (; topic[i] != '\0' && i + 1 < OUTBOX_TOPIC_MAX; ++i) {
        frame.topic[i] = topic[i];
    }
    frame.topic[i] = '\0';
    memcpy(frame.json, json, len);
    frame.len = (uint16_t)len;
    ++s_count;
    return true;
}

bool outboxDequeue(OutboxFrame& out) {
    if (!s_inited || s_count == 0) {
        return false;
    }
    out = s_ring[s_head];
    s_head = (s_head + 1) % OUTBOX_CAPACITY;
    --s_count;
    return true;
}

uint32_t outboxGetAndResetDropped() {
    uint32_t d = s_dropped;
    s_dropped = 0;
    return d;
}

#endif
