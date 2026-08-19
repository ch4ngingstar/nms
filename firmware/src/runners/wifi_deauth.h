#pragma once
#include <cstdint>

/// Maximum number of client stations discovered during pre-deauth sniffing.
constexpr size_t MAX_DEAUTH_CLIENTS = 32;

struct DeauthArgs {
    uint8_t target_bssid[6];
    uint8_t channel;
    uint16_t duration_s;
    uint8_t bursts;
    bool confirm;  // Must be true or the runner refuses (spec §6.4 safety interlock)
};

struct DeauthResult {
    uint8_t target_bssid[6];
    uint8_t channel;
    uint16_t clients_found;
    uint32_t frames_sent;
    uint32_t duration_ms;
};

/// Run the wifi_deauth command.  Caller must have already:
///   1. Published "accepted" while MQTT is still connected
///   2. Published retained status {"state":"attacking","expect_back_in":...}
///   3. Sent a clean MQTT DISCONNECT (suppresses LWT)
///   4. Left the AP
/// After this function returns, the caller reconnects and streams the result.
/// Returns true on success, false on bad_args (confirm != true).
bool runWifiDeauth(const DeauthArgs& args, DeauthResult& result);
