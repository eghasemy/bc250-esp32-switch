#pragma once

#include <Arduino.h>

// Persistent configuration, stored in NVS via the Preferences library.
//   controllers - up to MAX_CONTROLLERS bound BLE controllers. Any of them
//                 wakes the machine. addr is lower-case colon form ("aa:bb:..").
//   passHash    - SHA-256 hex of the web UI password ("" => not set yet).
//   forceSetup  - request that the next boot enters the WiFi setup portal.
//   wifiSsid    - router network to join in normal mode ("" => none, AP only).
//   wifiPass    - router password ("" => open network).
//
// Written from the web server task and read from loop(), so every access goes
// through the accessors below, which hold a mutex.

const int MAX_CONTROLLERS = 5;
const size_t MAX_NICKNAME_LEN = 24;

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
