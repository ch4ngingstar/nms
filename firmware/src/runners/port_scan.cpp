#include "port_scan.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <ArduinoJson.h>

// lwIP sockets exist only in the device build; the pure port-spec parser below
// is compiled everywhere so the native test env can exercise it (spec §9.1).
#if defined(ESP_PLATFORM) || defined(ARDUINO)
#define NMS_HAS_LWIP 1
#include <errno.h>
#include <sys/ioctl.h>
#include <Arduino.h>            // millis()
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#else
#define NMS_HAS_LWIP 0
#endif

// Cap on ports materialized from one spec — bounds heap use if an operator sends
// a huge range. A spec expanding past this fills what fits (spec §7 memory rules).
static const size_t MAX_PORTS = 1024;

// --- pure port-spec parser (host-testable) --------------------------------

static void trim(const char*& s, size_t& len) {
    while (len > 0 && (*s == ' ' || *s == '\t')) { ++s; --len; }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) { --len; }
}

// Parses a whitespace-trimmed decimal in [1,65535]; -1 on non-decimal or range.
static int parseU16(const char* s, size_t len) {
    trim(s, len);
    if (len == 0) return -1;
    uint32_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (uint32_t)(s[i] - '0');
        if (v > 65535) return -1;
    }
    if (v < 1) return -1;
    return (int)v;
}

static int cmpU16(const void* a, const void* b) {
    uint16_t x = *(const uint16_t*)a, y = *(const uint16_t*)b;
    return (x > y) - (x < y);
}

int parsePortSpec(const char* spec, uint16_t* out, size_t maxPorts) {
    if (spec == nullptr) return -1;
    size_t count = 0;
    const char* p = spec;
    for (;;) {
        const char* comma = strchr(p, ',');
        const char* el = p;
        size_t elen = comma ? (size_t)(comma - p) : strlen(p);
        trim(el, elen);
        if (elen == 0) return -1;  // empty element (protocol/ports.py rejects)

        const char* dash = (const char*)memchr(el, '-', elen);
        if (dash != nullptr) {
            int low = parseU16(el, (size_t)(dash - el));
            int high = parseU16(dash + 1, elen - (size_t)(dash - el) - 1);
            if (low < 0 || high < 0) return -1;
            if (low > high) return -1;  // inverted range -> bad_args
            for (int v = low; v <= high && count < maxPorts; ++v) {
                out[count++] = (uint16_t)v;
            }
        } else {
            int v = parseU16(el, elen);
            if (v < 0) return -1;
            if (count < maxPorts) out[count++] = (uint16_t)v;
        }

        if (comma == nullptr) break;
        p = comma + 1;
    }

    // Ascending + de-duplicated, matching parse_ports()'s sorted(set(...)).
    qsort(out, count, sizeof(uint16_t), cmpU16);
    size_t uniq = 0;
    for (size_t i = 0; i < count; ++i) {
        if (uniq == 0 || out[i] != out[uniq - 1]) out[uniq++] = out[i];
    }
    return (int)uniq;
}

// --- connect scan (device only) -------------------------------------------

#if NMS_HAS_LWIP

enum PortState { PS_OPEN, PS_CLOSED, PS_FILTERED, PS_OTHER };

static bool resolveHost(const char* host, struct in_addr* out) {
    if (inet_aton(host, out) == 1) return true;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr) {
        return false;
    }
    *out = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

// One in-flight non-blocking connect within a wave.
struct Pending {
    int sock;
    uint16_t port;
    bool done;
};

// Classifies a completed socket from SO_ERROR (mirrors NmapService.cpp's
// tcp_connect_with_timeout final switch).
static PortState classifySoErr(int soerr) {
    switch (soerr) {
        case 0: return PS_OPEN;
        case ECONNREFUSED:
        case ECONNRESET: return PS_CLOSED;
        case ETIMEDOUT:
        case EHOSTUNREACH:
        case ENETUNREACH:
        case EACCES:
        case EPERM: return PS_FILTERED;
        default: return PS_OTHER;
    }
}

// Emits accumulated open ports for one host as {"open":[...]} chunks, splitting
// so no chunk approaches the 1024-byte cap (spec §7.2). Returns rows emitted.
static uint32_t flushOpen(const char* host, const uint16_t* ports,
                          const uint32_t* rtts, size_t n,
                          EmitChunkFn emit, void* sink) {
    static char buf[900];
    uint32_t rows = 0;
    if (n == 0) {
        // One chunk per target even with nothing open, mirroring
        // probe/jobs.py:port_scan ("yield {'open': open_ports}" per host).
        emit(sink, "{\"open\":[]}");
        return 0;
    }
    size_t i = 0;
    while (i < n) {
        JsonDocument doc;
        JsonArray open = doc["open"].to<JsonArray>();
        for (; i < n; ++i) {
            JsonObject o = open.add<JsonObject>();
            o["host"] = host;
            o["port"] = ports[i];
            o["state"] = "open";
            o["rtt_ms"] = rtts[i];
            ++rows;
            // Keep a chunk well under the cap; the worker adds ~90 bytes of
            // envelope + event/job_id/seq around this payload.
            if (measureJson(doc) > 760) { ++i; break; }
        }
        size_t len = serializeJson(doc, buf, sizeof(buf));
        if (len > 0) emit(sink, buf);
    }
    return rows;
}

