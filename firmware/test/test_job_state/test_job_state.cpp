// Native unit tests for the duplicate-job dedup ring (protocol §7.1, §5).
//
// Pure, no ArduinoJson. Pins the QoS-1 redelivery guard: a job_id is accepted
// once, a repeat is suppressed, and the ring forgets only the oldest id once it
// overflows its 8 slots.

#include <unity.h>
#include <stdio.h>

#include "job_dedup.h"

void setUp(void) {}
void tearDown(void) {}

static void test_first_sighting_not_seen(void) {
    JobDedup ring{};
    TEST_ASSERT_FALSE(jobDedupSeen(ring, "job-7f3a91"));
}

static void test_repeat_is_seen(void) {
    JobDedup ring{};
    TEST_ASSERT_FALSE(jobDedupSeen(ring, "job-7f3a91"));
    TEST_ASSERT_TRUE(jobDedupSeen(ring, "job-7f3a91"));
    // A second repeat is still suppressed (idempotent redelivery).
    TEST_ASSERT_TRUE(jobDedupSeen(ring, "job-7f3a91"));
}

static void test_distinct_ids_tracked_independently(void) {
    JobDedup ring{};
    TEST_ASSERT_FALSE(jobDedupSeen(ring, "job-a"));
    TEST_ASSERT_FALSE(jobDedupSeen(ring, "job-b"));
    TEST_ASSERT_TRUE(jobDedupSeen(ring, "job-a"));
    TEST_ASSERT_TRUE(jobDedupSeen(ring, "job-b"));
}

// Exactly JOB_DEDUP_CAPACITY ids fit; the next distinct id evicts the oldest.
static void test_ring_evicts_only_oldest(void) {
    JobDedup ring{};
    char id[16];
    for (size_t i = 0; i < JOB_DEDUP_CAPACITY; ++i) {
        snprintf(id, sizeof(id), "job-%zu", i);
        TEST_ASSERT_FALSE(jobDedupSeen(ring, id));
    }
    // All 8 still remembered (checking a hit does not disturb the ring).
    TEST_ASSERT_TRUE(jobDedupSeen(ring, "job-0"));
    TEST_ASSERT_TRUE(jobDedupSeen(ring, "job-7"));

    // A 9th distinct id overwrites the oldest slot, which held "job-0".
    TEST_ASSERT_FALSE(jobDedupSeen(ring, "job-8"));
    TEST_ASSERT_FALSE(jobDedupSeen(ring, "job-0"));  // evicted -> treated as new
    // "job-7" was not the victim, so it is still suppressed.
    TEST_ASSERT_TRUE(jobDedupSeen(ring, "job-7"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_sighting_not_seen);
    RUN_TEST(test_repeat_is_seen);
    RUN_TEST(test_distinct_ids_tracked_independently);
    RUN_TEST(test_ring_evicts_only_oldest);
    return UNITY_END();
}
