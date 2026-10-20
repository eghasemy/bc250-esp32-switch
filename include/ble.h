#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// BLE scanning. One NimBLE scan serves two jobs:
//   * wake: passive, always on in normal mode. Records when each bound
//     controller was last seen advertising.
//   * discovery: active (so device names arrive), for the web UI's "add
//     controller" picker. Always on in setup mode; on demand in normal mode.
//
// The scan callback runs on the NimBLE task; everything here is safe to call
// from loop() and from the web server task.

void bleBegin(bool setupMode);
void bleLoop(unsigned long now);

// Re-read the bound controllers from config after adding/removing one. A newly
// bound controller is not armed for wake until it has been seen absent once, so
// binding a controller that is switched on doesn't power the machine up.
void bleSyncControllers();

// True if any armed bound controller is advertising right now.
bool bleWakePresent(unsigned long now);

// Bound controllers with nickname, presence, and seconds since last seen.
void bleControllersJson(JsonArray out);

// Discovery (normal mode): runs for DISCOVERY_MS from the last request.
void bleStartDiscovery();
bool bleDiscovering();
void bleDevicesJson(JsonArray out);
