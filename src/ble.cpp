#include "ble.h"

#include <NimBLEDevice.h>
#include "board.h"
#include "config.h"
#include "log.h"

static bool g_setupMode = false;

// --- Bound controllers, as seen by the scan task ---
struct Watched {
  char          addr[18];
  char          name[MAX_NICKNAME_LEN + 1];
  unsigned long lastSeen;  // 0 = never
  bool          armed;     // may wake the machine
};
static Watched      watched[MAX_CONTROLLERS];
static int          watchedCount = 0;
static portMUX_TYPE watchedMux = portMUX_INITIALIZER_UNLOCKED;

// --- Discovery results for the web UI's picker ---
struct BleDev {
  char addr[18];
  char name[33];
  int  rssi;
  bool used;
};
static const int    MAX_DEVS = 48;
static BleDev       devs[MAX_DEVS];
static portMUX_TYPE devsMux = portMUX_INITIALIZER_UNLOCKED;

// Discovery is requested from the web task and applied in bleLoop().
static volatile unsigned long discoveryUntil = 0;
static volatile bool          discoveryRequested = false;
static bool                   scanActive = false;  // current scan mode

// Signed: the scan task can stamp lastSeen a moment after the caller sampled
// `now`, and that must still count as present, not as ~49 days ago.
static bool isPresent(const Watched &w, unsigned long now) {
  return w.lastSeen != 0 &&
         (int32_t)(now - w.lastSeen) < (int32_t)BLE_PRESENCE_TIMEOUT_MS;
}

static void recordDevice(const char *addr, const std::string &name, int rssi) {
  portENTER_CRITICAL(&devsMux);
  int idx = -1, free = -1;
  for (int i = 0; i < MAX_DEVS; i++) {
    if (devs[i].used) {
      if (strcmp(devs[i].addr, addr) == 0) { idx = i; break; }
    } else if (free < 0) {
      free = i;
    }
  }
  if (idx < 0 && free >= 0) {
    idx = free;
    devs[idx].used = true;
    strncpy(devs[idx].addr, addr, sizeof(devs[idx].addr) - 1);
    devs[idx].addr[sizeof(devs[idx].addr) - 1] = 0;
    devs[idx].name[0] = 0;
  }
  if (idx >= 0) {
    devs[idx].rssi = rssi;
    if (!name.empty()) {
      strncpy(devs[idx].name, name.c_str(), sizeof(devs[idx].name) - 1);
      devs[idx].name[sizeof(devs[idx].name) - 1] = 0;
    }
  }
  portEXIT_CRITICAL(&devsMux);
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    std::string addrStr = dev->getAddress().toString();
    const char *addr = addrStr.c_str();
    unsigned long now = millis();

    portENTER_CRITICAL(&watchedMux);
    for (int i = 0; i < watchedCount; i++) {
      if (strcmp(watched[i].addr, addr) == 0) {
        watched[i].lastSeen = now ? now : 1;
        break;
      }
    }
    portEXIT_CRITICAL(&watchedMux);

    if (scanActive) recordDevice(addr, dev->getName(), dev->getRSSI());
  }
};
static ScanCallbacks scanCallbacks;

// (Re)start the scan in wake (passive) or discovery (active) mode. Keep the
// duty cycle low whenever WiFi is up: WiFi and BLE share one radio, and a
// high-duty scan starves it (SoftAP beacons stop going out entirely).
static void startScan(bool active) {
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (scan->isScanning()) scan->stop();
  scan->setActiveScan(active);
  if (active) {
    scan->setInterval(DISCOVERY_SCAN_INTERVAL_MS);
    scan->setWindow(DISCOVERY_SCAN_WINDOW_MS);
  } else {
    scan->setInterval(WAKE_SCAN_INTERVAL_MS);
    scan->setWindow(WAKE_SCAN_WINDOW_MS);
  }
  scan->start(0, false);  // 0 = scan continuously
  scanActive = active;
}

