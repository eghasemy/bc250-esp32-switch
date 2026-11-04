#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"

// Which GPIOs each job may use on this chip. The web UI offers only these, and
// the settings are checked against them before they are saved and again at
// boot. Excluded everywhere: the flash pins, the strapping pins (a wrong level
// at reset stops the chip booting) and the serial/USB pins.
//   PS_ON#, button, button ground, LED data: output-capable GPIOs with pull-ups.
//   TPMS1 sense: ADC1 only (ADC2 is unreliable while WiFi is on).

PinConfig defaultPins();

// nullptr if p is usable on this chip, else why not.
const char *pinsError(const PinConfig &p);

// {general: [gpio..], sense: [gpio..]}
void pinOptionsJson(JsonObject out);
