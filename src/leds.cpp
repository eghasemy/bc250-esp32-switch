#include "leds.h"

#include <AsyncUDP.h>
#include "board.h"
#include "config.h"
#include "log.h"

struct RGB {
  uint8_t r, g, b;
};

// Which component goes out first, second, third, per LedConfig::order.
static const uint8_t ORDERS[LED_ORDER_COUNT][3] = {
  {1, 0, 2},  // GRB
  {0, 1, 2},  // RGB
  {2, 0, 1},  // BRG
  {0, 2, 1},  // RBG
  {1, 2, 0},  // GBR
  {2, 1, 0},  // BGR
};

// WS2812 bit timing in 100 ns RMT ticks: a 0 is 0.4 us high + 0.9 us low, a 1
// is 0.8 us high + 0.5 us low.
static const uint32_t RMT_HZ = 10000000;
static const uint16_t T0H = 4, T0L = 9, T1H = 8, T1L = 5;

// Rewrite an unchanged frame this often, in case a glitch corrupted the strip.
static const unsigned long REFRESH_MS = 1000;

static const RGB STEAM_BLUE = {0x1a, 0x9f, 0xff};

enum Source { SRC_DISABLED, SRC_SETUP, SRC_TEST, SRC_DARK, SRC_PC, SRC_OFF,
              SRC_STANDBY, SRC_BOOT, SRC_IDLE, SRC_HOLD_OFF, SRC_HOLD_SETUP,
              SRC_WAKE, SRC_SHUTDOWN, SRC_BOOT_FAIL, SRC_PAIRING, SRC_STARTING };
static const char *const SOURCE_NAMES[] = {
  "disabled", "setup", "test", "dark", "pc", "off", "standby", "boot", "idle",
  "holdOff", "holdSetup", "wake", "shutdown", "bootFail", "pairing", "starting",
};

// --- Settings (owned by loop(); the UDP task gets copies) ---
static LedConfig        cfg = {};
static LedSignals       sig = {};
static int8_t           ledPin = -1;
static volatile bool    cfgDirty   = true;
static volatile bool    netAllowed = false;
static volatile uint8_t ledCount   = 0;
static bool             g_setupMode = false;

static AsyncUDP udp;

// --- Shared with the UDP and web server tasks, under mux ---
static portMUX_TYPE  mux = portMUX_INITIALIZER_UNLOCKED;
static RGB           streamPx[MAX_LEDS];
static bool          streamActive  = false;
static bool          streamForever = false;  // timeout byte 255
static unsigned long streamUntil   = 0;
static unsigned long streamLast    = 0;
static uint32_t      streamFrom    = 0;      // IPv4 of the last sender
static uint32_t      streamFrames  = 0;
static bool          testOn        = false;
static unsigned long testUntil     = 0;
static uint32_t      testColor     = 0;
static RGB           shown[MAX_LEDS];        // current output, before the brightness cap
static volatile Source source = SRC_DISABLED;
static volatile bool   wledOn = true;
static volatile int    fps    = 0;
static volatile bool   wifiWarning = false;

// --- Render state (loop() only) ---
static RGB           fadeFrom[MAX_LEDS];
static unsigned long fadeStart   = 0;
static unsigned long lastFrame   = 0;
static bool          firstLoop   = true;
static LedPower      lastPower   = LedPower::Off;
static unsigned long bootStart   = 0;
static bool          bootDone    = true;   // PC took over (or no boot to animate)
static bool          wasStreaming = false;
static bool          lastWledOn  = true;
static unsigned long fpsAt       = 0;
static uint32_t      fpsFrames   = 0;
static uint16_t      scale       = 0;      // output brightness, eased towards the cap

// --- Status light triggers (loop() only) ---
static bool          wakeOn      = false;
static unsigned long wakeAt      = 0;
static bool          shutdownOn  = false;
static unsigned long shutdownAt  = 0;
static bool          failOn      = false;
static unsigned long failAt      = 0;
static bool          upOn        = false;  // board just came up
static unsigned long upAt        = 0;

