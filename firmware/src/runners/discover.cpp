#include "discover.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>  // atoi

#include <ArduinoJson.h>

#if defined(ESP_PLATFORM) || defined(ARDUINO)
#define NMS_HAS_LWIP 1
#include <errno.h>
#include <sys/ioctl.h>
#include <Arduino.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
// Raw ICMP echo needs LWIP_RAW compiled into the prebuilt libs (spec §10.1).
#if (defined(CONFIG_LWIP_RAW) && CONFIG_LWIP_RAW) || (defined(LWIP_RAW) && LWIP_RAW)
#define NMS_HAS_ICMP 1
#else
#define NMS_HAS_ICMP 0
#endif
#else
#define NMS_HAS_LWIP 0
#define NMS_HAS_ICMP 0
#endif

static const uint32_t MAX_SWEEP_HOSTS = 512;  // bound a CIDR sweep (spec §7)
static const size_t TCP_PROBE_PORTS = 5;

#if NMS_HAS_LWIP

static uint16_t icmpChecksum(const uint8_t* d, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2) sum += ((uint32_t)d[i] << 8) | d[i + 1];
    if (n & 1) sum += (uint32_t)d[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

#if NMS_HAS_ICMP
// One ICMP echo with a receive timeout. Returns rtt in ms, or -1 on no reply.
static int icmpPing(const struct in_addr& addr, uint16_t id, uint16_t seq, int timeoutMs) {
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) return -1;
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t pkt[16];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 8;                       // echo request
    pkt[4] = id >> 8; pkt[5] = id & 0xff;
    pkt[6] = seq >> 8; pkt[7] = seq & 0xff;
    uint16_t ck = icmpChecksum(pkt, sizeof(pkt));
    pkt[2] = ck >> 8; pkt[3] = ck & 0xff;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr = addr;

    uint32_t start = millis();
    if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(s);
        return -1;
    }
    // Read replies until one matches our id or the timeout elapses.
    for (;;) {
        uint8_t resp[128];
        int n = recv(s, resp, sizeof(resp), 0);
        if (n <= 0) { close(s); return -1; }
        size_t ihl = (size_t)(resp[0] & 0x0f) * 4;         // IP header length
        if ((size_t)n < ihl + 8) continue;
        uint8_t type = resp[ihl];
        uint16_t rid = ((uint16_t)resp[ihl + 4] << 8) | resp[ihl + 5];
        if (type == 0 && rid == id) { close(s); return (int)(millis() - start); }
        if ((int32_t)(millis() - start) > timeoutMs) { close(s); return -1; }
    }
}
#endif  // NMS_HAS_ICMP

// TCP-liveness fallback: a completed OR refused connect both prove the host is
// up. Returns true if any probed port answered either way within the timeout.
static bool tcpAlive(const struct in_addr& addr, int timeoutMs) {
    static const uint16_t PORTS[TCP_PROBE_PORTS] = {80, 443, 22, 7, 53};
    for (size_t i = 0; i < TCP_PROBE_PORTS; ++i) {
        int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s < 0) continue;
        int nb = 1;
        ioctl(s, FIONBIO, &nb);
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(PORTS[i]);
        sa.sin_addr = addr;
        int rc = connect(s, (struct sockaddr*)&sa, sizeof(sa));
        if (rc == 0) { close(s); return true; }
        if (errno == ECONNREFUSED || errno == ECONNRESET) { close(s); return true; }
        if (errno != EINPROGRESS && errno != EALREADY) { close(s); continue; }
        fd_set wfds, efds;
        FD_ZERO(&wfds); FD_ZERO(&efds);
        FD_SET(s, &wfds); FD_SET(s, &efds);
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select(s + 1, nullptr, &wfds, &efds, &tv);
        if (sel > 0) {
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len);
            close(s);
            if (soerr == 0 || soerr == ECONNREFUSED || soerr == ECONNRESET) return true;
            continue;
        }
        close(s);
    }
    return false;
}

// Best-effort ARP-table MAC lookup (§10.1 ARP fallback). Returns true and fills
// `mac` ("aa:bb:...") if the host has an entry — a live L2 neighbour even when
// ICMP/TCP were filtered. NOTE: etharp_find_addr is the one call here most likely
// to need adjustment against the on-board lwIP headers; it is isolated so a tweak
// touches only this function.
static bool arpLookup(const struct in_addr& addr, char* mac, size_t macSize) {
    if (netif_default == nullptr) return false;
    ip4_addr_t ip;
    ip.addr = addr.s_addr;
    struct eth_addr* eth = nullptr;
    const ip4_addr_t* ipret = nullptr;
    if (etharp_find_addr(netif_default, &ip, &eth, &ipret) < 0 || eth == nullptr) {
        return false;
    }
    snprintf(mac, macSize, "%02x:%02x:%02x:%02x:%02x:%02x",
             eth->addr[0], eth->addr[1], eth->addr[2],
             eth->addr[3], eth->addr[4], eth->addr[5]);
    return true;
}

