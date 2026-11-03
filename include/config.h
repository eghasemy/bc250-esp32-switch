#pragma once

#include <Arduino.h>

// Persistent configuration, stored in NVS via the Preferences library.
//   controllers - up to MAX_CONTROLLERS bound BLE controllers. Any of them
//                 wakes the machine. addr is lower-case colon form ("aa:bb:..").
//   passHash    - SHA-256 hex of the web UI password ("" => not set yet).
//   forceSetup  - request that the next boot enters the WiFi setup portal.
//   wifiSsid    - router network to join in normal mode ("" => none, AP only).
//   wifiPass    - router password ("" => open network).
//   leds        - LED bar settings (LedConfig below, see leds.h).
//
// Written from the web server task and read from loop(), so every access goes
// through the accessors below, which hold a mutex.

const int MAX_CONTROLLERS = 5;
const size_t MAX_NICKNAME_LEN = 24;
const int MAX_LEDS = 64;

struct Controller {
  char addr[18];
  char name[MAX_NICKNAME_LEN + 1];
};

void loadConfig();

// Controllers. addController/removeController/renameController return false if
// the address is invalid, unknown, already bound, or the list is full.
int  getControllers(Controller out[MAX_CONTROLLERS]);  // returns count
bool addController(const String &addr, const String &name);
bool removeController(const String &addr);
bool renameController(const String &addr, const String &name);
bool isValidAddr(const String &addr);

String getPassHash();
void   setPassHash(const String &hash);
bool   hasPassword();

bool getForceSetup();
void setForceSetup(bool force);

void getWifi(String &ssid, String &pass);
void setWifi(const String &ssid, const String &pass);

// What the LED bar shows when the PC isn't driving it.
enum LedScene : uint8_t {
  SCENE_OFF     = 0,
  SCENE_COLOR   = 1,  // static colour
  SCENE_RAINBOW = 2,  // only offered while the machine is running
};

struct LedConfig {
  bool     enabled;
  uint8_t  count;       // 1..MAX_LEDS
  uint8_t  brightness;  // output cap, 1..255
  uint8_t  order;       // index into the colour orders in leds.cpp (0 = GRB)
  bool     network;     // accept WLED-compatible UDP/HTTP control from the LAN
  bool     bootAnim;    // animate while the machine boots, until the PC takes over
  uint8_t  offScene;    // machine off: SCENE_OFF or SCENE_COLOR
  uint32_t offColor;    // 0xRRGGBB
  uint8_t  idleScene;   // running, PC not sending: any LedScene
  uint32_t idleColor;
};

LedConfig getLedConfig();
void      setLedConfig(const LedConfig &c);
