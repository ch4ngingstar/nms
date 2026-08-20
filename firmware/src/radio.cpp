#include "radio.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "mqtt_client.h"

// --- module state ---------------------------------------------------------

static ProbeConfig s_cfg;
static volatile RadioState s_state = RADIO_STATION;
static volatile bool s_lockHeld = false;

// Dwell per channel while sniffing clients (Bit-Pirate handleSniff uses a
// comparable hop cadence).
static const uint32_t DWELL_MS = 220;

// The promiscuous RX callback runs in WiFi-driver context, so the client buffer
// it writes is guarded by a spinlock and reached through these file statics
// (a C callback cannot carry the SurveyBuffers pointer).
static portMUX_TYPE s_cliMux = portMUX_INITIALIZER_UNLOCKED;
static ClientRecord* s_cli = nullptr;
static size_t s_cliCap = 0;
static volatile size_t s_cliCount = 0;
static volatile uint32_t s_cliDropped = 0;

// --- lock -----------------------------------------------------------------

static bool radioAcquire() {
    if (s_lockHeld) return false;
    s_lockHeld = true;
    return true;
}
static void radioRelease() { s_lockHeld = false; }

bool radioInStation() { return s_state == RADIO_STATION && !s_lockHeld; }

const char* radioAuthName(uint8_t authMode) {
    switch ((wifi_auth_mode_t)authMode) {
        case WIFI_AUTH_OPEN: return "open";
        case WIFI_AUTH_WEP: return "wep";
        case WIFI_AUTH_WPA_PSK: return "wpa";
        case WIFI_AUTH_WPA2_PSK: return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
        case WIFI_AUTH_WPA3_PSK: return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
        default: return "unknown";
    }
}

void radioInit(const ProbeConfig& cfg) { s_cfg = cfg; }

// --- AP scan -----------------------------------------------------------
//
// esp_wifi_scan_start is used rather than WiFi.scanNetworks() so no Arduino
// String is created in the job path. wifi_ap_record_t already carries
// decoded authmode, avoiding raw IE parsing.
static void radioApScan(SurveyBuffers& buf, bool passive) {
    wifi_scan_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.show_hidden = true;
    cfg.scan_type = passive ? WIFI_SCAN_TYPE_PASSIVE : WIFI_SCAN_TYPE_ACTIVE;
    if (passive) {
        cfg.scan_time.passive = 120;  // ms/channel
    } else {
        cfg.scan_time.active.min = 60;
        cfg.scan_time.active.max = 120;
    }
    // channel 0 = all channels; the requested `channels` drive the client hop.
    if (esp_wifi_scan_start(&cfg, /*block=*/true) != ESP_OK) return;

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) return;
    if (num > buf.apCap) { buf.apDropped += (num - buf.apCap); num = buf.apCap; }

    wifi_ap_record_t* recs = (wifi_ap_record_t*)malloc(num * sizeof(wifi_ap_record_t));
    if (recs == nullptr) { esp_wifi_scan_get_ap_records(&num, nullptr); return; }
    uint16_t got = num;
    if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
        for (uint16_t i = 0; i < got && buf.apCount < buf.apCap; ++i) {
            ApRecord& a = buf.aps[buf.apCount++];
            memcpy(a.bssid, recs[i].bssid, 6);
            strncpy(a.ssid, (const char*)recs[i].ssid, sizeof(a.ssid) - 1);
            a.ssid[sizeof(a.ssid) - 1] = '\0';
            a.channel = recs[i].primary;
            a.rssi = recs[i].rssi;
            a.auth = (uint8_t)recs[i].authmode;
            a.hidden = (recs[i].ssid[0] == '\0');
        }
    }
    free(recs);
}

// --- client sniff --------------------------------------------------------

// Dedup-inserts a station under the spinlock, keeping the strongest RSSI. Runs in
// the RX callback context, so it must be short and lock-brief.
static void insertClient(const uint8_t* mac, const uint8_t* bssid, int8_t rssi) {
    portENTER_CRITICAL_ISR(&s_cliMux);
    for (size_t i = 0; i < s_cliCount; ++i) {
        if (memcmp(s_cli[i].mac, mac, 6) == 0) {
            if (rssi > s_cli[i].rssi) {
                s_cli[i].rssi = rssi;
                memcpy(s_cli[i].bssid, bssid, 6);
            }
            portEXIT_CRITICAL_ISR(&s_cliMux);
            return;
        }
    }
    if (s_cliCount < s_cliCap) {
        memcpy(s_cli[s_cliCount].mac, mac, 6);
        memcpy(s_cli[s_cliCount].bssid, bssid, 6);
        s_cli[s_cliCount].rssi = rssi;
        ++s_cliCount;
    } else {
        ++s_cliDropped;  // counted as a drop rather than grown — never exhaust the heap
    }
    portEXIT_CRITICAL_ISR(&s_cliMux);
}

