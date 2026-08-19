// Radio scheduling and the survey sequence (spec §6). The 2.4 GHz radio has two
// mutually exclusive states — associated STATION (MQTT up) and off-AP
// PROMISCUOUS (MQTT down by design) — arbitrated by a single owner. Any radio
// request while the lock is held returns `radio_conflict`.
//
// §6.1 is the load-bearing rule: the survey MUST publish `accepted` and a
// retained `surveying` status, then cleanly DISCONNECT MQTT (which suppresses the
// Last Will) *before* leaving the AP. Skipping the clean disconnect fires a false
// node-death alert on every survey.
//
// Single-writer note (spec §5): PubSubClient is owned by the MQTT task. The
// survey runs on the worker task, so it hands the client over via
// mqttSurveyPause* — the MQTT task steps aside for the survey window, leaving the
// worker the sole writer during it. Ownership is passed, never shared.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

enum RadioState { RADIO_STATION, RADIO_PROMISCUOUS };

// A decoded access point (spec §6.3). `auth` is the raw wifi_auth_mode_t code;
// radioAuthName() maps it to the protocol's lowercase string.
struct ApRecord {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint8_t auth;
    bool hidden;
};

// A client station observed associated to a BSSID (spec §6.3).
struct ClientRecord {
    uint8_t mac[6];
    uint8_t bssid[6];
    int8_t rssi;
};

// Caller-owned survey buffers (sized from free heap by wifi_survey). radioRunSurvey
// fills them, de-duplicated by BSSID / station MAC keeping the strongest RSSI, and
// counts overflow drops (§6.3, §8.5).
struct SurveyBuffers {
    ApRecord* aps;
    size_t apCap;
    size_t apCount;
    uint32_t apDropped;
    ClientRecord* clients;
    size_t clientCap;
    size_t clientCount;
    uint32_t clientDropped;
};

// Stores the WiFi credentials the survey needs to reassociate after the radio
// window. Call once at boot after loadConfig, before any survey.
void radioInit(const ProbeConfig& cfg);

// True while the radio is associated (STATION). The monitor scheduler checks this
// to skip cycles that fall due during a survey (§6.2).
bool radioInStation();

// Maps a wifi_auth_mode_t code to the protocol `auth` string (e.g. "wpa2").
const char* radioAuthName(uint8_t authMode);

// Runs the full §6.1 survey sequence: accepted → retained surveying → clean
// DISCONNECT → leave AP → AP scan + client sniff (channel-hopping for durationS)
// → reassociate → reconnect MQTT (re-announce + retained online). Fills `buf`.
// Returns true on success; false with errCode/errMsg set on `radio_conflict`
// (lock already held). Chunk emission and `done` are the caller's job.
bool radioRunSurvey(const char* jobId, uint16_t durationS,
                    const uint8_t* channels, size_t nChannels, bool passive,
                    SurveyBuffers& buf,
                    char* errCode, size_t ecs, char* errMsg, size_t ems);

// --- passive IDS (wifi_ids, spec §6.4) ------------------------------------
//
// wifi_ids reuses the survey's §6.1 radio choreography (including the retained
// `surveying` status — it is the same off-AP window, just classifying frames
// instead of cataloguing them) but is a strictly passive listener: it transmits
// nothing. It is the detection counterpart to the deauth attack the firmware
// deliberately does not implement.

// A known-good AP the operator supplied. A beacon whose BSSID is on this list is
// legitimate; an unknown BSSID advertising a known SSID is an evil twin; an
// unknown BSSID with an unknown SSID is a rogue AP.
struct IdsKnownAp {
    uint8_t bssid[6];
    char ssid[33];
};

enum IdsAlertKind { IDS_DEAUTH_FLOOD = 0, IDS_ROGUE_AP = 1, IDS_EVIL_TWIN = 2 };

// One security event. Which fields are meaningful depends on `kind`:
//   IDS_DEAUTH_FLOOD: src = transmitter MAC, dst = target MAC, count/first/last
//   IDS_ROGUE_AP:     src = BSSID, ssid, channel, rssi
//   IDS_EVIL_TWIN:    src = BSSID, expected = the known BSSID for that SSID
struct IdsAlert {
    uint8_t kind;
    uint8_t src[6];
    uint8_t dst[6];
    uint8_t expected[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint32_t count;
    uint32_t firstSeen;
    uint32_t lastSeen;
};

// Caller-owned alert buffer plus frame-type tallies. radioRunIds de-duplicates
// alerts (deauth by transmitter MAC, rogue/evil-twin by BSSID) and counts drops
// on overflow (§8.5), the same discipline as the survey buffers.
struct IdsBuffers {
    IdsAlert* alerts;
    size_t alertCap;
    size_t alertCount;
    uint32_t alertDropped;
    uint32_t total;
    uint32_t mgmt;
    uint32_t data;
    uint32_t ctrl;
    uint32_t deauth;
    uint32_t probeReq;
};

// Runs the §6.1 disconnect sequence, then listens in promiscuous mode for
// durationS (channel-hopping across `channels`), classifying every frame against
// `known` to fill `buf`, then reassociates and reconnects MQTT. Chunk emission
// and `done` are the caller's job. Returns false with errCode/errMsg set on
// radio_conflict.
bool radioRunIds(const char* jobId, uint16_t durationS,
                 const uint8_t* channels, size_t nChannels,
                 const IdsKnownAp* known, size_t nKnown,
                 IdsBuffers& buf,
                 char* errCode, size_t ecs, char* errMsg, size_t ems);
