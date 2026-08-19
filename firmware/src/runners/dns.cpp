#include "dns.h"

#include <string.h>
#include <stdio.h>
#include <strings.h>  // strcasecmp

#include <ArduinoJson.h>

#if defined(ESP_PLATFORM) || defined(ARDUINO)
#define NMS_HAS_LWIP 1
#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#else
#define NMS_HAS_LWIP 0
#endif

// --- type name <-> code ---------------------------------------------------

uint16_t dnsTypeFromName(const char* name) {
    if (name == nullptr) return 0;
    if (strcasecmp(name, "A") == 0) return DNS_A;
    if (strcasecmp(name, "NS") == 0) return DNS_NS;
    if (strcasecmp(name, "CNAME") == 0) return DNS_CNAME;
    if (strcasecmp(name, "PTR") == 0) return DNS_PTR;
    if (strcasecmp(name, "MX") == 0) return DNS_MX;
    if (strcasecmp(name, "TXT") == 0) return DNS_TXT;
    if (strcasecmp(name, "AAAA") == 0) return DNS_AAAA;
    return 0;
}

const char* dnsTypeName(uint16_t type) {
    switch (type) {
        case DNS_A: return "A";
        case DNS_NS: return "NS";
        case DNS_CNAME: return "CNAME";
        case DNS_PTR: return "PTR";
        case DNS_MX: return "MX";
        case DNS_TXT: return "TXT";
        case DNS_AAAA: return "AAAA";
        default: return "UNKNOWN";
    }
}

// --- pure builder ---------------------------------------------------------

size_t dnsBuildQuery(const char* name, uint16_t qtype, uint16_t id,
                     uint8_t* out, size_t outSize) {
    if (name == nullptr || outSize < 12) return 0;
    size_t p = 0;
    out[p++] = (uint8_t)(id >> 8); out[p++] = (uint8_t)(id & 0xff);
    out[p++] = 0x01; out[p++] = 0x00;   // flags: RD=1
    out[p++] = 0x00; out[p++] = 0x01;   // QDCOUNT=1
    out[p++] = 0x00; out[p++] = 0x00;   // ANCOUNT
    out[p++] = 0x00; out[p++] = 0x00;   // NSCOUNT
    out[p++] = 0x00; out[p++] = 0x00;   // ARCOUNT

    const char* s = name;
    while (*s) {
        const char* dot = strchr(s, '.');
        size_t llen = dot ? (size_t)(dot - s) : strlen(s);
        if (llen == 0 || llen > 63) return 0;      // empty / oversized label
        if (p + 1 + llen > outSize) return 0;
        out[p++] = (uint8_t)llen;
        memcpy(out + p, s, llen); p += llen;
        if (!dot) break;
        s = dot + 1;
    }
    if (p + 5 > outSize) return 0;
    out[p++] = 0x00;                    // root label
    out[p++] = (uint8_t)(qtype >> 8); out[p++] = (uint8_t)(qtype & 0xff);
    out[p++] = 0x00; out[p++] = 0x01;   // QCLASS IN
    return p;
}

// --- pure parser ----------------------------------------------------------

// Decodes a (possibly compression-pointed) name starting at `off`. Writes a
// dotted string to `out`; returns the stream offset just past the name in the
// record (the byte after the first pointer, or after the terminating 0), or -1.
static int decodeName(const uint8_t* msg, size_t len, size_t off,
                      char* out, size_t outSize) {
    size_t w = 0;
    size_t pos = off;
    int nextOff = -1;
    int hops = 0;
    bool first = true;
    for (;;) {
        if (pos >= len) return -1;
        uint8_t b = msg[pos];
        if ((b & 0xC0) == 0xC0) {                  // compression pointer
            if (pos + 1 >= len) return -1;
            if (nextOff < 0) nextOff = (int)(pos + 2);
            size_t ptr = ((size_t)(b & 0x3F) << 8) | msg[pos + 1];
            if (ptr >= len) return -1;
            pos = ptr;
            if (++hops > 128) return -1;           // malformed pointer loop
            continue;
        }
        if ((b & 0xC0) != 0) return -1;            // reserved label type
        if (b == 0) { pos += 1; if (nextOff < 0) nextOff = (int)pos; break; }
        pos += 1;
        if (pos + b > len) return -1;
        if (!first && w + 1 < outSize) out[w++] = '.';
        first = false;
        for (uint8_t i = 0; i < b && w + 1 < outSize; ++i) out[w++] = (char)msg[pos + i];
        pos += b;
    }
    out[w] = '\0';
    return nextOff;
}

