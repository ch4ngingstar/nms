#include "trace.h"

#include <string.h>
#include <stdio.h>

#include <ArduinoJson.h>

#if (defined(ESP_PLATFORM) || defined(ARDUINO)) && NMS_TRACE_AVAILABLE
#define NMS_TRACE_BUILD 1
#include <Arduino.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#else
#define NMS_TRACE_BUILD 0
#endif

#if NMS_TRACE_BUILD

static const int MAX_HOPS_CAP = 30;

static uint16_t icmpChecksum(const uint8_t* d, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2) sum += ((uint32_t)d[i] << 8) | d[i + 1];
    if (n & 1) sum += (uint32_t)d[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static bool resolveHost(const char* host, struct in_addr* out) {
    if (inet_aton(host, out) == 1) return true;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_RAW;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr) return false;
    *out = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

#endif  // NMS_TRACE_BUILD

bool runTrace(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
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
    int timeoutMs = args["timeout_ms"] | 1000;
    if (timeoutMs < 1) timeoutMs = 1;
    if (timeoutMs > 60000) timeoutMs = 60000;

#if NMS_TRACE_BUILD
    int maxHops = args["max_hops"] | MAX_HOPS_CAP;
    if (maxHops < 1) maxHops = 1;
    if (maxHops > MAX_HOPS_CAP) maxHops = MAX_HOPS_CAP;

    static char buf[900];
    uint16_t id = (uint16_t)(millis() & 0xffff);
    uint32_t rows = 0;

    for (JsonVariantConst t : targets) {
        if (cancelled(sink)) break;
        const char* host = t.as<const char*>();
        if (host == nullptr) continue;
        struct in_addr dst;
        if (!resolveHost(host, &dst)) continue;

        JsonDocument doc;
        JsonArray hops = doc["hops"].to<JsonArray>();

        for (int ttl = 1; ttl <= maxHops; ++ttl) {
            if (cancelled(sink)) break;
            int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
            if (s < 0) {
                snprintf(errCode, errCodeSize, "unsupported");
                snprintf(errMsg, errMsgSize, "raw ICMP socket unavailable");
                return false;  // no raw sockets -> trace cannot run
            }
            setsockopt(s, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
            struct timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            uint8_t pkt[16];
            memset(pkt, 0, sizeof(pkt));
            pkt[0] = 8;                                    // echo request
            pkt[4] = id >> 8; pkt[5] = id & 0xff;
            pkt[6] = ttl >> 8; pkt[7] = ttl & 0xff;        // seq = ttl
            uint16_t ck = icmpChecksum(pkt, sizeof(pkt));
            pkt[2] = ck >> 8; pkt[3] = ck & 0xff;

            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_addr = dst;

            uint32_t start = millis();
            JsonObject hop = hops.add<JsonObject>();
            hop["ttl"] = ttl;

            bool reached = false;
            if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
                hop["addr"] = (const char*)nullptr;
                hop["rtt_ms"] = (const char*)nullptr;
                hop["timeout"] = true;
                close(s);
                ++rows;
                continue;
            }

            struct sockaddr_in from;
            socklen_t fromLen = sizeof(from);
            uint8_t resp[128];
            int n = recvfrom(s, resp, sizeof(resp), 0, (struct sockaddr*)&from, &fromLen);
            close(s);
            ++rows;

            if (n <= 0) {
                hop["addr"] = (const char*)nullptr;
                hop["rtt_ms"] = (const char*)nullptr;
                hop["timeout"] = true;
                continue;
            }
            char addrstr[16];
            const char* p = inet_ntoa(from.sin_addr);
            strncpy(addrstr, p ? p : "0.0.0.0", sizeof(addrstr) - 1);
            addrstr[sizeof(addrstr) - 1] = '\0';
            hop["addr"] = addrstr;
            hop["rtt_ms"] = (int)(millis() - start);
            hop["timeout"] = false;

            size_t ihl = (size_t)(resp[0] & 0x0f) * 4;
            uint8_t type = ((size_t)n > ihl) ? resp[ihl] : 0xff;
            if (type == 0) reached = true;                 // echo reply from target
            // type 11 (time exceeded) -> intermediate hop; keep going.

            if (measureJson(doc) > 760) {
                size_t len = serializeJson(doc, buf, sizeof(buf));
                if (len > 0) emit(sink, buf);
                doc.clear();
                hops = doc["hops"].to<JsonArray>();
            }
            if (reached) break;
        }

        if (hops.size() > 0) {
            size_t len = serializeJson(doc, buf, sizeof(buf));
            if (len > 0) emit(sink, buf);
        }
    }
    *resultsOut = rows;
    return true;
#else
    (void)emit; (void)cancelled; (void)sink; (void)targets;
    *resultsOut = 0;
    snprintf(errCode, errCodeSize, "unsupported");
    snprintf(errMsg, errMsgSize, "trace not built (raw ICMP unavailable)");
    return false;
#endif
}