// Expands one target into host addresses (host-order uint32), appended to `out`
// up to `cap`. A "a.b.c.d/nn" target yields its usable hosts (network and
// broadcast excluded for nn<31); a bare IP yields itself. Returns count added.
static size_t expandTarget(const char* target, uint32_t* out, size_t have, size_t cap) {
    char buf[64];
    strncpy(buf, target, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* slash = strchr(buf, '/');
    struct in_addr base;
    if (slash != nullptr) *slash = '\0';
    if (inet_aton(buf, &base) != 1) return 0;
    uint32_t baseHost = ntohl(base.s_addr);

    if (slash == nullptr) {
        if (have < cap) out[have] = baseHost;
        return (have < cap) ? 1 : 0;
    }
    int prefix = atoi(slash + 1);
    if (prefix < 0 || prefix > 32) return 0;
    uint32_t mask = (prefix == 0) ? 0 : (0xFFFFFFFFu << (32 - prefix));
    uint32_t network = baseHost & mask;
    uint32_t broadcast = network | ~mask;
    size_t added = 0;
    for (uint32_t a = network; ; ++a) {
        if (have + added >= cap) break;
        bool edge = (prefix < 31) && (a == network || a == broadcast);
        if (!edge) out[have + added++] = a;
        if (a == broadcast) break;  // avoid uint overflow wrap at /0
    }
    return added;
}

#endif  // NMS_HAS_LWIP

bool runDiscover(const RunnerCtx& ctx, EmitChunkFn emit, IsCancelledFn cancelled,
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

#if NMS_HAS_LWIP
    static uint32_t hosts[MAX_SWEEP_HOSTS];
    size_t nHosts = 0;
    for (JsonVariantConst t : targets) {
        const char* target = t.as<const char*>();
        if (target == nullptr) continue;
        nHosts += expandTarget(target, hosts, nHosts, MAX_SWEEP_HOSTS);
        if (nHosts >= MAX_SWEEP_HOSTS) break;
    }

    static char buf[900];
    uint16_t id = (uint16_t)(millis() & 0xffff);
    uint32_t up = 0;

    JsonDocument doc;
    JsonArray arr = doc["hosts"].to<JsonArray>();
    for (size_t i = 0; i < nHosts; ++i) {
        if (cancelled(sink)) break;
        struct in_addr addr;
        addr.s_addr = htonl(hosts[i]);

        const char* method = nullptr;
        int rtt = -1;
#if NMS_HAS_ICMP
        rtt = icmpPing(addr, id, (uint16_t)i, timeoutMs);
        if (rtt >= 0) method = "icmp";
#endif
        if (method == nullptr && tcpAlive(addr, timeoutMs)) method = "tcp";

        char mac[18];
        bool haveMac = arpLookup(addr, mac, sizeof(mac));
        if (method == nullptr && haveMac) method = "arp";  // L2-present but L3-filtered
        if (method == nullptr) continue;                   // host not seen as up

        char ipstr[16];
        snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u",
                 (hosts[i] >> 24) & 0xff, (hosts[i] >> 16) & 0xff,
                 (hosts[i] >> 8) & 0xff, hosts[i] & 0xff);
        JsonObject h = arr.add<JsonObject>();
        h["ip"] = ipstr;
        if (haveMac) h["mac"] = mac;
        if (rtt >= 0) h["rtt_ms"] = rtt; else h["rtt_ms"] = (const char*)nullptr;
        h["method"] = method;
        ++up;

        if (measureJson(doc) > 760) {
            size_t len = serializeJson(doc, buf, sizeof(buf));
            if (len > 0) emit(sink, buf);
            doc.clear();
            arr = doc["hosts"].to<JsonArray>();
        }
    }
    if (arr.size() > 0) {
        size_t len = serializeJson(doc, buf, sizeof(buf));
        if (len > 0) emit(sink, buf);
    }
    *resultsOut = up;
    return true;
#else
    (void)emit; (void)cancelled; (void)sink;
    *resultsOut = 0;
    snprintf(errCode, errCodeSize, "unsupported");
    snprintf(errMsg, errMsgSize, "discover requires lwIP (device build)");
    return false;
#endif
}