// Renders one RDATA field to printable text by record type.
static void renderRdata(const uint8_t* msg, size_t len, size_t pos, uint16_t rdlen,
                        uint16_t type, char* out, size_t outSize) {
    out[0] = '\0';
    switch (type) {
        case DNS_A:
            if (rdlen >= 4) {
                snprintf(out, outSize, "%u.%u.%u.%u", msg[pos], msg[pos + 1],
                         msg[pos + 2], msg[pos + 3]);
            }
            break;
        case DNS_AAAA:
            if (rdlen >= 16) {
                size_t w = 0;
                for (int g = 0; g < 8; ++g) {
                    uint16_t grp = ((uint16_t)msg[pos + g * 2] << 8) | msg[pos + g * 2 + 1];
                    w += snprintf(out + w, (w < outSize) ? outSize - w : 0,
                                  (g == 0) ? "%x" : ":%x", grp);
                }
            }
            break;
        case DNS_CNAME:
        case DNS_NS:
        case DNS_PTR:
            decodeName(msg, len, pos, out, outSize);
            break;
        case DNS_MX: {
            if (rdlen >= 3) {
                uint16_t pref = ((uint16_t)msg[pos] << 8) | msg[pos + 1];
                char host[128];
                decodeName(msg, len, pos + 2, host, sizeof(host));
                snprintf(out, outSize, "%u %s", pref, host);
            }
            break;
        }
        case DNS_TXT: {
            // One or more <len><bytes> character-strings, concatenated.
            size_t w = 0, i = 0;
            while (i < rdlen && w + 1 < outSize) {
                uint8_t slen = msg[pos + i++];
                for (uint8_t k = 0; k < slen && i < rdlen && w + 1 < outSize; ++k, ++i) {
                    uint8_t c = msg[pos + i];
                    out[w++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
                }
            }
            out[w] = '\0';
            break;
        }
        default:
            break;  // unknown type -> empty value
    }
}

int dnsParseResponse(const uint8_t* msg, size_t len, DnsAnswer* out, size_t maxAnswers) {
    if (msg == nullptr || len < 12) return -1;
    uint16_t qd = ((uint16_t)msg[4] << 8) | msg[5];
    uint16_t an = ((uint16_t)msg[6] << 8) | msg[7];

    size_t pos = 12;
    char scratch[128];
    for (uint16_t i = 0; i < qd; ++i) {                 // skip questions
        int no = decodeName(msg, len, pos, scratch, sizeof(scratch));
        if (no < 0) return -1;
        pos = (size_t)no + 4;                           // QTYPE + QCLASS
        if (pos > len) return -1;
    }

    size_t count = 0;
    for (uint16_t i = 0; i < an && count < maxAnswers; ++i) {
        int no = decodeName(msg, len, pos, out[count].name, sizeof(out[count].name));
        if (no < 0) return -1;
        pos = (size_t)no;
        if (pos + 10 > len) return -1;
        uint16_t type = ((uint16_t)msg[pos] << 8) | msg[pos + 1];
        uint32_t ttl = ((uint32_t)msg[pos + 4] << 24) | ((uint32_t)msg[pos + 5] << 16) |
                       ((uint32_t)msg[pos + 6] << 8) | msg[pos + 7];
        uint16_t rdlen = ((uint16_t)msg[pos + 8] << 8) | msg[pos + 9];
        pos += 10;
        if (pos + rdlen > len) return -1;

        out[count].type = type;
        out[count].ttl = ttl;
        renderRdata(msg, len, pos, rdlen, type, out[count].value, sizeof(out[count].value));
        pos += rdlen;
        ++count;
    }
    return (int)count;
}

// --- device runner --------------------------------------------------------

bool runDns(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
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
    uint16_t qtype = dnsTypeFromName(args["type"] | "A");
    if (qtype == 0) {
        snprintf(errCode, errCodeSize, "bad_args");
        snprintf(errMsg, errMsgSize, "unsupported record type");
        return false;
    }
    int timeoutMs = args["timeout_ms"] | 3000;
    if (timeoutMs < 1) timeoutMs = 1;
    if (timeoutMs > 60000) timeoutMs = 60000;

#if NMS_HAS_LWIP
    // Resolver: explicit arg, else the DHCP-assigned server, else a public one.
    IPAddress server;
    const char* srvArg = args["server"] | (const char*)nullptr;
    if (srvArg != nullptr) {
        server.fromString(srvArg);
    } else {
        server = WiFi.dnsIP();
        if ((uint32_t)server == 0) server.fromString("8.8.8.8");
    }

    static char buf[900];
    static DnsAnswer answers[16];
    uint32_t rows = 0;

    for (JsonVariantConst t : targets) {
        if (cancelled(sink)) break;
        const char* name = t.as<const char*>();
        if (name == nullptr) continue;

        uint8_t query[300];
        uint16_t id = (uint16_t)esp_random();
        size_t qlen = dnsBuildQuery(name, qtype, id, query, sizeof(query));
        if (qlen == 0) continue;

        int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s < 0) continue;
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(53);
        sa.sin_addr.s_addr = (uint32_t)server;

        int nAns = -1;
        if (sendto(s, query, qlen, 0, (struct sockaddr*)&sa, sizeof(sa)) == (int)qlen) {
            uint8_t resp[512];
            int n = recv(s, resp, sizeof(resp), 0);
            if (n > 0) nAns = dnsParseResponse(resp, (size_t)n, answers, 16);
        }
        close(s);

        // Emit a chunk for this name (empty answers array when nothing resolved),
        // splitting if a name returns many records.
        JsonDocument doc;
        JsonArray arr = doc["answers"].to<JsonArray>();
        for (int i = 0; i < nAns; ++i) {
            JsonObject a = arr.add<JsonObject>();
            a["name"] = answers[i].name;
            a["type"] = dnsTypeName(answers[i].type);
            a["ttl"] = answers[i].ttl;
            a["value"] = answers[i].value;
            ++rows;
            if (measureJson(doc) > 760) {
                size_t len = serializeJson(doc, buf, sizeof(buf));
                if (len > 0) emit(sink, buf);
                doc.clear();
                arr = doc["answers"].to<JsonArray>();
            }
        }
        size_t len = serializeJson(doc, buf, sizeof(buf));
        if (len > 0) emit(sink, buf);
    }
    *resultsOut = rows;
    return true;
#else
    (void)emit; (void)cancelled; (void)sink;
    *resultsOut = 0;
    snprintf(errCode, errCodeSize, "unsupported");
    snprintf(errMsg, errMsgSize, "dns runner requires lwIP (device build)");
    return false;
#endif
}
