#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// WiFi. Setup mode runs only the open setup AP with a captive DNS. Normal mode
// joins the configured router and serves the web UI there; when no router is
// configured or it stays unreachable for STA_FALLBACK_MS, the same open AP comes
// up as a fallback while the router keeps being retried. The fallback AP goes
// away once the router is back and no client is connected to the AP.

void netBegin(bool setupMode);
void netLoop(unsigned long now);

// Join the router again with the credentials now in config. Safe to call from
// the web server task; applied in netLoop().
void netReconnect();

// {mode, ssid, staConnected, ip, rssi, apActive, apIp, apClients}
void netStatusJson(JsonObject out);

// Short summary for the serial heartbeat, e.g. "sta 192.168.1.20" or "ap".
String netSummary();
