#include "web.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <esp_random.h>
#include "mbedtls/sha256.h"

#include "board.h"
#include "ble.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "power.h"

static const size_t MIN_PASSWORD_LEN = 6;

static bool           g_setupMode = false;
static AsyncWebServer server(80);

// Deferred restart (so the HTTP response can flush before we reboot).
static unsigned long g_restartAt = 0;

// --- Sessions ---
// Bearer tokens handed out on password set / login, kept in RAM (a reboot signs
// everyone out). Several at once so a phone and a PC can both stay signed in.
// Only touched from the web server task, so no locking.
struct Session {
  char          token[33];
  unsigned long lastUsed;
  bool          used;
};
static const int MAX_SESSIONS = 6;
static Session   sessions[MAX_SESSIONS];

// Failed logins back off exponentially so the password can't be brute-forced.
static int           failedLogins = 0;
static unsigned long loginBlockedUntil = 0;

// --- Helpers ---

static String sha256hex(const String &s) {
  uint8_t out[32];
  mbedtls_sha256((const unsigned char *)s.c_str(), s.length(), out, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", out[i]);
  hex[64] = 0;
  return String(hex);
}

static String newSession() {
  char buf[33];
  for (int i = 0; i < 4; i++) sprintf(buf + i * 8, "%08x", (unsigned)esp_random());
  buf[32] = 0;

  // Reuse a free slot, else evict the least recently used one.
  int slot = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].used) { slot = i; break; }
    if (sessions[i].lastUsed < sessions[slot].lastUsed) slot = i;
  }
  memcpy(sessions[slot].token, buf, sizeof(buf));
  sessions[slot].lastUsed = millis();
  sessions[slot].used = true;
  return String(buf);
}

static void clearSessions() {
  memset(sessions, 0, sizeof(sessions));
}

static Session *findSession(AsyncWebServerRequest *req) {
  if (!req->hasHeader("X-Auth-Token")) return nullptr;
  String token = req->header("X-Auth-Token");
  unsigned long now = millis();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    Session &s = sessions[i];
    if (!s.used) continue;
    if (now - s.lastUsed >= SESSION_IDLE_MS) {
      s.used = false;  // expired
      continue;
    }
    if (token == s.token) {
      s.lastUsed = now;
      return &s;
    }
  }
  return nullptr;
}

static void sendJson(AsyncWebServerRequest *req, int code, JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  req->send(code, "application/json", out);
}

static void sendJsonError(AsyncWebServerRequest *req, int code, const char *msg) {
  JsonDocument doc;
  doc["error"] = msg;
  sendJson(req, code, doc);
}

static void sendOk(AsyncWebServerRequest *req) {
  req->send(200, "application/json", "{\"ok\":true}");
}

static void sendToken(AsyncWebServerRequest *req, const String &token) {
  JsonDocument doc;
  doc["token"] = token;
  sendJson(req, 200, doc);
}

// Returns false (and has answered the request) if there is no valid session.
static bool requireAuth(AsyncWebServerRequest *req) {
  if (findSession(req)) return true;
  sendJsonError(req, 401, "unauthorized");
  return false;
}

static void serveIndex(AsyncWebServerRequest *req) {
  if (SPIFFS.exists("/index.html")) {
    req->send(SPIFFS, "/index.html", "text/html");
  } else {
    req->send(200, "text/html",
              "<h1>BC250 Switch</h1><p>Filesystem image missing. Flash it with "
              "<code>pio run -t uploadfs</code>.</p>");
  }
}

// Nicknames are shown in the UI; keep them short and free of control chars.
static bool validNickname(const String &name) {
  if (name.length() > MAX_NICKNAME_LEN) return false;
  for (size_t i = 0; i < name.length(); i++) {
    if ((unsigned char)name[i] < 0x20) return false;
  }
  return true;
}

// --- Endpoint handlers ---