// --- Strip output (loop() only) ---
static bool          rmtReady = false;
static rmt_data_t    rmtBuf[MAX_LEDS * 24];
static uint8_t       lastWire[MAX_LEDS * 3];
static int           writtenCount = 0;
static unsigned long lastWrite = 0;

static RGB rgbOf(uint32_t c) {
  return {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
}

static RGB dim(RGB c, float level) {
  return {(uint8_t)(c.r * level), (uint8_t)(c.g * level), (uint8_t)(c.b * level)};
}

// Hue 0..255 -> fully saturated colour.
static RGB wheel(uint8_t h) {
  h = 255 - h;
  if (h < 85) return {(uint8_t)(255 - h * 3), 0, (uint8_t)(h * 3)};
  if (h < 170) {
    h -= 85;
    return {0, (uint8_t)(h * 3), (uint8_t)(255 - h * 3)};
  }
  h -= 170;
  return {(uint8_t)(h * 3), (uint8_t)(255 - h * 3), 0};
}

// Sends n pixels to the strip with the brightness cap and colour order applied.
// Skips the write when the strip already shows exactly this, unless forced.
static void writeStrip(const RGB *px, int n, bool force) {
  if (!rmtReady) return;
  uint8_t wire[MAX_LEDS * 3];
  const uint8_t *ord = ORDERS[cfg.order < LED_ORDER_COUNT ? cfg.order : 0];
  for (int i = 0; i < n; i++) {
    const uint8_t c[3] = {px[i].r, px[i].g, px[i].b};
    for (int k = 0; k < 3; k++) wire[i * 3 + k] = (c[ord[k]] * scale) >> 8;
  }
  unsigned long now = millis();
  if (!force && n == writtenCount && now - lastWrite < REFRESH_MS &&
      memcmp(wire, lastWire, n * 3) == 0) {
    return;
  }

  int k = 0;
  for (int i = 0; i < n * 3; i++) {
    for (int bit = 7; bit >= 0; bit--) {
      bool one = (wire[i] >> bit) & 1;
      rmtBuf[k].level0    = 1;
      rmtBuf[k].duration0 = one ? T1H : T0H;
      rmtBuf[k].level1    = 0;
      rmtBuf[k].duration1 = one ? T1L : T0L;
      k++;
    }
  }
  if (!rmtWrite(ledPin, rmtBuf, k, 20)) return;
  memcpy(lastWire, wire, n * 3);
  writtenCount = n;
  lastWrite = now;
}

static void blankStrip(int n) {
  RGB zero[MAX_LEDS] = {};
  writeStrip(zero, n, true);
}

static void applyConfig() {
  cfgDirty = false;
  LedConfig next = getLedConfig();
  next.count = constrain(next.count, 1, MAX_LEDS);
  sig = getLedSignals();
  sig.standbyBrightness = max<uint8_t>(sig.standbyBrightness, 1);

  // Clear LEDs that will no longer be driven, so they don't keep their colour.
  if (rmtReady && (!next.enabled || next.count < writtenCount)) {
    blankStrip(writtenCount);
  }
  bool wasEnabled = cfg.enabled;
  cfg = next;
  ledCount   = cfg.count;
  netAllowed = cfg.enabled && cfg.network;

  if (cfg.enabled && !rmtReady) {
    rmtReady = rmtInit(ledPin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, RMT_HZ);
    if (!rmtReady) logEvent("[LED ] RMT init failed on GPIO%d", ledPin);
  }
  if (cfg.enabled != wasEnabled) {
    if (cfg.enabled) {
      logEvent("[LED ] bar on: %d LEDs on GPIO%d", cfg.count, ledPin);
    } else {
      logEvent("[LED ] bar disabled");
    }
  }
  if (!cfg.enabled) {
    portENTER_CRITICAL(&mux);
    memset(shown, 0, sizeof(shown));
    source = SRC_DISABLED;
    portEXIT_CRITICAL(&mux);
  }
  lastWrite = 0;  // redraw with the new settings
}

// WLED realtime packets: [protocol, timeout s, data...]. Runs in the UDP task.
static void onPacket(AsyncUDPPacket &packet) {
  if (!netAllowed) return;
  const uint8_t *d = packet.data();
  size_t len = packet.length();
  if (len < 2 || d[0] < 1 || d[0] > 4) return;  // not WARLS/DRGB/DRGBW/DNRGB
  uint8_t proto = d[0], timeout = d[1];
  int n = ledCount;
  uint32_t from = packet.remoteIP();
  unsigned long now = millis();

  portENTER_CRITICAL(&mux);
  if (timeout == 0) {  // "leave realtime mode now"
    streamActive = false;
    portEXIT_CRITICAL(&mux);
    return;
  }
  if (!streamActive) memset(streamPx, 0, sizeof(streamPx));
  switch (proto) {
    case 1:  // WARLS: index, r, g, b
      for (size_t i = 2; i + 4 <= len; i += 4) {
        if (d[i] < n) streamPx[d[i]] = {d[i + 1], d[i + 2], d[i + 3]};
      }
      break;
    case 2:  // DRGB: r, g, b for LED 0, 1, ...
      for (int j = 0; j < n && 2 + j * 3 + 3 <= (int)len; j++) {
        const uint8_t *p = d + 2 + j * 3;
        streamPx[j] = {p[0], p[1], p[2]};
      }
      break;
    case 3:  // DRGBW: white is mixed into all three
      for (int j = 0; j < n && 2 + j * 4 + 4 <= (int)len; j++) {
        const uint8_t *p = d + 2 + j * 4;
        streamPx[j] = {(uint8_t)min(255, p[0] + p[3]), (uint8_t)min(255, p[1] + p[3]),
                       (uint8_t)min(255, p[2] + p[3])};
      }
      break;
    case 4:  // DNRGB: 16-bit start index, then r, g, b
      if (len >= 4) {
        int start = (d[2] << 8) | d[3];
        for (int j = 0; start + j < n && 4 + j * 3 + 3 <= (int)len; j++) {
          const uint8_t *p = d + 4 + j * 3;
          streamPx[start + j] = {p[0], p[1], p[2]};
        }
      }
      break;
  }
  streamActive  = true;
  streamForever = timeout == 255;
  streamUntil   = now + timeout * 1000UL;
  streamLast    = now;
  streamFrom    = from;
  streamFrames++;
  portEXIT_CRITICAL(&mux);
}

// Centre-out sweep in Steam blue over the first 1.2 s, then a slow breath.
static void bootFrame(RGB *px, int n, unsigned long t) {
  const float SWEEP_MS = 1200, BREATH_MS = 2400;
  float half = n / 2.0f;
  for (int i = 0; i < n; i++) {
    float level;
    if (t < SWEEP_MS) {
      float dist = fabsf(i + 0.5f - half) / half;  // 0 centre .. 1 ends
      level = constrain((t / SWEEP_MS * 1.25f - dist) * 4, 0.0f, 1.0f);
    } else {
      level = 0.35f + 0.65f * (0.5f + 0.5f * cosf((t - SWEEP_MS) * 2 * PI / BREATH_MS));
    }
    px[i] = dim(STEAM_BLUE, level);
  }
}

static void fill(RGB *px, int n, RGB c) {
  for (int i = 0; i < n; i++) px[i] = c;
}

// 0 -> 1 -> 0 over period ms, smooth.
static float wave(unsigned long t, float period) {
  return 0.5f - 0.5f * cosf(t * 2 * PI / period);
}

// Progress bar: the first p of the bar lit, the rest dimly so the track shows.
static void progressFrame(RGB *px, int n, RGB c, float p) {
  float lit = constrain(p, 0.0f, 1.0f) * n;
  for (int i = 0; i < n; i++) {
    px[i] = dim(c, max(0.08f, constrain(lit - i, 0.0f, 1.0f)));
  }
}

// One bright head with a short tail, crossing the bar left to right.
static void sweepFrame(RGB *px, int n, RGB c, unsigned long t) {
  const float TAIL = 4;
  float head = t / (float)LED_WAKE_MS * (n + TAIL);
  for (int i = 0; i < n; i++) {
    float d = head - i;
    float level = d >= 0 && d < TAIL ? 1 - d / TAIL : 0;
    px[i] = dim(c, level * level);
  }
}

// A dot travelling back and forth, over a faint bar.
static void scannerFrame(RGB *px, int n, RGB c, unsigned long t) {
  const float PERIOD = 2400;
  float x = fmodf(t, PERIOD) / PERIOD * 2;  // 0..2
  float pos = (x < 1 ? x : 2 - x) * (n - 1);
  for (int i = 0; i < n; i++) {
    float level = max(0.0f, 1 - fabsf(i - pos) / 2.5f);
    px[i] = dim(c, max(0.06f, level * level));
  }
}

// Status lights that show over the PC and the scenes, in priority order.
static Source statusSource(const LedInputs &in) {
  if (in.hold == LedHold::ForceOff && sig.on[SIG_FORCE_OFF]) return SRC_HOLD_OFF;
  if (in.hold == LedHold::Setup && sig.on[SIG_SETUP])        return SRC_HOLD_SETUP;
  if (g_setupMode)                                           return SRC_SETUP;
  if (wakeOn && sig.on[SIG_WAKE])                            return SRC_WAKE;
  if (shutdownOn && sig.on[SIG_SHUTDOWN])                    return SRC_SHUTDOWN;
  if (failOn && sig.on[SIG_BOOT_FAIL])                       return SRC_BOOT_FAIL;
  return SRC_DISABLED;  // none
}

void ledsBegin(bool setupMode) {
  g_setupMode = setupMode;
  ledPin = getPins().led;  // pins are fixed until the next restart
  if (setupMode) return;
  udp.onPacket(onPacket);
  if (!udp.listen(LED_UDP_PORT)) {
    logEvent("[LED ] can't listen on UDP %u", LED_UDP_PORT);
  }
}

void ledsEvent(LedEvent e, unsigned long now) {
  switch (e) {
    case LedEvent::ControllerWake: wakeOn = true;     wakeAt = now;     break;
    case LedEvent::ShutDown:       shutdownOn = true; shutdownAt = now; break;
    case LedEvent::BootFailed:
      shutdownOn = false;  // the failure says more than the fade
      failOn = true;
      failAt = now;
      break;
  }
}

void ledsLoop(unsigned long now, const LedInputs &in) {
  if (cfgDirty) applyConfig();
  LedPower power = in.power;

  // Triggers expire whether or not the bar is on.
  if (wakeOn && now - wakeAt >= LED_WAKE_MS) wakeOn = false;
  if (shutdownOn && (now - shutdownAt >= LED_SHUTDOWN_MS || power != LedPower::Off)) shutdownOn = false;
  if (failOn && (now - failAt >= LED_BOOT_FAIL_MS || power != LedPower::Off)) failOn = false;
  if (upOn && now - upAt >= LED_BOARD_UP_MS) upOn = false;

  // Power changes are tracked whether or not the bar is on. A fresh boot plays
  // the boot animation; adopting a machine that was already running when the
  // ESP started doesn't. Any power change undoes a WLED "off".
  if (firstLoop) {
    firstLoop = false;
    lastPower = power;
  } else if (power != lastPower) {
    if ((power == LedPower::Booting || power == LedPower::On) &&
        (lastPower == LedPower::Off || lastPower == LedPower::Setup)) {
      bootStart = now;
      bootDone = false;
    }
    if (power == LedPower::On && lastPower == LedPower::Booting) {
      upOn = true;
      upAt = now;
    }
    wledOn = true;
    lastPower = power;
  }
  if (!cfg.enabled) return;

  if (now - lastFrame < LED_FRAME_MS) return;
  lastFrame = now;
  int n = cfg.count;

  // Snapshot what the other tasks set.
  RGB frame[MAX_LEDS];
  bool streaming, testing;
  uint32_t from, frames, tColor;
  portENTER_CRITICAL(&mux);
  if (streamActive && !streamForever && (int32_t)(now - streamUntil) >= 0) {
    streamActive = false;
  }
  streaming = streamActive;
  if (streaming) memcpy(frame, streamPx, sizeof(RGB) * n);
  testing = testOn && (int32_t)(testUntil - now) > 0;
  if (!testing) testOn = false;
  tColor = testColor;
  from   = streamFrom;
  frames = streamFrames;
  portEXIT_CRITICAL(&mux);

  if (now - fpsAt >= 1000) {
    fps = frames - fpsFrames;
    fpsFrames = frames;
    fpsAt = now;
  }
  if (streaming != wasStreaming) {
    wasStreaming = streaming;
    if (streaming) {
      logEvent("[LED ] PC took over the bar (%s)", IPAddress(from).toString().c_str());
    } else {
      logEvent("[LED ] PC stopped sending");
    }
  }
  bool on = wledOn;
  if (on != lastWledOn) {
    lastWledOn = on;
    logEvent("[LED ] bar switched %s over the network", on ? "on" : "off");
  }
  if (streaming) bootDone = true;

  bool off = power == LedPower::Off || power == LedPower::Setup;
  Source src = statusSource(in);
  if (testing)                         src = SRC_TEST;
  else if (src != SRC_DISABLED)        {}  // a status light
  else if (!on)                        src = SRC_DARK;
  else if (in.pairing && sig.on[SIG_PAIRING]) src = SRC_PAIRING;
  else if (streaming)                  src = SRC_PC;
  else if (sig.on[SIG_STARTING] && (power == LedPower::Booting || upOn)) src = SRC_STARTING;
  else if (off)                        src = cfg.offScene == SCENE_COLOR ? SRC_STANDBY : SRC_OFF;
  else if (cfg.bootAnim && !bootDone && now - bootStart < LED_BOOT_ANIM_MAX_MS) src = SRC_BOOT;
  else                                 src = SRC_IDLE;

  RGB target[MAX_LEDS] = {};
  switch (src) {
    case SRC_TEST:    fill(target, n, rgbOf(tColor)); break;
    case SRC_PC:      memcpy(target, frame, sizeof(RGB) * n); break;
    case SRC_STANDBY: fill(target, n, rgbOf(cfg.offColor)); break;
    case SRC_BOOT:    bootFrame(target, n, now - bootStart); break;
    case SRC_IDLE:
      if (cfg.idleScene == SCENE_COLOR) {
        fill(target, n, rgbOf(cfg.idleColor));
      } else if (cfg.idleScene == SCENE_RAINBOW) {
        for (int i = 0; i < n; i++) target[i] = wheel(now / 20 + i * 256 / n);
      }
      break;
    case SRC_HOLD_OFF:
      progressFrame(target, n, rgbOf(sig.color[SIG_FORCE_OFF]), in.holdProgress);
      break;
    case SRC_HOLD_SETUP:
      progressFrame(target, n, rgbOf(sig.color[SIG_SETUP]), in.holdProgress);
      break;
    case SRC_SETUP:
      if (sig.on[SIG_SETUP]) scannerFrame(target, n, rgbOf(sig.color[SIG_SETUP]), now);
      break;
    case SRC_WAKE:
      sweepFrame(target, n, rgbOf(sig.color[SIG_WAKE]), now - wakeAt);
      break;
    case SRC_SHUTDOWN: {
      float left = 1 - (now - shutdownAt) / (float)LED_SHUTDOWN_MS;
      fill(target, n, dim(rgbOf(sig.color[SIG_SHUTDOWN]), left * left));
      break;
    }
    case SRC_BOOT_FAIL: {
      float w = wave(now - failAt, 1600);
      fill(target, n, dim(rgbOf(sig.color[SIG_BOOT_FAIL]), 0.08f + 0.92f * w * w));
      break;
    }
    case SRC_PAIRING:
      fill(target, n, dim(rgbOf(sig.color[SIG_PAIRING]), 0.15f + 0.85f * wave(now, 2000)));
      break;
    case SRC_STARTING:
      if (upOn) {
        fill(target, n, rgbOf(sig.color[SIG_STARTING]));
      } else {
        progressFrame(target, n, rgbOf(sig.color[SIG_STARTING]),
                      (now - bootStart) / (float)BOOT_TIMEOUT_MS);
      }
      break;
    default: break;  // dark
  }

  // Crossfade into a new source.
  if (src != source) {
    portENTER_CRITICAL(&mux);
    memcpy(fadeFrom, shown, sizeof(fadeFrom));
    portEXIT_CRITICAL(&mux);
    fadeStart = now;
  }
  unsigned long t = now - fadeStart;
  uint32_t a = t >= LED_FADE_MS ? 256 : t * 256 / LED_FADE_MS;
  RGB out[MAX_LEDS];
  for (int i = 0; i < n; i++) {
    out[i].r = fadeFrom[i].r + (((int)target[i].r - fadeFrom[i].r) * (int)a >> 8);
    out[i].g = fadeFrom[i].g + (((int)target[i].g - fadeFrom[i].g) * (int)a >> 8);
    out[i].b = fadeFrom[i].b + (((int)target[i].b - fadeFrom[i].b) * (int)a >> 8);
  }

  // WiFi trouble: the end LEDs pulse over the local scenes (not over the PC,
  // other status lights, or a bar the PC switched off).
  bool warn = in.wifiLost && sig.on[SIG_WIFI] &&
              (src == SRC_OFF || src == SRC_STANDBY || src == SRC_BOOT || src == SRC_IDLE);
  wifiWarning = warn;
  if (warn) {
    RGB c = dim(rgbOf(sig.color[SIG_WIFI]), 0.15f + 0.85f * wave(now, 2000));
    out[0] = c;
    out[n - 1] = c;
  }

  // Brightness cap: the standby one while the machine is off. Eased so a power
  // change doesn't jump.
  uint16_t cap = (off ? min(cfg.brightness, sig.standbyBrightness) : cfg.brightness) + 1;
  const uint16_t STEP = 256 * LED_FRAME_MS / LED_FADE_MS + 1;
  if (scale == 0) scale = cap;  // first frame
  else if (scale < cap) scale = min<uint16_t>(cap, scale + STEP);
  else if (scale > cap) scale = max<uint16_t>(cap, scale - STEP);

  portENTER_CRITICAL(&mux);
  memcpy(shown, out, sizeof(RGB) * n);
  source = src;
  portEXIT_CRITICAL(&mux);
  writeStrip(out, n, false);
}

void ledsConfigChanged() {
  cfgDirty = true;
}

void ledsTest(uint32_t rgb) {
  portENTER_CRITICAL(&mux);
  testColor = rgb;
  testUntil = millis() + LED_TEST_MS;
  testOn = true;
  portEXIT_CRITICAL(&mux);
}

void ledsSetWledOn(int on) {
  wledOn = on == 2 ? !wledOn : on != 0;
}

bool ledsWledOn() {
  return wledOn;
}

bool ledsStreaming() {
  return source == SRC_PC;
}

int ledsStreamFps() {
  return fps;
}

void ledsStatusJson(JsonObject out) {
  RGB px[MAX_LEDS];
  int n = ledCount;
  Source src;
  bool active;
  uint32_t from;
  unsigned long last;
  portENTER_CRITICAL(&mux);
  memcpy(px, shown, sizeof(px));
  src    = source;
  active = streamActive;
  from   = streamFrom;
  last   = streamLast;
  portEXIT_CRITICAL(&mux);

  out["source"] = SOURCE_NAMES[src];
  out["wledOn"] = (bool)wledOn;
  out["wifiWarning"] = (bool)wifiWarning;
  JsonArray pixels = out["pixels"].to<JsonArray>();
  if (src != SRC_DISABLED) {
    for (int i = 0; i < n; i++) {
      char hex[7];
      snprintf(hex, sizeof(hex), "%02x%02x%02x", px[i].r, px[i].g, px[i].b);
      pixels.add(hex);
    }
  }
  JsonObject stream = out["stream"].to<JsonObject>();
  stream["active"] = active;
  stream["fps"]    = active ? (int)fps : 0;
  if (from) {
    stream["from"] = IPAddress(from).toString();
    stream["ago"]  = millis() - last;
  }
}
