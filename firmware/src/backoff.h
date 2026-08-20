// Reconnect backoff — pure and host-testable, split out of
// mqtt_client so the native Unity suite can exercise it without pulling in the
// PubSubClient / FreeRTOS translation unit. mqtt_client.cpp calls it from the
// reconnect path.
#pragma once

#include <stdint.h>

// Returns a reconnect delay in ms for `attempt` (0-based). The base doubles from
// 1s toward a 60s ceiling; `jitterRand` (any 32-bit random value) spreads the
// result across roughly [0.75x, 1.0x] of the base so a fleet returning after a
// broker restart does not retry in lockstep. Result is bounded to <= 60000.
uint32_t computeBackoffMs(uint32_t attempt, uint32_t jitterRand);
