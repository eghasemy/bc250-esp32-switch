#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Event log. logEvent() prints to Serial like Serial.printf() and also keeps the
// line in a small RAM ring buffer that the web UI shows. Use it for events
// (state changes, actions, connections), not for the periodic heartbeat.
// Safe to call from any task.

const int LOG_LINES = 40;

void logEvent(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Appends {t: millis() at the event, msg} objects, oldest first.
void logSnapshot(JsonArray out);