// Extracts (station, BSSID, RSSI) from a data frame's address fields, keyed off
// the To-DS/From-DS bits (Bit-Pirate clientSnifferCallback pattern).
static void promiscCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
    if (p->rx_ctrl.sig_len < 24) return;  // shorter than an 802.11 MAC header
    const uint8_t* h = p->payload;
    int8_t rssi = p->rx_ctrl.rssi;

    uint8_t ds = h[1] & 0x03;
    const uint8_t* addr1 = h + 4;
    const uint8_t* addr2 = h + 10;
    uint8_t sta[6], bssid[6];
    if (ds == 0x01) {          // To-DS: addr1 = BSSID, addr2 = STA (source)
        memcpy(bssid, addr1, 6);
        memcpy(sta, addr2, 6);
    } else if (ds == 0x02) {   // From-DS: addr1 = STA (dest), addr2 = BSSID
        memcpy(bssid, addr2, 6);
        memcpy(sta, addr1, 6);
    } else {
        return;                // ad-hoc / WDS — no single AP/STA pairing
    }
    if (sta[0] & 0x01) return; // multicast/broadcast station address — not a client
    insertClient(sta, bssid, rssi);
}

static void sniffClients(SurveyBuffers& buf, const uint8_t* channels,
                         size_t nChannels, uint32_t durationS) {
    portENTER_CRITICAL(&s_cliMux);
    s_cli = buf.clients;
    s_cliCap = buf.clientCap;
    s_cliCount = 0;
    s_cliDropped = 0;
    portEXIT_CRITICAL(&s_cliMux);

    esp_wifi_set_promiscuous_rx_cb(&promiscCb);
    esp_wifi_set_promiscuous(true);
    s_state = RADIO_PROMISCUOUS;

    uint32_t endMs = millis() + durationS * 1000u;
    size_t hop = 0;
    while ((int32_t)(endMs - millis()) > 0) {
        uint8_t ch = channels[hop % nChannels];
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        delay(DWELL_MS);
        ++hop;
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    s_state = RADIO_STATION;

    portENTER_CRITICAL(&s_cliMux);
    buf.clientCount = s_cliCount;
    buf.clientDropped += s_cliDropped;
    s_cli = nullptr;
    s_cliCap = 0;
    portEXIT_CRITICAL(&s_cliMux);
}

// --- the survey sequence ---------------------------------------------------

bool radioRunSurvey(const char* jobId, uint16_t durationS,
                    const uint8_t* channels, size_t nChannels, bool passive,
                    SurveyBuffers& buf,
                    char* errCode, size_t ecs, char* errMsg, size_t ems) {
    if (!radioAcquire()) {
        snprintf(errCode, ecs, "radio_conflict");
        snprintf(errMsg, ems, "radio already in use");
        return false;
    }

    // (a) accepted while still connected — lets the server tell a survey-in-
    // progress apart from a node that never received the command.
    mqttPublishAccepted(jobId);
    // (b) retained surveying status with the return estimate.
    mqttPublishSurveying((uint32_t)durationS + 12u);

    // (c) hand PubSubClient to us: pause the MQTT task, wait for it to step
    // aside, then send a CLEAN DISCONNECT so the Last Will does NOT fire.
    mqttSurveyPauseBegin();
    uint32_t t0 = millis();
    while (!mqttSurveyPausedAck() && (millis() - t0) < 1500) delay(10);
    mqttDisconnectClean();

    // (d) leave the AP (still WIFI_STA, just unassociated).
    WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
    delay(50);

    // (e) AP scan, then client sniff for the remaining window.
    uint32_t startMs = millis();
    radioApScan(buf, passive);
    uint32_t elapsedS = (millis() - startMs) / 1000u;
    uint32_t remainingS = (durationS > elapsedS) ? (durationS - elapsedS) : 1u;
    sniffClients(buf, channels, nChannels, remainingS);

    // (f) reassociate and reconnect MQTT (which re-announces + retained online),
    // then release the client back to the MQTT task. If the immediate reconnect
    // fails, the resumed MQTT task retries via backoff and the outbox drains
    // then — so the survey's chunks/done are never lost.
    WiFi.mode(WIFI_STA);
    WiFi.begin(s_cfg.ssid, s_cfg.wifi_pass);
    uint32_t wifiWait = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiWait) < 20000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
        mqttConnect();
    }
    mqttSurveyPauseEnd();
    radioRelease();
    return true;
}

