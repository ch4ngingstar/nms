// Node identity derivation (spec §4.1).
//
// node_id is "probe-" followed by the low three bytes of the factory MAC in
// lowercase hex. This must match protocol/topics.py's regex
// ^probe-(?:[0-9a-f]{6}|server)$ exactly: a mismatch is not a soft failure —
// every topic construction and every broker ACL entry is rejected by it.
#pragma once

#include <stddef.h>

// Length of "probe-" + 6 hex digits + NUL.
static const size_t NODE_ID_LEN = 13;
// Length of "aa:bb:cc:dd:ee:ff" + NUL.
static const size_t MAC_STR_LEN = 18;

// Returns the node identity, e.g. "probe-a4c1f8". Derived once on first call
// from esp_efuse_mac_get_default() and cached in a static buffer; the pointer
// is stable for the process lifetime and is never freed.
const char* getNodeId();

// Returns the full factory MAC as colon-delimited lowercase hex, e.g.
// "a4:c1:f8:12:34:56", for the announce `mac` field (announce.schema.json:
// ^([0-9a-f]{2}:){5}[0-9a-f]{2}$). Cached like getNodeId().
const char* getMacString();
