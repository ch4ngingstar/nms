#include "config.h"

#include <string.h>
#include <Preferences.h>

// The config is stored as one opaque blob under a single key rather than as
// field-by-field entries: it is written and read atomically, and the struct's
// `valid` flag rides along so "does a config exist" needs no separate sentinel.
static const char* CFG_KEY = "cfg";

bool loadConfig(ProbeConfig& out) {
    memset(&out, 0, sizeof(out));  // Unprovisioned default: all-zero, valid=false.

    Preferences prefs;
    // Read-only open. If the namespace was never created (fresh device) this
    // still succeeds; getBytesLength then reports 0.
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
        return false;
    }

    // A blob written by an older, differently-sized struct is treated as absent
    // rather than memcpy'd into a mismatched layout — safer than trusting it.
    bool ok = prefs.getBytesLength(CFG_KEY) == sizeof(ProbeConfig) &&
              prefs.getBytes(CFG_KEY, &out, sizeof(out)) == sizeof(out);
    prefs.end();

    if (!ok) {
        memset(&out, 0, sizeof(out));
        return false;
    }
    return out.valid;
}

bool saveConfig(const ProbeConfig& cfg) {
    // Copy so we can force `valid` true regardless of what the caller passed —
    // a persisted config is by definition valid.
    ProbeConfig stored = cfg;
    stored.valid = true;

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        return false;
    }
    size_t written = prefs.putBytes(CFG_KEY, &stored, sizeof(stored));
    prefs.end();
    return written == sizeof(stored);
}

bool clearConfig() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        return false;
    }
    bool ok = prefs.clear();  // Wipes every key in the "nms" namespace.
    prefs.end();
    return ok;
}