// --- passive IDS sniff -----------------------------------------------------
//
// Reached from the WiFi driver's promiscuous callback like the survey sniffer,
// so the shared IDS buffer is guarded by a spinlock through file statics (a C
// callback cannot carry the IdsBuffers pointer).
static portMUX_TYPE s_idsMux = portMUX_INITIALIZER_UNLOCKED;
static IdsBuffers* s_ids = nullptr;
static const IdsKnownAp* s_known = nullptr;
static size_t s_nKnown = 0;

// Known-AP lookups — both O(nKnown) with nKnown tiny (operator-supplied list).
static int knownByBssid(const uint8_t* bssid) {
    for (size_t i = 0; i < s_nKnown; ++i)
        if (memcmp(s_known[i].bssid, bssid, 6) == 0) return (int)i;
    return -1;
}
static int knownBySsid(const char* ssid) {
    for (size_t i = 0; i < s_nKnown; ++i)
        if (strcmp(s_known[i].ssid, ssid) == 0) return (int)i;
    return -1;
}

// Alert-table lookups (called with s_idsMux held). alertCap is small, so the
// linear scans are cheap even inside the critical section.
static IdsAlert* findDeauth(const uint8_t* src) {
    for (size_t i = 0; i < s_ids->alertCount; ++i)
        if (s_ids->alerts[i].kind == IDS_DEAUTH_FLOOD &&
            memcmp(s_ids->alerts[i].src, src, 6) == 0) return &s_ids->alerts[i];
    return nullptr;
}
static IdsAlert* findBeaconAlert(const uint8_t* bssid) {
    for (size_t i = 0; i < s_ids->alertCount; ++i)
        if ((s_ids->alerts[i].kind == IDS_ROGUE_AP || s_ids->alerts[i].kind == IDS_EVIL_TWIN) &&
            memcmp(s_ids->alerts[i].src, bssid, 6) == 0) return &s_ids->alerts[i];
    return nullptr;
}
static IdsAlert* newAlert() {
    if (s_ids->alertCount >= s_ids->alertCap) { s_ids->alertDropped++; return nullptr; }
    IdsAlert* a = &s_ids->alerts[s_ids->alertCount++];
    memset(a, 0, sizeof(*a));
    return a;
}

