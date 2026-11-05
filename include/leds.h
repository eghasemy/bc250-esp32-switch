#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Addressable LED bar (WS2812/NeoPixel) on the configured LED pin, driven
// through the RMT peripheral.
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
//
// Status lights (each can be turned off, see LedSignals) show what the switch
// itself is doing, over the PC's frames and the scenes:
//   button held      -> fills towards force-off (5 s) or setup mode (8 s)
//   setup mode       -> a dot sweeping back and forth
//   controller wake  -> a sweep across the bar
//   machine off      -> fades out
//   boot failed      -> slow red pulses, until the next power-on
//   pairing          -> breathes while the web UI looks for controllers
//   starting         -> fills until the board comes up, then flashes full
//   WiFi lost        -> the end LEDs pulse amber over the local scenes
// While the machine is off the bar is also capped at the standby brightness.

enum class LedPower { Setup, Off, Booting, On };
enum class LedHold { None, ForceOff, Setup };
enum class LedEvent { ControllerWake, ShutDown, BootFailed };

// What loop() tells the bar each iteration.
struct LedInputs {
  LedPower power;
  LedHold  hold;          // a button hold long enough to show
  float    holdProgress;  // 0..1 towards the hold's action
  bool     pairing;       // controller discovery running (normal mode)
  bool     wifiLost;      // router configured but unreachable
};

// LedConfig::order: 0 GRB (WS2812B), 1 RGB, 2 BRG, 3 RBG, 4 GBR, 5 BGR.
const int LED_ORDER_COUNT = 6;

// Call after netBegin(): the UDP listener needs the network stack.
void ledsBegin(bool setupMode);
void ledsLoop(unsigned long now, const LedInputs &in);

// One-off status light triggers, from loop().
void ledsEvent(LedEvent e, unsigned long now);

// Re-read the settings after setLedConfig()/setLedSignals(). Safe from the web
// server task, as are the functions below.
void ledsConfigChanged();

// Shows 0xRRGGBB on every LED for LED_TEST_MS.
void ledsTest(uint32_t rgb);

// {source, wledOn, wifiWarning, pixels: ["rrggbb"..], stream: {active, from, fps, ago}}
void ledsStatusJson(JsonObject out);

// WLED-compatible power: 0 = off, 1 = on, 2 = toggle.
void ledsSetWledOn(int on);
bool ledsWledOn();
bool ledsStreaming();
int  ledsStreamFps();
