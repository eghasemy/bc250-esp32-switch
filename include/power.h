#pragma once

#include <Arduino.h>

// Power state machine (main.cpp) as seen from the web server task. Requests are
// queued and acted on by loop(); the status is a snapshot loop() publishes.

struct PowerStatus {
  const char   *state;          // "OFF" | "BOOTING" | "ON" | "SETUP"
  bool          psuOn;          // PS_ON# asserted
  bool          boardUp;        // debounced TPMS1
  uint32_t      senseMv;        // last TPMS1 reading
  bool          buttonDown;
  unsigned long wakeInhibitMs;  // remaining BLE wake guard window, 0 if none
};

PowerStatus powerStatus();

// Return false (and do nothing) when the request doesn't apply right now:
// power on only from OFF, force off only from BOOTING/ON, neither in setup mode.
bool requestPowerOn();
bool requestForceOff();
