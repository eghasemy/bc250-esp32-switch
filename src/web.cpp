#include "web.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include "mbedtls/sha256.h"

#include "board.h"
#include "ble.h"
#include "config.h"
#include "leds.h"
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

// --- LED bar ---

// "rrggbb" or "#rrggbb" -> 0xRRGGBB; false if malformed.
static bool parseColor(const String &s, uint32_t *out) {
  const char *p = s.c_str();
  if (*p == '#') p++;
  if (strlen(p) != 6) return false;
  for (int i = 0; i < 6; i++) {
    if (!isxdigit((unsigned char)p[i])) return false;
  }
  *out = strtoul(p, nullptr, 16);
  return true;
}

static String colorHex(uint32_t c) {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%06x", (unsigned)(c & 0xffffff));
  return String(hex);
}

static void handleLeds(AsyncWebServerRequest *req) {
  if (!requireAuth(req)) return;
  LedConfig c = getLedConfig();
  JsonDocument doc;
  JsonObject cfg = doc["config"].to<JsonObject>();
  cfg["enabled"]    = c.enabled;
  cfg["count"]      = c.count;
  cfg["brightness"] = c.brightness;
  cfg["order"]      = c.order;
  cfg["network"]    = c.network;
  cfg["bootAnim"]   = c.bootAnim;
  cfg["offScene"]   = c.offScene;
  cfg["offColor"]   = colorHex(c.offColor);
  cfg["idleScene"]  = c.idleScene;
  cfg["idleColor"]  = colorHex(c.idleColor);
  ledsStatusJson(doc["status"].to<JsonObject>());
  doc["pin"]     = LED_PIN;
  doc["maxLeds"] = MAX_LEDS;
  doc["udpPort"] = LED_UDP_PORT;
  doc["ip"]      = WiFi.localIP().toString();
  sendJson(req, 200, doc);
}

// Updates the fields present in the body; the rest keep their values.
static void handleLedsSave(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!requireAuth(req)) return;
  JsonObject o = json.as<JsonObject>();
  LedConfig c = getLedConfig();
  if (o["enabled"].is<bool>())  c.enabled  = o["enabled"];
  if (o["network"].is<bool>())  c.network  = o["network"];
  if (o["bootAnim"].is<bool>()) c.bootAnim = o["bootAnim"];
  if (!o["count"].isNull()) {
    int n = o["count"] | 0;
    if (n < 1 || n > MAX_LEDS) { sendJsonError(req, 400, "LED count must be 1-64"); return; }
    c.count = n;
  }
  if (!o["brightness"].isNull()) {
    int b = o["brightness"] | 0;
    if (b < 1 || b > 255) { sendJsonError(req, 400, "brightness must be 1-255"); return; }
    c.brightness = b;
  }
  if (!o["order"].isNull()) {
    int v = o["order"] | -1;
    if (v < 0 || v >= LED_ORDER_COUNT) { sendJsonError(req, 400, "unknown colour order"); return; }
    c.order = v;
  }
  if (!o["offScene"].isNull()) {
    int v = o["offScene"] | -1;
    if (v != SCENE_OFF && v != SCENE_COLOR) { sendJsonError(req, 400, "unknown standby scene"); return; }
    c.offScene = v;
  }
  if (!o["idleScene"].isNull()) {
    int v = o["idleScene"] | -1;
    if (v < SCENE_OFF || v > SCENE_RAINBOW) { sendJsonError(req, 400, "unknown idle scene"); return; }
    c.idleScene = v;
  }
  if (!o["offColor"].isNull() && !parseColor(o["offColor"] | "", &c.offColor)) {
    sendJsonError(req, 400, "colours are #rrggbb");
    return;
  }
  if (!o["idleColor"].isNull() && !parseColor(o["idleColor"] | "", &c.idleColor)) {
    sendJsonError(req, 400, "colours are #rrggbb");
    return;
  }
  setLedConfig(c);
  ledsConfigChanged();
  sendOk(req);
}

static void handleLedsTest(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!requireAuth(req)) return;
  uint32_t color;
  if (!parseColor(json["color"] | "", &color)) {
    sendJsonError(req, 400, "colours are #rrggbb");
    return;
  }
  if (!getLedConfig().enabled) { sendJsonError(req, 409, "the LED bar is disabled"); return; }
  ledsTest(color);
  sendOk(req);
}