// Open: what the UI needs to pick its first screen.
static void handleInfo(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["mode"]        = g_setupMode ? "setup" : "normal";
  doc["passwordSet"] = hasPassword();
  doc["hostname"]    = HOSTNAME;
  sendJson(req, 200, doc);
}

static void handleStatus(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
  PowerStatus p = powerStatus();

  JsonDocument doc;
  doc["mode"]   = g_setupMode ? "setup" : "normal";
  doc["uptime"] = millis();

  JsonObject power = doc["power"].to<JsonObject>();
  power["state"]         = p.state;
  power["psuOn"]         = p.psuOn;
  power["boardUp"]       = p.boardUp;
  power["senseMv"]       = p.senseMv;
  power["buttonDown"]    = p.buttonDown;
  power["wakeInhibitMs"] = p.wakeInhibitMs;

  netStatusJson(doc["net"].to<JsonObject>());

  bleControllersJson(doc["controllers"].to<JsonArray>());
  doc["maxControllers"] = MAX_CONTROLLERS;
  doc["discovering"]    = bleDiscovering();
  sendJson(req, 200, doc);
}

static void handleLog(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
  JsonDocument doc;
  doc["now"] = millis();
  logSnapshot(doc["lines"].to<JsonArray>());
  sendJson(req, 200, doc);
}

static void handlePassword(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  String pw = o["password"] | "";
  if (pw.length() < MIN_PASSWORD_LEN) {
    sendJsonError(req, 400, "password too short");
    return;
  }
  if (!hasPassword()) {
    // The first password can only be created in setup mode, which needs the
    // physical 8 s button hold. Otherwise anyone on the (possibly open
    // fallback) network could claim the device.
    if (!g_setupMode) {
      sendJsonError(req, 403, "hold the button 8 s while off to set a password");
      return;
    }
  } else {
    Session *s = findSession(req);
    String current = o["current"] | "";
    if (!s || sha256hex(current) != getPassHash()) {
      sendJsonError(req, 401, "current password is wrong");
      return;
    }
  }
  setPassHash(sha256hex(pw));
  clearSessions();  // sign out everyone else
  logEvent("[WEB ] password set");
  sendToken(req, newSession());
}

static void handleLogin(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!hasPassword()) {
    sendJsonError(req, 400, "no password set");
    return;
  }
  unsigned long now = millis();
  if ((int32_t)(loginBlockedUntil - now) > 0) {
    sendJsonError(req, 429, "too many attempts, wait a moment");
    return;
  }
  JsonObject o = json.as<JsonObject>();
  String pw = o["password"] | "";
  if (sha256hex(pw) != getPassHash()) {
    failedLogins++;
    if (failedLogins >= 3) {
      unsigned long wait = 1000UL << min(failedLogins - 3, 6);  // 1 s .. 64 s
      loginBlockedUntil = now + wait;
    }
    logEvent("[WEB ] failed login (%d)", failedLogins);
    sendJsonError(req, 401, "wrong password");
    return;
  }
  failedLogins = 0;
  sendToken(req, newSession());
}

static void handleLogout(AsyncWebServerRequest *req) {
  Session *s = findSession(req);
  if (s) s->used = false;
  sendOk(req);
}

static void handlePower(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!requireAuth(req)) return;
  JsonObject o = json.as<JsonObject>();
  String action = o["action"] | "";
  bool ok;
  if (action == "on") {
    ok = requestPowerOn();
  } else if (action == "off") {
    ok = requestForceOff();
  } else {
    sendJsonError(req, 400, "action must be \"on\" or \"off\"");
    return;
  }
  if (!ok) {
    sendJsonError(req, 409, "not possible in the current state");
    return;
  }
  sendOk(req);
}

static void handleDiscover(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
  bleStartDiscovery();
  sendOk(req);
}

static void handleBleDevices(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
  JsonDocument doc;
  doc["discovering"] = bleDiscovering();
  bleDevicesJson(doc["devices"].to<JsonArray>());
  sendJson(req, 200, doc);
}

