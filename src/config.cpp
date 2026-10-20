#include "config.h"
#include <Preferences.h>

static Preferences prefs;
static const char *NS = "bc250";

static SemaphoreHandle_t mux = nullptr;

struct Lock {
  Lock() { xSemaphoreTakeRecursive(mux, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(mux); }
};

static Controller ctls[MAX_CONTROLLERS];
static int        ctlCount = 0;
static String     passHash;
static bool       forceSetup = false;
static String     wifiSsid;
static String     wifiPass;

// NVS keys are limited to 15 characters: "c0a"/"c0n" = slot 0 address/name.
static void slotKeys(int i, char *addrKey, char *nameKey) {
  sprintf(addrKey, "c%da", i);
  sprintf(nameKey, "c%dn", i);
}

static void copyStr(char *dst, size_t size, const String &src) {
  strncpy(dst, src.c_str(), size - 1);
  dst[size - 1] = 0;
}

// Rewrites every slot so the stored list stays compact after a removal.
static void saveControllers() {
  prefs.begin(NS, false);
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    char ak[8], nk[8];
    slotKeys(i, ak, nk);
    if (i < ctlCount) {
      prefs.putString(ak, ctls[i].addr);
      prefs.putString(nk, ctls[i].name);
    } else {
      prefs.remove(ak);
      prefs.remove(nk);
    }
  }
  prefs.end();
}

// getString() on a missing key works but logs an error; check first.
static String getStr(const char *key) {
  return prefs.isKey(key) ? prefs.getString(key, "") : String();
}

void loadConfig() {
  if (!mux) mux = xSemaphoreCreateRecursiveMutex();
  Lock lock;

  prefs.begin(NS, true);  // read-only
  passHash   = getStr("passHash");
  forceSetup = prefs.getBool("forceSetup", false);
  wifiSsid   = getStr("wifiSsid");
  wifiPass   = getStr("wifiPass");
  ctlCount = 0;
  for (int i = 0; i < MAX_CONTROLLERS; i++) {
    char ak[8], nk[8];
    slotKeys(i, ak, nk);
    String addr = getStr(ak);
    if (addr.isEmpty()) break;
    copyStr(ctls[ctlCount].addr, sizeof(ctls[0].addr), addr);
    copyStr(ctls[ctlCount].name, sizeof(ctls[0].name), getStr(nk));
    ctlCount++;
  }
  String legacy = getStr("wakeAddr");
  prefs.end();

  // Firmware before multi-controller support stored a single "wakeAddr".
  if (!legacy.isEmpty()) {
    if (ctlCount == 0) addController(legacy, "");
    prefs.begin(NS, false);
    prefs.remove("wakeAddr");
    prefs.end();
  }
}

bool isValidAddr(const String &addr) {
  if (addr.length() != 17) return false;
  for (int i = 0; i < 17; i++) {
    char c = addr[i];
    if (i % 3 == 2) {
      if (c != ':') return false;
    } else if (!isxdigit((unsigned char)c)) {
      return false;
    }
  }
  return true;
}

static int findSlot(const String &addr) {
  for (int i = 0; i < ctlCount; i++) {
    if (addr.equalsIgnoreCase(ctls[i].addr)) return i;
  }
  return -1;
}

int getControllers(Controller out[MAX_CONTROLLERS]) {
  Lock lock;
  memcpy(out, ctls, sizeof(ctls));
  return ctlCount;
}

bool addController(const String &addr, const String &name) {
  Lock lock;
  if (!isValidAddr(addr) || findSlot(addr) >= 0 || ctlCount >= MAX_CONTROLLERS) {
    return false;
  }
  String lower = addr;
  lower.toLowerCase();
  copyStr(ctls[ctlCount].addr, sizeof(ctls[0].addr), lower);
  copyStr(ctls[ctlCount].name, sizeof(ctls[0].name), name);
  ctlCount++;
  saveControllers();
  return true;
}

bool removeController(const String &addr) {
  Lock lock;
  int i = findSlot(addr);
  if (i < 0) return false;
  for (; i < ctlCount - 1; i++) ctls[i] = ctls[i + 1];
  ctlCount--;
  saveControllers();
  return true;
}

bool renameController(const String &addr, const String &name) {
  Lock lock;
  int i = findSlot(addr);
  if (i < 0) return false;
  copyStr(ctls[i].name, sizeof(ctls[0].name), name);
  saveControllers();
  return true;
}

String getPassHash() {
  Lock lock;
  return passHash;
}

void setPassHash(const String &hash) {
  Lock lock;
  passHash = hash;
  prefs.begin(NS, false);
  prefs.putString("passHash", hash);
  prefs.end();
}

bool hasPassword() {
  Lock lock;
  return passHash.length() > 0;
}

bool getForceSetup() {
  Lock lock;
  return forceSetup;
}

void setForceSetup(bool force) {
  Lock lock;
  forceSetup = force;
  prefs.begin(NS, false);
  prefs.putBool("forceSetup", force);
  prefs.end();
}

void getWifi(String &ssid, String &pass) {
  Lock lock;
  ssid = wifiSsid;
  pass = wifiPass;
}

void setWifi(const String &ssid, const String &pass) {
  Lock lock;
  wifiSsid = ssid;
  wifiPass = pass;
  prefs.begin(NS, false);
  prefs.putString("wifiSsid", ssid);
  prefs.putString("wifiPass", pass);
  prefs.end();
}
