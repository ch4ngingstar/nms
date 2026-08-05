#include "banner_grab.h"
#include "port_scan.h"  // parsePortSpec — the shared, host-tested port parser

#include <string.h>
#include <stdio.h>

#include <ArduinoJson.h>

#if defined(ESP_PLATFORM) || defined(ARDUINO)
#define NMS_HAS_LWIP 1
#include <errno.h>
#include <sys/ioctl.h>
#include <Arduino.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#else
#define NMS_HAS_LWIP 0
#endif

static const size_t MAX_PORTS = 64;    // banner grab targets a handful of services
static const size_t BANNER_MAX = 256;  // read cap per the spec/task

#if NMS_HAS_LWIP

static bool resolveHost(const char* host, struct in_addr* out) {
    if (inet_aton(host, out) == 1) return true;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr) return false;
    *out = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

// Non-blocking connect bounded by `timeoutMs`, then a blocking recv (SO_RCVTIMEO)
// of up to BANNER_MAX bytes. Returns the number of bytes read (0 on any failure).
// `out` is filled with sanitized printable ASCII, NUL-terminated.
static size_t grab(const struct in_addr& addr, uint16_t port, int timeoutMs,
                   char* out, size_t outSize) {
    out[0] = '\0';
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return 0;

    int nb = 1;
    if (ioctl(s, FIONBIO, &nb) < 0) { close(s); return 0; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = addr;

    int rc = connect(s, (struct sockaddr*)&sa, sizeof(sa));
    if (rc != 0) {
        if (errno != EINPROGRESS && errno != EALREADY) { close(s); return 0; }
        fd_set wfds, efds;
        FD_ZERO(&wfds); FD_ZERO(&efds);
        FD_SET(s, &wfds); FD_SET(s, &efds);
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        if (select(s + 1, nullptr, &wfds, &efds, &tv) <= 0) { close(s); return 0; }
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 || soerr != 0) {
            close(s);
            return 0;
        }
    }

    // Back to blocking with a receive deadline for the banner read.
    nb = 0;
    ioctl(s, FIONBIO, &nb);
    struct timeval rtv;
    rtv.tv_sec = timeoutMs / 1000;
    rtv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    uint8_t raw[BANNER_MAX];
    int n = recv(s, raw, sizeof(raw), 0);
    close(s);
    if (n <= 0) return 0;

    // Sanitize to printable ASCII so the JSON stays valid UTF-8 (raw bytes may
    // be anything); collapse control bytes, then trim surrounding whitespace.
    size_t w = 0;
    for (int i = 0; i < n && w + 1 < outSize; ++i) {
        uint8_t c = raw[i];
        out[w++] = (c >= 0x20 && c <= 0x7e) ? (char)c : ' ';
    }
    out[w] = '\0';
    // trim
    size_t start = 0;
    while (out[start] == ' ') ++start;
    size_t end = strlen(out + start);
    char* base = out + start;
    while (end > 0 && base[end - 1] == ' ') base[--end] = '\0';
    if (start > 0) memmove(out, base, end + 1);
    return strlen(out);
}

#endif  // NMS_HAS_LWIP

bool runBannerGrab(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
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
    if (targets.isNull() || targets.size() == 0) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "targets are required");
        return false;
    }
    const char* portsSpec = args["ports"] | "22,80";
    static uint16_t ports[MAX_PORTS];
    int nPorts = parsePortSpec(portsSpec, ports, MAX_PORTS);
    if (nPorts < 0) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "invalid ports spec");
        return false;
    }
    int timeoutMs = args["timeout_ms"] | 2000;
    if (timeoutMs < 1) timeoutMs = 1;
    if (timeoutMs > 60000) timeoutMs = 60000;

#if NMS_HAS_LWIP
    // Stream one chunk per (host) so a wide sweep stays under the 1024-byte cap.
    static char buf[900];
    static char banner[BANNER_MAX];
    uint32_t rows = 0;
    for (JsonVariantConst t : targets) {
        if (cancelled(sink)) break;
        const char* host = t.as<const char*>();
        if (host == nullptr) continue;
        struct in_addr addr;
        bool resolved = resolveHost(host, &addr);

        JsonDocument doc;
        JsonArray banners = doc["banners"].to<JsonArray>();
        for (int i = 0; i < nPorts; ++i) {
            JsonObject b = banners.add<JsonObject>();
            b["host"] = host;
            b["port"] = ports[i];
            if (resolved && grab(addr, ports[i], timeoutMs, banner, sizeof(banner)) > 0) {
                b["banner"] = banner;
            } else {
                b["banner"] = (const char*)nullptr;  // serializes as null
            }
            ++rows;
            if (measureJson(doc) > 760) {
                size_t len = serializeJson(doc, buf, sizeof(buf));
                if (len > 0) emit(sink, buf);
                doc.clear();
                banners = doc["banners"].to<JsonArray>();
            }
        }
        size_t len = serializeJson(doc, buf, sizeof(buf));
        if (len > 0 && banners.size() > 0) emit(sink, buf);
    }
    *resultsOut = rows;
    return true;
#else
    (void)emit; (void)cancelled; (void)sink;
    *resultsOut = 0;
    snprintf(errCode, errCodeSize, "unsupported");
    snprintf(errMsg, errMsgSize, "banner_grab requires lwIP (device build)");
    return false;
#endif
}
