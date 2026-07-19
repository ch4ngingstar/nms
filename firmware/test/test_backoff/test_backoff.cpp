// Native unit tests for the reconnect backoff.
//
// Pure integer math, no ArduinoJson — compiles and runs under g++ as well as
// `pio test -e native`. Pins the base-doubling schedule, the 60s ceiling, and
// the jitter band so a change to the curve is caught here rather than on a fleet.

#include <unity.h>

#include "backoff.h"

void setUp(void) {}
void tearDown(void) {}

// Base doubles from 1s with a fixed -25% jitter floor when jitterRand == 0:
// attempt 0 -> 0.75 * 1000 = 750, then 1500, 3000, 6000, ...
static void test_lower_bound_and_doubling(void) {
    TEST_ASSERT_EQUAL_UINT32(750u, computeBackoffMs(0, 0));
    TEST_ASSERT_EQUAL_UINT32(1500u, computeBackoffMs(1, 0));
    TEST_ASSERT_EQUAL_UINT32(3000u, computeBackoffMs(2, 0));
    TEST_ASSERT_EQUAL_UINT32(6000u, computeBackoffMs(3, 0));
}

// The jitter band for a given attempt is [0.75*base, 1.25*base]: floor 0.75*base
// plus up to span = base/2. For attempt 0 (base 1000) that is [750, 1250].
static void test_jitter_within_band(void) {
    for (uint32_t j = 0; j < 100000u; j += 137u) {
        uint32_t d = computeBackoffMs(0, j);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(750u, d);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(1250u, d);
    }
    // The extreme jitter word still lands inside the band.
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1250u, computeBackoffMs(0, 0xFFFFFFFFu));
}

// The base saturates: the shift is capped so 1000<<shift can't overflow, and the
// result is clamped to the 60s ceiling regardless of attempt or jitter.
static void test_ceiling_never_exceeded(void) {
    for (uint32_t attempt = 0; attempt < 40u; ++attempt) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(60000u, computeBackoffMs(attempt, 0xFFFFFFFFu));
    }
    // Deep into saturation the floor is 0.75 * 60000 = 45000.
    TEST_ASSERT_EQUAL_UINT32(45000u, computeBackoffMs(30, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lower_bound_and_doubling);
    RUN_TEST(test_jitter_within_band);
    RUN_TEST(test_ceiling_never_exceeded);
    return UNITY_END();
}
