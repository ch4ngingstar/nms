#include "job_dedup.h"

#include <string.h>

bool jobDedupSeen(JobDedup& ring, const char* jobId) {
    for (size_t i = 0; i < JOB_DEDUP_CAPACITY; ++i) {
        if (strcmp(ring.ids[i], jobId) == 0) {
            return true;
        }
    }
    // Record it (drop-oldest ring) so a QoS-1 redelivery is ignored next time.
    strncpy(ring.ids[ring.idx], jobId, JOB_DEDUP_ID_MAX - 1);
    ring.ids[ring.idx][JOB_DEDUP_ID_MAX - 1] = '\0';
    ring.idx = (uint8_t)((ring.idx + 1) % JOB_DEDUP_CAPACITY);
    return false;
}