// --- WLED compatibility ---
// Just enough of WLED's HTTP API for the PC bridge (steamos-led-wled): its
// installer finds the device through /json/info, and it switches the bar on and
// off with /win&T=. Open like WLED itself, but it only reaches the LED bar, and
// only while the bar is enabled with network control on.

static bool wledEnabled(AsyncWebServerRequest *req) {
  LedConfig c = getLedConfig();
  if (c.enabled && c.network) return true;
  sendJsonError(req, 404, "not found");
  return false;
}

static void wledState(JsonObject out) {
  out["on"]   = ledsWledOn();
  out["bri"]  = getLedConfig().brightness;
  out["live"] = ledsStreaming();
}

static void wledInfo(JsonObject out) {
  LedConfig c = getLedConfig();
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  out["ver"]     = "0.15.0";  // the WLED API level these endpoints follow
  out["name"]    = "BC250 Switch";
  out["brand"]   = "WLED";    // what WLED clients look for
  out["product"] = "BC250 Switch";
  out["arch"]    = "esp32";
  out["mac"]     = mac;
  out["ip"]      = WiFi.localIP().toString();
  out["udpport"] = LED_UDP_PORT;
  out["live"]    = ledsStreaming();
  out["uptime"]  = millis() / 1000;
  JsonObject leds = out["leds"].to<JsonObject>();
  leds["count"] = c.count;
  leds["rgbw"]  = false;
  leds["fps"]   = ledsStreamFps();
}

static void handleWledInfo(AsyncWebServerRequest *req) {
  if (!wledEnabled(req)) return;
  JsonDocument doc;
  wledInfo(doc.to<JsonObject>());
  sendJson(req, 200, doc);
}

static void handleWledState(AsyncWebServerRequest *req) {
  if (!wledEnabled(req)) return;
  JsonDocument doc;
  wledState(doc.to<JsonObject>());
  sendJson(req, 200, doc);
}

static void handleWledAll(AsyncWebServerRequest *req) {
  if (!wledEnabled(req)) return;
  JsonDocument doc;
  wledState(doc["state"].to<JsonObject>());
  wledInfo(doc["info"].to<JsonObject>());
  sendJson(req, 200, doc);
}

// POST /json/state {"on": true|false|"t"}. Other fields are accepted and ignored.
static void handleWledSetState(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!wledEnabled(req)) return;
  JsonVariant on = json["on"];
  if (on.is<bool>()) {
    ledsSetWledOn(on.as<bool>() ? 1 : 0);
  } else if (on.is<const char *>() && strcmp(on.as<const char *>(), "t") == 0) {
    ledsSetWledOn(2);
  }
  JsonDocument doc;
  wledState(doc.to<JsonObject>());
  sendJson(req, 200, doc);
}

// /win&T=0|1|2 (WLED's legacy API puts parameters after '&' with no '?').
// Other /win parameters are accepted and ignored.
static void handleWin(AsyncWebServerRequest *req) {
  if (!wledEnabled(req)) return;
  String t;
  if (req->hasParam("T")) {
    t = req->getParam("T")->value();
  } else {
    int i = req->url().indexOf("&T=");
    if (i >= 0) t = req->url().substring(i + 3, i + 4);
  }
  if (t == "0" || t == "1" || t == "2") ledsSetWledOn(t.toInt());
  req->send(200, "text/xml", ledsWledOn() ? "<vs><ac>255</ac></vs>" : "<vs><ac>0</ac></vs>");
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
  server.on("/api/leds", HTTP_GET, handleLeds);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/leds/test", handleLedsTest));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/leds", handleLedsSave));

  // WLED compatibility. More specific paths first: a handler for "/json" also
  // matches "/json/...".
  server.on("/json/info", HTTP_GET, handleWledInfo);
  server.on("/json/state", HTTP_GET, handleWledState);
  server.addHandler(new AsyncCallbackJsonWebHandler("/json/state", handleWledSetState));
  server.on("/json", HTTP_GET, handleWledAll);

  server.on("/", HTTP_GET, serveIndex);
  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->url().startsWith("/win")) {
      handleWin(req);
    } else if (req->url().startsWith("/api/")) {
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
