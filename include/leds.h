#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Addressable LED bar (WS2812/NeoPixel) on LED_PIN, driven through the RMT
// peripheral.
//
// The PC drives it over the network with the WLED realtime UDP protocol on
// LED_UDP_PORT (WARLS, DRGB, DRGBW, DNRGB), which is what the steamos-led-wled
// bridge sends: Steam's Game Mode LED settings end up on the strip. A packet's
// timeout byte says how long its frame holds; after that the bar falls back to
// a local scene that follows the switch's power state:
//   machine off      -> off, or a static glow
//   booting          -> boot animation until the first frame from the PC
//   running, no PC   -> off, a static colour, or a rainbow
// The bridge also switches the bar on/off through WLED's HTTP API (/win&T=);
// "off" holds until it's switched back on or the machine changes power state.

enum class LedPower { Setup, Off, Booting, On };

// LedConfig::order: 0 GRB (WS2812B), 1 RGB, 2 BRG, 3 RBG, 4 GBR, 5 BGR.
const int LED_ORDER_COUNT = 6;

// Call after netBegin(): the UDP listener needs the network stack.
void ledsBegin(bool setupMode);
void ledsLoop(unsigned long now, LedPower power);

// Re-read the settings after setLedConfig(). Safe from the web server task, as
// are the functions below.
void ledsConfigChanged();

// Shows 0xRRGGBB on every LED for LED_TEST_MS.
void ledsTest(uint32_t rgb);

// {source, wledOn, pixels: ["rrggbb"..], stream: {active, from, fps, ago}}
void ledsStatusJson(JsonObject out);

// WLED-compatible power: 0 = off, 1 = on, 2 = toggle.
void ledsSetWledOn(int on);
bool ledsWledOn();
bool ledsStreaming();
int  ledsStreamFps();
