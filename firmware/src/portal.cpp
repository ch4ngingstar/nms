#include "portal.h"

#include <string.h>
#include <stdlib.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "config.h"
#include "identity.h"

// SoftAP is fixed at 192.168.4.1 (the ESP32 default); the DNS wildcard points
// every lookup here so a phone that joins the AP is bounced to the form no
// matter what URL it opens.
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);
static const byte DNS_PORT = 53;

static DNSServer dnsServer;
static WebServer server(80);

// The provisioning form. One %s (the mqtt_user default = node_id);
// literal percent signs in the CSS are doubled so snprintf_P leaves them alone.
// Kept in flash (PROGMEM) — it is never mutated.
static const char FORM_FMT[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>nms probe setup</title><style>"
    "body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px;background:#111;color:#eee}"
    "h1{font-size:1.2em}label{display:block;margin:12px 0 4px;font-size:.9em}"
    "input{width:100%%;box-sizing:border-box;padding:8px;border:1px solid #444;border-radius:4px;background:#222;color:#eee}"
    "button{margin-top:20px;width:100%%;padding:12px;border:0;border-radius:4px;background:#2a7;color:#fff;font-size:1em}"
    "small{color:#888}</style></head><body>"
    "<h1>nms probe setup</h1>"
    "<form method='POST' action='/save'>"
    "<label>WiFi SSID</label><input name='ssid' maxlength='32' required>"
    "<label>WiFi password</label><input name='wifi_pass' type='password' maxlength='63'>"
    "<label>Broker host</label><input name='broker_host' maxlength='63' required>"
    "<label>Broker port</label><input name='broker_port' type='number' min='1' max='65535' value='1883' required>"
    "<label>MQTT username</label><input name='mqtt_user' maxlength='31' value='%s'>"
    "<label>MQTT password</label><input name='mqtt_pass' type='password' maxlength='63'>"
    "<label>Label <small>(optional)</small></label><input name='label' maxlength='64'>"
    "<button type='submit'>Save &amp; reboot</button>"
    "</form></body></html>";

// Copies the value of POST field `name` into `dst` (always NUL-terminated,
// truncated to fit). Kept tiny so no Arduino String is retained: the library's
// arg() returns a transient String whose c_str() we copy out immediately.
static void copyArg(const char* name, char* dst, size_t dstSize) {
    strlcpy(dst, server.arg(name).c_str(), dstSize);
}

static void handleRoot() {
    // Render into a static buffer (no heap). The form is ~1.3 KB; 2 KB leaves
    // margin for the injected node_id.
    static char page[2048];
    snprintf_P(page, sizeof(page), FORM_FMT, getNodeId());
    server.send(200, "text/html", page);
}

static void handleSave() {
    ProbeConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    copyArg("ssid", cfg.ssid, sizeof(cfg.ssid));
    copyArg("wifi_pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
    copyArg("broker_host", cfg.broker_host, sizeof(cfg.broker_host));
    copyArg("mqtt_user", cfg.mqtt_user, sizeof(cfg.mqtt_user));
    copyArg("mqtt_pass", cfg.mqtt_pass, sizeof(cfg.mqtt_pass));
    copyArg("label", cfg.label, sizeof(cfg.label));

    // Port parsed via atoi on the transient c_str() — no String kept.
    long port = atol(server.arg("broker_port").c_str());

    // MQTT username defaults to node_id when left blank.
    if (cfg.mqtt_user[0] == '\0') {
        strlcpy(cfg.mqtt_user, getNodeId(), sizeof(cfg.mqtt_user));
    }

    // Validate at this boundary (validate input at system boundaries):
    // the two coordinates without which the node cannot reach a broker, plus a
    // legal TCP port. WiFi/MQTT passwords may legitimately be empty.
    if (cfg.ssid[0] == '\0' || cfg.broker_host[0] == '\0' ||
        port < 1 || port > 65535) {
        server.send(400, "text/html",
                    "<!DOCTYPE html><html><body><h1>Invalid</h1>"
                    "<p>SSID, broker host, and a port in 1-65535 are required.</p>"
                    "<a href='/'>Back</a></body></html>");
        return;
    }
    cfg.broker_port = (uint16_t)port;

    if (!saveConfig(cfg)) {
        server.send(500, "text/html",
                    "<!DOCTYPE html><html><body><h1>Save failed</h1>"
                    "<p>Could not write configuration. <a href='/'>Retry</a>.</p>"
                    "</body></html>");
        return;
    }

    server.send(200, "text/html",
                "<!DOCTYPE html><html><body><h1>Saved</h1>"
                "<p>Rebooting to connect&hellip;</p></body></html>");
    // Let the response flush before the reboot into the provisioned path.
    delay(500);
    ESP.restart();
}

void startPortal() {
    // node_id is already "probe-<hex>", so the AP name comes out as
    // "nms-probe-<hex>".
    static char apName[32];
    snprintf(apName, sizeof(apName), "nms-%s", getNodeId());

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
    WiFi.softAP(apName);  // Open network — no password argument.

    // Wildcard: resolve every hostname to us so any navigation hits the form.
    dnsServer.start(DNS_PORT, "*", AP_IP);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    // Anything else (OS captive-portal probes included) → the form.
    server.onNotFound(handleRoot);
    server.begin();
}

void handlePortal() {
    dnsServer.processNextRequest();
    server.handleClient();
}
