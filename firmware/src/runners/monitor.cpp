#include "monitor.h"
#include "../radio.h"       // radioInStation()
#include "../mqtt_client.h" // mqttEnqueueMonitor
#include "../envelope.h"    // PAYLOAD_MAX_BYTES

#include <string.h>
#include <stdio.h>

#include <ArduinoJson.h>

#if defined(ESP_PLATFORM) || defined(ARDUINO)
#define NMS_HAS_DEV 1
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"          // NVS_NAMESPACE
#include "lwip/sockets.h"
#include "lwip/inet.h"
// Raw ICMP for real ping latency, else fall back to a TCP-liveness probe.
#if (defined(CONFIG_LWIP_RAW) && CONFIG_LWIP_RAW) || (defined(LWIP_RAW) && LWIP_RAW)
#define NMS_HAS_ICMP 1
#else
#define NMS_HAS_ICMP 0
#endif
#else
#define NMS_HAS_DEV 0
#define NMS_HAS_ICMP 0
#endif

static const char* MON_KEY = "mon";
static const size_t MON_JSON_MAX = 1024;  // set_monitor args fit within the 1024B cap

// Config kept as the raw args JSON (persisted verbatim) plus the parsed header;
// devices are re-parsed each cycle, which is cheap and avoids a second schema.
static char s_json[MON_JSON_MAX] = {0};
static bool s_enabled = false;
static uint32_t s_intervalS = 0;
static uint32_t s_lastMs = 0;
static bool s_ran = false;

// --- header parse ---------------------------------------------------------

static void parseHeader() {
    s_enabled = false;
    s_intervalS = 0;
    if (s_json[0] == '\0') return;
    JsonDocument doc;
    if (deserializeJson(doc, s_json) != DeserializationError::Ok) return;
    s_enabled = doc["enabled"] | false;
    s_intervalS = (uint32_t)(int)(doc["interval_s"] | 0);
}

#if NMS_HAS_DEV

// The monitor schedule (s_json + parsed header + timing) is touched from two
// cores: set_monitor is applied on the MQTT task (core 0) while monitorTick runs
// cycles on the worker task (core 1). A FreeRTOS mutex (not a portMUX spinlock —
// the guarded region parses JSON and must not run with interrupts masked) gives
// mutual exclusion plus the memory barrier a lock-free scheme would lack. Until
// monitorInit() creates it the guard is a no-op; that boot window is single
// benign torn read at worst (a fixed-size buffer, a self-healing next cycle).
static SemaphoreHandle_t s_monMutex = nullptr;
static inline void lockCfg()   { if (s_monMutex) xSemaphoreTake(s_monMutex, portMAX_DELAY); }
static inline void unlockCfg() { if (s_monMutex) xSemaphoreGive(s_monMutex); }

// --- persistence ----------------------------------------------------------

void monitorInit() {
    if (s_monMutex == nullptr) s_monMutex = xSemaphoreCreateMutex();
    String v;
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
        v = prefs.getString(MON_KEY, "");
        prefs.end();
    }
    lockCfg();
    if (v.length() > 0) {
        strncpy(s_json, v.c_str(), sizeof(s_json) - 1);
        s_json[sizeof(s_json) - 1] = '\0';
    }
    parseHeader();
    s_lastMs = 0;
    s_ran = false;
    unlockCfg();
}

void monitorSetFromArgs(const char* argsJson) {
    if (argsJson == nullptr) return;
    if (s_monMutex == nullptr) s_monMutex = xSemaphoreCreateMutex();
    static char forNvs[MON_JSON_MAX];  // core-0 only caller — single-threaded
    lockCfg();
    strncpy(s_json, argsJson, sizeof(s_json) - 1);
    s_json[sizeof(s_json) - 1] = '\0';
    parseHeader();
    s_lastMs = 0;     // let the new schedule take effect on the next idle tick
    s_ran = false;
    memcpy(forNvs, s_json, sizeof(forNvs));
    unlockCfg();
    // Flash write outside the lock — it is slow and needs no config coherence.
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        prefs.putString(MON_KEY, forNvs);
        prefs.end();
    }
}

// --- probes ---------------------------------------------------------------

// Non-blocking TCP connect classified as open/closed/filtered (same discipline
// as the port_scan runner). Returns "open"/"closed"/"filtered".
static const char* tcpState(const struct in_addr& addr, uint16_t port, int timeoutMs) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return "filtered";
    int nb = 1;
    ioctl(s, FIONBIO, &nb);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = addr;
    int rc = connect(s, (struct sockaddr*)&sa, sizeof(sa));
    if (rc == 0) { close(s); return "open"; }
    if (errno == ECONNREFUSED || errno == ECONNRESET) { close(s); return "closed"; }
    if (errno != EINPROGRESS && errno != EALREADY) { close(s); return "filtered"; }
    fd_set wfds, efds;
    FD_ZERO(&wfds); FD_ZERO(&efds);
    FD_SET(s, &wfds); FD_SET(s, &efds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int sel = select(s + 1, nullptr, &wfds, &efds, &tv);
    const char* state = "filtered";
    if (sel > 0) {
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr == 0) state = "open";
        else if (soerr == ECONNREFUSED || soerr == ECONNRESET) state = "closed";
    }
    close(s);
    return state;
}

#if NMS_HAS_ICMP
static uint16_t icmpChecksum(const uint8_t* d, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2) sum += ((uint32_t)d[i] << 8) | d[i + 1];
    if (n & 1) sum += (uint32_t)d[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}
