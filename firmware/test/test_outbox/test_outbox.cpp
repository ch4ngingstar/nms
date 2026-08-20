// Native unit tests for the bounded outbox.
//
// Runs against the native ring-buffer shim in outbox.cpp (the FreeRTOS queue is
// #ifdef'd out off-device). No ArduinoJson. Pins FIFO order, the drop-oldest
// overflow policy, the read-and-clear dropped counter, and oversize rejection.

#include <unity.h>
#include <string.h>
#include <stdio.h>

#include "outbox.h"

void setUp(void) { outboxInit(); }  // native init resets the ring between cases
void tearDown(void) {}

static void test_roundtrip_preserves_topic_and_payload(void) {
    const char* topic = "nms/v1/node/probe-a4c1f8/result";
    const char* body = "{\"event\":\"accepted\"}";
    size_t len = strlen(body);
    TEST_ASSERT_TRUE(outboxEnqueue(topic, body, len));

    OutboxFrame f;
    TEST_ASSERT_TRUE(outboxDequeue(f));
    TEST_ASSERT_EQUAL_STRING(topic, f.topic);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)len, f.len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(f.json, body, len));
}

static void test_dequeue_empty_returns_false(void) {
    OutboxFrame f;
    TEST_ASSERT_FALSE(outboxDequeue(f));
}

static void test_fifo_order(void) {
    TEST_ASSERT_TRUE(outboxEnqueue("t", "a", 1));
    TEST_ASSERT_TRUE(outboxEnqueue("t", "b", 1));
    TEST_ASSERT_TRUE(outboxEnqueue("t", "c", 1));

    OutboxFrame f;
    outboxDequeue(f); TEST_ASSERT_EQUAL_INT('a', f.json[0]);
    outboxDequeue(f); TEST_ASSERT_EQUAL_INT('b', f.json[0]);
    outboxDequeue(f); TEST_ASSERT_EQUAL_INT('c', f.json[0]);
    TEST_ASSERT_FALSE(outboxDequeue(f));
}

// Overflowing the ring drops the oldest frames; the newest CAPACITY survive.
static void test_drop_oldest_on_overflow(void) {
    const uint32_t overflow = 3;
    char body[8];
    for (uint32_t i = 0; i < OUTBOX_CAPACITY + overflow; ++i) {
        snprintf(body, sizeof(body), "%u", i);
        TEST_ASSERT_TRUE(outboxEnqueue("t", body, strlen(body)));
    }
    // Exactly `overflow` frames were dropped, and the counter clears on read.
    TEST_ASSERT_EQUAL_UINT32(overflow, outboxGetAndResetDropped());
    TEST_ASSERT_EQUAL_UINT32(0u, outboxGetAndResetDropped());

    // The first three ("0","1","2") were evicted; the oldest survivor is "3".
    OutboxFrame f;
    TEST_ASSERT_TRUE(outboxDequeue(f));
    TEST_ASSERT_EQUAL_INT('3', f.json[0]);
}

static void test_oversize_payload_rejected(void) {
    static char big[OUTBOX_JSON_MAX + 16];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_FALSE(outboxEnqueue("t", big, OUTBOX_JSON_MAX + 1));
    // A payload exactly at the cap is accepted.
    TEST_ASSERT_TRUE(outboxEnqueue("t", big, OUTBOX_JSON_MAX));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_preserves_topic_and_payload);
    RUN_TEST(test_dequeue_empty_returns_false);
    RUN_TEST(test_fifo_order);
    RUN_TEST(test_drop_oldest_on_overflow);
    RUN_TEST(test_oversize_payload_rejected);
    return UNITY_END();
}