// Scans one host's ports in waves of <= concurrency non-blocking connects.
static uint32_t scanHost(const struct in_addr& addr, const char* host,
                         const uint16_t* ports, size_t nPorts,
                         int timeoutMs, int concurrency,
                         EmitChunkFn emit, IsCancelledFn cancelled, void* sink) {
    static uint16_t openPorts[MAX_PORTS];
    static uint32_t openRtts[MAX_PORTS];
    size_t openN = 0;

    size_t idx = 0;
    while (idx < nPorts) {
        if (cancelled(sink)) break;

        Pending wave[8];
        size_t waveN = 0;
        uint32_t startMs = millis();

        // Kick off up to `concurrency` connects.
        while (waveN < (size_t)concurrency && idx < nPorts) {
            uint16_t port = ports[idx++];
            int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s < 0) continue;  // descriptor exhaustion — skip, spec §7
            int nb = 1;
            if (ioctl(s, FIONBIO, &nb) < 0) { close(s); continue; }

            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(port);
            sa.sin_addr = addr;

            int rc = connect(s, (struct sockaddr*)&sa, sizeof(sa));
            if (rc == 0) {  // immediate connect (loopback / very close host)
                if (openN < MAX_PORTS) { openPorts[openN] = port; openRtts[openN] = 0; ++openN; }
                close(s);
                continue;
            }
            if (errno == EINPROGRESS || errno == EALREADY) {
                wave[waveN].sock = s;
                wave[waveN].port = port;
                wave[waveN].done = false;
                ++waveN;
            } else {
                // Immediate refusal/unreachable — closed/filtered, not reported.
                close(s);
            }
        }

        // Drive the wave to completion or a shared deadline.
        uint32_t deadline = startMs + (uint32_t)timeoutMs;
        size_t remaining = waveN;
        while (remaining > 0) {
            uint32_t now = millis();
            if ((int32_t)(deadline - now) <= 0) break;

            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            int maxfd = -1;
            for (size_t k = 0; k < waveN; ++k) {
                if (wave[k].done) continue;
                FD_SET(wave[k].sock, &wfds);
                FD_SET(wave[k].sock, &efds);
                if (wave[k].sock > maxfd) maxfd = wave[k].sock;
            }
            if (maxfd < 0) break;

            uint32_t waitMs = deadline - now;
            struct timeval tv;
            tv.tv_sec = waitMs / 1000;
            tv.tv_usec = (waitMs % 1000) * 1000;
            int rc = select(maxfd + 1, nullptr, &wfds, &efds, &tv);
            if (rc <= 0) break;  // timeout (0) or error (<0): survivors -> filtered

            for (size_t k = 0; k < waveN; ++k) {
                if (wave[k].done) continue;
                if (!FD_ISSET(wave[k].sock, &wfds) && !FD_ISSET(wave[k].sock, &efds)) {
                    continue;
                }
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                if (getsockopt(wave[k].sock, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 &&
                    classifySoErr(soerr) == PS_OPEN) {
                    if (openN < MAX_PORTS) {
                        openPorts[openN] = wave[k].port;
                        openRtts[openN] = millis() - startMs;
                        ++openN;
                    }
                }
                close(wave[k].sock);
                wave[k].done = true;
                --remaining;
            }
        }
        // Anything still pending never completed the handshake -> filtered.
        for (size_t k = 0; k < waveN; ++k) {
            if (!wave[k].done) close(wave[k].sock);
        }
    }

    return flushOpen(host, openPorts, openRtts, openN, emit, sink);
}

#endif  // NMS_HAS_LWIP

bool runPortScan(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
                 void* sink, uint32_t* resultsOut,
                 char* errCode, size_t errCodeSize,
                 char* errMsg, size_t errMsgSize) {
    JsonDocument args;
    if (deserializeJson(args, ctx.args) != DeserializationError::Ok) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "args are not valid JSON");
        return false;
    }

    JsonArrayConst targets = args["targets"].as<JsonArrayConst>();
    const char* portsSpec = args["ports"] | (const char*)nullptr;
    if (targets.isNull() || targets.size() == 0 || portsSpec == nullptr) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "targets and ports are required");
        return false;
    }

    static uint16_t ports[MAX_PORTS];
    int nPorts = parsePortSpec(portsSpec, ports, MAX_PORTS);
    if (nPorts < 0) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "invalid ports spec");
        return false;
    }

    int timeoutMs = args["timeout_ms"] | 1000;
    if (timeoutMs < 1) timeoutMs = 1;
    if (timeoutMs > 60000) timeoutMs = 60000;
    int concurrency = args["concurrency"] | 8;
    if (concurrency < 1) concurrency = 1;
    if (concurrency > 8) concurrency = 8;  // clamp to lwIP budget (spec §7), do not error

#if NMS_HAS_LWIP
    uint32_t rows = 0;
    for (JsonVariantConst t : targets) {
        if (cancelled(sink)) break;
        const char* host = t.as<const char*>();
        if (host == nullptr) continue;
        struct in_addr addr;
        if (!resolveHost(host, &addr)) {
            continue;  // unresolvable target contributes no open ports
        }
        rows += scanHost(addr, host, ports, (size_t)nPorts, timeoutMs, concurrency,
                         emit, cancelled, sink);
    }
    *resultsOut = rows;
    return true;
#else
    (void)emit; (void)cancelled; (void)sink; (void)ctx;
    *resultsOut = 0;
    snprintf(errCode, errCodeSize, "unsupported");
    snprintf(errMsg, errMsgSize, "port_scan requires lwIP (device build)");
    return false;
#endif
}
