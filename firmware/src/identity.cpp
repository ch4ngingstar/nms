#include "identity.h"

#include <stdio.h>
#include <esp_mac.h>

// The factory MAC never changes, so both strings are derived on first use and
// cached. Static buffers, no heap — the same memory discipline used elsewhere
// applies here too even though identity is off the hot path.
static char s_nodeId[NODE_ID_LEN] = {0};
static char s_macStr[MAC_STR_LEN] = {0};

// Reads the six-byte factory MAC once. esp_efuse_mac_get_default() fills the
// array high byte first: mac[0..2] is the OUI, mac[3..5] the device-specific
// low three bytes the node_id is built from.
static const uint8_t* factoryMac() {
    static uint8_t mac[6];
    static bool read = false;
    if (!read) {
        esp_efuse_mac_get_default(mac);
        read = true;
    }
    return mac;
}

const char* getNodeId() {
    if (s_nodeId[0] == '\0') {
        const uint8_t* mac = factoryMac();
        // Lowercase hex is required by the regex; %02x guarantees it.
        snprintf(s_nodeId, sizeof(s_nodeId), "probe-%02x%02x%02x",
                 mac[3], mac[4], mac[5]);
    }
    return s_nodeId;
}

const char* getMacString() {
    if (s_macStr[0] == '\0') {
        const uint8_t* mac = factoryMac();
        snprintf(s_macStr, sizeof(s_macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return s_macStr;
}