// Classifies one captured frame. Frame parsing (read-only against the packet) is
// done before the lock so the critical section is just the counter/table commit.
static void idsCb(void* buf, wifi_promiscuous_pkt_type_t /*type*/) {
    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
    if (p->rx_ctrl.sig_len < 24) return;  // shorter than an 802.11 MAC header
    const uint8_t* h = p->payload;
    int8_t rssi = p->rx_ctrl.rssi;
    uint8_t ch = p->rx_ctrl.channel;

    uint8_t ftype = (h[0] >> 2) & 0x03;    // 0 = mgmt, 1 = ctrl, 2 = data
    uint8_t subtype = (h[0] >> 4) & 0x0f;
    bool isDeauth   = (ftype == 0 && (subtype == 0x0c || subtype == 0x0a));  // deauth/disassoc
    bool isProbeReq = (ftype == 0 && subtype == 0x04);
    bool isBeacon   = (ftype == 0 && subtype == 0x08);

    // Beacon SSID lives in the first tagged parameter (id 0) after the 24-byte
    // header + 12-byte fixed beacon params. Parse and sanitize to printable ASCII
    // (advertisement data is untrusted and may not be valid UTF-8).
    char ssid[33];
    ssid[0] = '\0';
    if (isBeacon && p->rx_ctrl.sig_len >= 38) {
        const uint8_t* tags = h + 36;
        size_t remain = (size_t)p->rx_ctrl.sig_len - 36;
        if (remain >= 2 && tags[0] == 0x00) {
            uint8_t slen = tags[1];
            if (slen <= 32 && (size_t)(2 + slen) <= remain) {
                for (uint8_t k = 0; k < slen; ++k) {
                    unsigned char c = tags[2 + k];
                    ssid[k] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
                }
                ssid[slen] = '\0';
            }
        }
    }
    uint32_t now = (uint32_t)time(nullptr);  // captured before the lock (may take one)

    portENTER_CRITICAL_ISR(&s_idsMux);
    if (s_ids == nullptr) { portEXIT_CRITICAL_ISR(&s_idsMux); return; }
    s_ids->total++;
    if (ftype == 0) s_ids->mgmt++;
    else if (ftype == 1) s_ids->ctrl++;
    else if (ftype == 2) s_ids->data++;

    if (isDeauth) {
        s_ids->deauth++;
        const uint8_t* src = h + 10;   // addr2 = transmitter
        const uint8_t* dst = h + 4;    // addr1 = receiver (target)
        IdsAlert* a = findDeauth(src);
        if (a == nullptr) {
            a = newAlert();
            if (a) {
                a->kind = IDS_DEAUTH_FLOOD;
                memcpy(a->src, src, 6);
                memcpy(a->dst, dst, 6);
                a->channel = ch;
                a->rssi = rssi;
                a->count = 1;
                a->firstSeen = now;
                a->lastSeen = now;
            }
        } else {
            a->count++;
            a->lastSeen = now;
        }
    } else if (isProbeReq) {
        s_ids->probeReq++;
    } else if (isBeacon) {
        const uint8_t* bssid = h + 16;  // addr3
        if (knownByBssid(bssid) < 0) {  // an allow-listed BSSID is never an alert
            int ks = ssid[0] ? knownBySsid(ssid) : -1;
            IdsAlert* a = findBeaconAlert(bssid);
            if (a == nullptr) {
                a = newAlert();
                if (a) {
                    memcpy(a->src, bssid, 6);
                    strncpy(a->ssid, ssid, sizeof(a->ssid) - 1);
                    a->ssid[sizeof(a->ssid) - 1] = '\0';
                    a->channel = ch;
                    a->rssi = rssi;
                    if (ks >= 0) {  // known SSID on an unknown BSSID → evil twin
                        a->kind = IDS_EVIL_TWIN;
                        memcpy(a->expected, s_known[ks].bssid, 6);
                    } else {
                        a->kind = IDS_ROGUE_AP;
                    }
                }
            } else if (rssi > a->rssi) {
                a->rssi = rssi;  // keep the strongest sighting
            }
        }
    }
    portEXIT_CRITICAL_ISR(&s_idsMux);
}

bool radioRunIds(const char* jobId, uint16_t durationS,
                 const uint8_t* channels, size_t nChannels,
                 const IdsKnownAp* known, size_t nKnown,
                 IdsBuffers& buf,
                 char* errCode, size_t ecs, char* errMsg, size_t ems) {
    if (!radioAcquire()) {
        snprintf(errCode, ecs, "radio_conflict");
        snprintf(errMsg, ems, "radio already in use");
        return false;
    }

    // Same handoff sequence as the survey: accepted while connected, retained
    // surveying status, hand off PubSubClient, clean DISCONNECT (no false LWT),
    // then leave the AP.
    mqttPublishAccepted(jobId);
    mqttPublishSurveying((uint32_t)durationS + 12u);
    mqttSurveyPauseBegin();
    uint32_t t0 = millis();
    while (!mqttSurveyPausedAck() && (millis() - t0) < 1500) delay(10);
    mqttDisconnectClean();
    WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
    delay(50);

    portENTER_CRITICAL(&s_idsMux);
    s_ids = &buf;
    s_known = known;
    s_nKnown = nKnown;
    portEXIT_CRITICAL(&s_idsMux);

    esp_wifi_set_promiscuous_rx_cb(&idsCb);
    esp_wifi_set_promiscuous(true);
    s_state = RADIO_PROMISCUOUS;

    uint32_t endMs = millis() + (uint32_t)durationS * 1000u;
    size_t hop = 0;
    while ((int32_t)(endMs - millis()) > 0) {
        uint8_t ch = channels[hop % nChannels];
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        delay(DWELL_MS);
        ++hop;
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    s_state = RADIO_STATION;

    portENTER_CRITICAL(&s_idsMux);
    s_ids = nullptr;
    s_known = nullptr;
    s_nKnown = 0;
    portEXIT_CRITICAL(&s_idsMux);

    // Reassociate and reconnect (re-announce + retained online); on failure the
    // resumed MQTT task retries via backoff and the outbox drains then.
    WiFi.mode(WIFI_STA);
    WiFi.begin(s_cfg.ssid, s_cfg.wifi_pass);
    uint32_t wifiWait = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiWait) < 20000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
        mqttConnect();
    }
    mqttSurveyPauseEnd();
    radioRelease();
    return true;
}
