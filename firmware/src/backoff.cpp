#include "backoff.h"

uint32_t computeBackoffMs(uint32_t attempt, uint32_t jitterRand) {
    // Base doubles 1s, 2s, 4s, ... capped at a 60s ceiling (protocol §8.4).
    uint32_t shift = attempt > 6 ? 6 : attempt;   // cap the shift so 1000<<shift can't overflow
    uint32_t base = 1000u << shift;               // 1000 .. 64000
    if (base > 60000u) {
        base = 60000u;
    }
    // Jitter within [-25%, +25%] so a fleet returning after a broker restart
    // does not retry in lockstep. Result is clamped back under the ceiling.
    uint32_t span = base / 2;                      // full jitter width
    uint32_t delay = (base - base / 4) + (jitterRand % (span + 1));
    if (delay > 60000u) {
        delay = 60000u;
    }
    return delay;
}