// Returns rtt ms, or -1 on no reply.
static int icmpPing(const struct in_addr& addr, int timeoutMs) {
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) return -1;
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t pkt[16];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 8;
    uint16_t id = (uint16_t)(millis() & 0xffff);
    pkt[4] = id >> 8; pkt[5] = id & 0xff;
    uint16_t ck = icmpChecksum(pkt, sizeof(pkt));
    pkt[2] = ck >> 8; pkt[3] = ck & 0xff;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr = addr;
    uint32_t start = millis();
    if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr*)&sa, sizeof(sa)) < 0) { close(s); return -1; }
    uint8_t resp[128];
    int n = recv(s, resp, sizeof(resp), 0);
    close(s);
    if (n <= 0) return -1;
    size_t ihl = (size_t)(resp[0] & 0x0f) * 4;
    if ((size_t)n < ihl + 8 || resp[ihl] != 0) return -1;
    return (int)(millis() - start);
}
#endif  // NMS_HAS_ICMP

// A quick TCP-liveness fallback for the ping check when raw ICMP is unavailable.
static bool tcpAlive(const struct in_addr& addr, int timeoutMs) {
    static const uint16_t PORTS[] = {80, 443, 22, 7};
    for (uint16_t port : PORTS) {
        const char* st = tcpState(addr, port, timeoutMs);
        if (strcmp(st, "open") == 0 || strcmp(st, "closed") == 0) return true;
    }
    return false;
}

// --- cycle ----------------------------------------------------------------

static void runCycle(const char* json) {
    JsonDocument cfg;
    if (deserializeJson(cfg, json) != DeserializationError::Ok) return;
    JsonArrayConst devices = cfg["devices"].as<JsonArrayConst>();
    if (devices.isNull()) return;

    uint32_t nowTs = (uint32_t)time(nullptr);
    if (nowTs < 1000000000u) return;  // clock not synced — a cycle_ts would be invalid

    JsonDocument doc;
    doc["cycle_ts"] = nowTs;
    JsonArray results = doc["results"].to<JsonArray>();
    int timeoutMs = 800;

    for (JsonVariantConst dev : devices) {
        int id = dev["id"] | 0;
        const char* ip = dev["ip"] | (const char*)nullptr;
        if (id < 1 || ip == nullptr) continue;

        struct in_addr addr;
        JsonObject r = results.add<JsonObject>();
        r["id"] = id;
        if (inet_aton(ip, &addr) != 1) {
            r["status"] = "unknown";
            r["latency_ms"] = (const char*)nullptr;
            continue;
        }

        bool up = false, haveLatency = false;
        int latency = 0;
        JsonArrayConst checks = dev["checks"].as<JsonArrayConst>();
        bool wantPing = false, wantPort = false;
        for (JsonVariantConst c : checks) {
            const char* cs = c.as<const char*>();
            if (cs && strcmp(cs, "ping") == 0) wantPing = true;
            if (cs && strcmp(cs, "port") == 0) wantPort = true;
        }

        if (wantPing) {
#if NMS_HAS_ICMP
            int rtt = icmpPing(addr, timeoutMs);
            if (rtt >= 0) { up = true; haveLatency = true; latency = rtt; }
            else if (tcpAlive(addr, timeoutMs)) up = true;
#else
            if (tcpAlive(addr, timeoutMs)) up = true;   // no raw ICMP: liveness only
#endif
        }
        if (wantPort) {
            JsonArrayConst ports = dev["ports"].as<JsonArrayConst>();
            if (!ports.isNull() && ports.size() > 0) {
                JsonObject pmap = r["ports"].to<JsonObject>();
                for (JsonVariantConst pv : ports) {
                    int port = pv.as<int>();
                    if (port < 1 || port > 65535) continue;
                    const char* st = tcpState(addr, (uint16_t)port, timeoutMs);
                    char key[6];
                    snprintf(key, sizeof(key), "%d", port);
                    pmap[key] = st;
                    if (strcmp(st, "open") == 0 || strcmp(st, "closed") == 0) up = true;
                }
            }
        }

        r["status"] = up ? "up" : "down";
        if (up && haveLatency) r["latency_ms"] = latency;
        else r["latency_ms"] = (const char*)nullptr;  // null unless up with a measured rtt
    }

    static char out[PAYLOAD_MAX_BYTES];
    size_t len = serializeJson(doc, out, sizeof(out));
    if (len > 0 && len <= PAYLOAD_MAX_BYTES) mqttEnqueueMonitor(out);
    // (A large device list that would exceed the cap is left for per-cycle
    // chunking in a follow-up; typical fleets fit one cycle in 1024 bytes.)
}

void monitorTick() {
    if (!radioInStation()) return;  // §6.2: skip cycles due during a survey
    // Decide under the lock and copy the schedule out, so the cycle itself runs
    // on a private snapshot a concurrent set_monitor (core 0) cannot tear. Only
    // the worker task (core 1) calls monitorTick, so the static snapshot is
    // single-threaded once the lock is released.
    static char snap[MON_JSON_MAX];
    bool due = false;
    lockCfg();
    if (s_enabled && s_intervalS != 0) {
        uint32_t now = millis();
        if (!(s_ran && (now - s_lastMs) < s_intervalS * 1000u)) {
            s_lastMs = now;
            s_ran = true;
            memcpy(snap, s_json, sizeof(snap));
            due = true;
        }
    }
    unlockCfg();
    if (due) runCycle(snap);
}

#else  // native build: no NVS/sockets — keep config in RAM, cycles are no-ops.

void monitorInit() { parseHeader(); }
void monitorSetFromArgs(const char* argsJson) {
    if (argsJson) { strncpy(s_json, argsJson, sizeof(s_json) - 1); s_json[sizeof(s_json) - 1] = '\0'; }
    parseHeader();
}
void monitorTick() {}

#endif  // NMS_HAS_DEV
