#include "net.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "board.h"
#include "config.h"
#include "log.h"

static bool      g_setupMode = false;
static DNSServer dnsServer;
static bool      apActive = false;

// Main-task copies of the router credentials (config is the source of truth).
static String ssid;
static String pass;

static bool          staWasConnected = false;
static unsigned long staDownSince    = 0;  // router unreachable since
static unsigned long lastAttempt     = 0;
static volatile bool reconnectRequested = false;

static void startAp() {
  WiFi.mode(g_setupMode ? WIFI_AP : WIFI_AP_STA);
  bool ok = WiFi.softAP(AP_SSID);  // open network
  // Must be set AFTER softAP().
  WiFi.setTxPower(AP_TX_POWER);
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());  // captive portal: every name -> us
  apActive = true;
  logEvent("[NET ] AP '%s' up (ret=%d) at %s, txpwr=%d", AP_SSID, ok,
           WiFi.softAPIP().toString().c_str(), WiFi.getTxPower());
}

static void stopAp() {
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  apActive = false;
  logEvent("[NET ] fallback AP off");
}

static void joinRouter(unsigned long now) {
  lastAttempt = now;
  if (ssid.isEmpty()) return;
  WiFi.begin(ssid.c_str(), pass.isEmpty() ? nullptr : pass.c_str());
}

void netBegin(bool setupMode) {
  g_setupMode = setupMode;
  WiFi.persistent(false);       // credentials live in our own config
  WiFi.setHostname(HOSTNAME);   // must precede mode()

  if (setupMode) {
    startAp();
    return;
  }

  getWifi(ssid, pass);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // retries are paced in netLoop()
  unsigned long now = millis();
  staDownSince = now;
  if (ssid.isEmpty()) {
    logEvent("[NET ] no router configured");
    startAp();
  } else {
    logEvent("[NET ] joining '%s'", ssid.c_str());
    joinRouter(now);
  }

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
  } else {
    logEvent("[NET ] mDNS failed to start");
  }
}

void netReconnect() {
  reconnectRequested = true;
}

void netLoop(unsigned long now) {
  if (apActive) dnsServer.processNextRequest();
  if (g_setupMode) return;

  if (reconnectRequested) {
    reconnectRequested = false;
    getWifi(ssid, pass);
    WiFi.disconnect();
    staWasConnected = false;
    staDownSince = now;
    if (ssid.isEmpty()) {
      logEvent("[NET ] router removed");
      if (!apActive) startAp();
    } else {
      logEvent("[NET ] joining '%s'", ssid.c_str());
      joinRouter(now);
    }
  }

  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != staWasConnected) {
    staWasConnected = connected;
    if (connected) {
      logEvent("[NET ] joined '%s' at %s (rssi %d), http://%s.local",
               ssid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(),
               HOSTNAME);
    } else {
      logEvent("[NET ] lost '%s'", ssid.c_str());
      staDownSince = now;
    }
  }

  if (connected) {
    // Router is back: drop the fallback AP, but not under someone using it.
    if (apActive && WiFi.softAPgetStationNum() == 0) stopAp();
    return;
  }

  if (!apActive && (now - staDownSince) >= STA_FALLBACK_MS) {
    logEvent("[NET ] router unreachable for %lus, starting fallback AP",
             STA_FALLBACK_MS / 1000);
    startAp();
  }
  unsigned long retry = apActive ? STA_RETRY_FALLBACK_MS : STA_RETRY_MS;
  if (!ssid.isEmpty() && (now - lastAttempt) >= retry) joinRouter(now);
}

void netStatusJson(JsonObject out) {
  String curSsid, curPass;
  getWifi(curSsid, curPass);
  bool connected = WiFi.status() == WL_CONNECTED;
  out["mode"]         = g_setupMode ? "setup" : "normal";
  out["ssid"]         = curSsid;
  out["hasPassword"]  = curPass.length() > 0;
  out["staConnected"] = connected;
  if (connected) {
    out["ip"]   = WiFi.localIP().toString();
    out["rssi"] = WiFi.RSSI();
  }
  out["hostname"] = HOSTNAME;
  out["apActive"] = apActive;
  if (apActive) {
    out["apSsid"]    = AP_SSID;
    out["apIp"]      = WiFi.softAPIP().toString();
    out["apClients"] = WiFi.softAPgetStationNum();
  }
}

String netSummary() {
  String s;
  if (WiFi.status() == WL_CONNECTED) {
    s = "sta " + WiFi.localIP().toString();
  } else if (!ssid.isEmpty() && !g_setupMode) {
    s = "sta connecting";
  }
  if (apActive) s += s.isEmpty() ? "ap" : "+ap";
  return s.isEmpty() ? "off" : s;
}