void bleBegin(bool setupMode) {
  g_setupMode = setupMode;
  bleSyncControllers();
  // Controllers bound before this boot may wake straight away.
  for (int i = 0; i < watchedCount; i++) watched[i].armed = true;

  NimBLEDevice::init("");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);  // true = report duplicates
  scan->setMaxResults(0);  // don't accumulate results; callbacks are enough
  startScan(setupMode);
  logEvent("[BLE ] scanning (%s), %d controller(s) bound",
           setupMode ? "discovery" : "wake", watchedCount);
}

void bleSyncControllers() {
  Controller ctls[MAX_CONTROLLERS];
  int n = getControllers(ctls);

  portENTER_CRITICAL(&watchedMux);
  Watched next[MAX_CONTROLLERS];
  for (int i = 0; i < n; i++) {
    memcpy(next[i].addr, ctls[i].addr, sizeof(next[i].addr));
    memcpy(next[i].name, ctls[i].name, sizeof(next[i].name));
    next[i].lastSeen = 0;
    next[i].armed = false;
    for (int j = 0; j < watchedCount; j++) {
      if (strcmp(watched[j].addr, ctls[i].addr) == 0) {
        next[i].lastSeen = watched[j].lastSeen;
        next[i].armed = watched[j].armed;
        break;
      }
    }
  }
  memcpy(watched, next, sizeof(next));
  watchedCount = n;
  portEXIT_CRITICAL(&watchedMux);
}

void bleLoop(unsigned long now) {
  // Arm any controller once it has been seen absent.
  portENTER_CRITICAL(&watchedMux);
  for (int i = 0; i < watchedCount; i++) {
    if (!watched[i].armed && !isPresent(watched[i], now)) watched[i].armed = true;
  }
  portEXIT_CRITICAL(&watchedMux);

  if (g_setupMode) return;  // discovery runs for the whole setup session

  if (discoveryRequested) {
    discoveryRequested = false;
    portENTER_CRITICAL(&devsMux);
    memset(devs, 0, sizeof(devs));
    portEXIT_CRITICAL(&devsMux);
    if (!scanActive) {
      startScan(true);
      logEvent("[BLE ] discovery started");
    }
  }
  if (scanActive && (int32_t)(now - discoveryUntil) >= 0) {
    startScan(false);
    logEvent("[BLE ] discovery finished");
  }
}

bool bleWakePresent(unsigned long now) {
  bool present = false;
  portENTER_CRITICAL(&watchedMux);
  for (int i = 0; i < watchedCount; i++) {
    if (watched[i].armed && isPresent(watched[i], now)) { present = true; break; }
  }
  portEXIT_CRITICAL(&watchedMux);
  return present;
}

void bleControllersJson(JsonArray out) {
  Watched snap[MAX_CONTROLLERS];
  int n;
  portENTER_CRITICAL(&watchedMux);
  memcpy(snap, watched, sizeof(watched));
  n = watchedCount;
  portEXIT_CRITICAL(&watchedMux);

  unsigned long now = millis();
  for (int i = 0; i < n; i++) {
    JsonObject o = out.add<JsonObject>();
    o["addr"]    = snap[i].addr;
    o["name"]    = snap[i].name;
    o["present"] = isPresent(snap[i], now);
    o["armed"]   = snap[i].armed;
    if (snap[i].lastSeen) o["seenAgo"] = (now - snap[i].lastSeen) / 1000;
  }
}

void bleStartDiscovery() {
  discoveryUntil = millis() + DISCOVERY_MS;
  discoveryRequested = true;
}

bool bleDiscovering() {
  return g_setupMode || scanActive || discoveryRequested;
}

void bleDevicesJson(JsonArray out) {
  BleDev snap[MAX_DEVS];
  portENTER_CRITICAL(&devsMux);
  memcpy(snap, devs, sizeof(devs));
  portEXIT_CRITICAL(&devsMux);

  for (int i = 0; i < MAX_DEVS; i++) {
    if (!snap[i].used) continue;
    JsonObject o = out.add<JsonObject>();
    o["addr"] = snap[i].addr;
    o["name"] = snap[i].name;
    o["rssi"] = snap[i].rssi;
  }
}