static void handleControllerAdd(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!requireAuth(req)) return;
  JsonObject o = json.as<JsonObject>();
  String addr = o["addr"] | "";
  String name = o["name"] | "";
  addr.trim();
  name.trim();
  if (!isValidAddr(addr)) { sendJsonError(req, 400, "invalid address"); return; }
  if (!validNickname(name)) { sendJsonError(req, 400, "nickname too long"); return; }
  if (!addController(addr, name)) {
    sendJsonError(req, 409, "already bound, or all controller slots are full");
    return;
  }
  bleSyncControllers();
  logEvent("[WEB ] bound controller %s %s", addr.c_str(), name.c_str());
  sendOk(req);
}

static void handleControllerRemove(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!requireAuth(req)) return;
  String addr = json["addr"] | "";
  if (!removeController(addr)) { sendJsonError(req, 404, "not bound"); return; }
  bleSyncControllers();
  logEvent("[WEB ] removed controller %s", addr.c_str());
  sendOk(req);
}

static void handleControllerRename(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!requireAuth(req)) return;
  String addr = json["addr"] | "";
  String name = json["name"] | "";
  name.trim();
  if (!validNickname(name)) { sendJsonError(req, 400, "nickname too long"); return; }
  if (!renameController(addr, name)) { sendJsonError(req, 404, "not bound"); return; }
  bleSyncControllers();
  sendOk(req);
}

static void handleWifi(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!requireAuth(req)) return;
  JsonObject o = json.as<JsonObject>();
  String ssid = o["ssid"] | "";
  String pass = o["password"] | "";
  bool keepPassword = o["keepPassword"] | false;  // blank field = unchanged
  ssid.trim();
  if (ssid.length() > 32) { sendJsonError(req, 400, "network name too long"); return; }

  String oldSsid, oldPass;
  getWifi(oldSsid, oldPass);
  if (keepPassword) pass = oldPass;
  if (ssid.isEmpty()) pass = "";
  if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 63)) {
    sendJsonError(req, 400, "WiFi password must be 8-63 characters");
    return;
  }
  setWifi(ssid, pass);
  logEvent("[WEB ] router set to '%s'", ssid.c_str());
  // Setup mode applies it after "Finish" restarts into normal mode.
  if (!g_setupMode) netReconnect();
  sendOk(req);
}

static void handleFinish(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
  if (!g_setupMode) { sendJsonError(req, 400, "not in setup mode"); return; }
  setForceSetup(false);
  sendOk(req);
  logEvent("[WEB ] setup complete, restarting into normal mode");
  g_restartAt = millis() + 800;
}

// --- Lifecycle ---

void webBegin(bool setupMode) {
  g_setupMode = setupMode;

  if (!SPIFFS.begin(true)) {
    logEvent("[WEB ] WARNING: SPIFFS mount failed");
  }

  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/log", HTTP_GET, handleLog);
  server.on("/api/logout", HTTP_POST, handleLogout);
  server.on("/api/ble/discover", HTTP_POST, handleDiscover);
  server.on("/api/ble/devices", HTTP_GET, handleBleDevices);
  server.on("/api/finish", HTTP_POST, handleFinish);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/password", handlePassword));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/login", handleLogin));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/power", handlePower));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/controllers/add", handleControllerAdd));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/controllers/remove", handleControllerRemove));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/controllers/rename", handleControllerRename));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/wifi", handleWifi));

  server.on("/", HTTP_GET, serveIndex);
  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->url().startsWith("/api/")) {
      sendJsonError(req, 404, "not found");
    } else {
      serveIndex(req);  // captive-portal catch-all
    }
  });

  server.begin();
  logEvent("[WEB ] web server up");
}

void webLoop(unsigned long now) {
  if (g_restartAt && (int32_t)(now - g_restartAt) >= 0) {
    ESP.restart();
  }
}
